// px4rec.cpp - TS recording client for DriverHost_PX4 on macOS
//
// Usage:
//   px4rec [options] T|S|C <channel>
//   px4rec [options] T|S|C <channel> <output_file>
//
// Options:
//   -i N   Tuner index (0-based; default: -1 = first available)
//   -d N   Duration in seconds (default: run until SIGTERM/SIGINT)
//
// Channel encoding:
//   T (ISDB-T): physical UHF channel number  13 – 62
//   S (ISDB-S): transponder number  0–11 = BS,  12–23 = CS110
//   C (ISDB-C): CATV channel number 13–63 (experimental)
//
// Output:
//   File path, or '-' (or omitted) for stdout.
//
// Sockets are looked up in $PX4_RUNTIME_DIR (default /tmp), the same place
// DriverHost_PX4 creates them.
//
// This tool connects to a running DriverHost_PX4 daemon via UNIX domain
// sockets, opens a receiver, tunes to the requested channel, and streams
// raw MPEG-2 TS to the output.  It is intended to be used as a "tuner
// command" for mirakurun / Chinachu / EPGStation on macOS.
//
// mirakurun config example:
//   tuners:
//     - name: PX4-T1
//       types: [GR]
//       command: px4rec T <ch>

#ifdef msg_prefix
#undef msg_prefix
#endif
#define msg_prefix "px4rec"

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <ctime>
#include <cerrno>

#include <mach-o/dyld.h>

#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <spawn.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

extern char **environ;

// Pull in the shared command protocol (relative paths resolved by -I flags)
#include "command.hpp"      // px4::command::*
#include "card_command.hpp" // px4::card_command::*
#include "misc_posix.h"     // GUID, HANDLE stubs, wchar_t helpers
#include "runtime_path.hpp" // px4::runtime_path

// B25 decoder (ARIB STD-B25 / B-CAS)
extern "C" {
#include "arib_std_b25.h"
#include "b_cas_card.h"
#include "b_cas_card_error_code.h"
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Same location rule as DriverHost_PX4: $PX4_RUNTIME_DIR, else /tmp.
static const char *ctrl_sock()
{
	static const std::string path = px4::runtime_path("px4_ctrl_pipe.sock");
	return path.c_str();
}

static const char *data_sock()
{
	static const std::string path = px4::runtime_path("px4_data_pipe.sock");
	return path.c_str();
}

static const char *card_sock()
{
	static const std::string path = px4::runtime_path("px4_card_pipe.sock");
	return path.c_str();
}

static volatile sig_atomic_t g_quit = 0;
static void sig_handler(int) { g_quit = 1; }

static int unix_connect(const char *path);

static bool get_executable_dir(char *buf, std::size_t size)
{
	uint32_t path_size = 0;
	_NSGetExecutablePath(nullptr, &path_size);
	if (path_size == 0)
		return false;

	auto *path = static_cast<char *>(malloc(path_size));
	if (!path)
		return false;

	if (_NSGetExecutablePath(path, &path_size) != 0) {
		free(path);
		return false;
	}

	char resolved[PATH_MAX];
	if (!realpath(path, resolved)) {
		free(path);
		return false;
	}
	free(path);

	char dirbuf[PATH_MAX];
	strncpy(dirbuf, resolved, sizeof(dirbuf) - 1);
	dirbuf[sizeof(dirbuf) - 1] = '\0';

	char *dir = dirname(dirbuf);
	if (!dir)
		return false;

	if (strlcpy(buf, dir, size) >= size) {
		errno = ENAMETOOLONG;
		return false;
	}

	return true;
}

static bool start_driver_host_if_needed()
{
	char exe_dir[PATH_MAX];
	if (!get_executable_dir(exe_dir, sizeof(exe_dir)))
		return false;

	char driver_path[PATH_MAX];
	if (snprintf(driver_path, sizeof(driver_path), "%s/DriverHost_PX4", exe_dir) >=
	    static_cast<int>(sizeof(driver_path))) {
		errno = ENAMETOOLONG;
		return false;
	}

	if (access(driver_path, X_OK) != 0)
		return false;

	pid_t pid = fork();
	if (pid < 0)
		return false;

	if (pid == 0) {
		setsid();
		char *const argv[] = { driver_path, nullptr };
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			posix_spawn_file_actions_adddup2(&actions, devnull, STDIN_FILENO);
			posix_spawn_file_actions_adddup2(&actions, devnull, STDOUT_FILENO);
			posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				posix_spawn_file_actions_addclose(&actions, devnull);
		}
		(void)posix_spawn(nullptr, driver_path, &actions, nullptr, argv, environ);
		posix_spawn_file_actions_destroy(&actions);
		if (devnull >= 0)
			close(devnull);
		_exit(0);
	}

	return true;
}

static int connect_ctrl_socket(bool *started_driver_host)
{
	int fd = unix_connect(ctrl_sock());
	if (fd >= 0)
		return fd;

	if (started_driver_host)
		*started_driver_host = false;

	if (errno != ENOENT && errno != ECONNREFUSED)
		return -1;

	if (!start_driver_host_if_needed())
		return -1;

	if (started_driver_host)
		*started_driver_host = true;

	for (int i = 0; i < 50; ++i) {
		usleep(100000);
		fd = unix_connect(ctrl_sock());
		if (fd >= 0)
			return fd;
		if (errno != ENOENT && errno != ECONNREFUSED)
			break;
	}

	return -1;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static int unix_connect(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr),
		    sizeof(addr)) < 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static bool send_all(int fd, const void *buf, std::size_t size)
{
	const char *p = static_cast<const char *>(buf);
	while (size > 0) {
		ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
		if (n <= 0) return false;
		p    += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

static bool recv_all(int fd, void *buf, std::size_t size)
{
	char *p = static_cast<char *>(buf);
	while (size > 0) {
		ssize_t n = recv(fd, p, size, 0);
		if (n <= 0) return false;
		p    += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

static int g_internal_card_fd = -1;
static wchar_t g_internal_card_reader[px4::card_command::MAX_READER_NAME] = {};

static void close_internal_card_socket()
{
	if (g_internal_card_fd >= 0)
		close(g_internal_card_fd);
	g_internal_card_fd = -1;
	g_internal_card_reader[0] = L'\0';
}

static bool exchange_card_command(px4::card_command::Command &command)
{
	if (g_internal_card_fd < 0)
		return false;
	/* 送受信の途中で切れた接続は応答の境界を失うため、次回は接続し直す。 */
	if (!send_all(g_internal_card_fd, &command, sizeof(command)) ||
	    !recv_all(g_internal_card_fd, &command, sizeof(command))) {
		close_internal_card_socket();
		return false;
	}
	return command.status == px4::card_command::Status::SUCCEEDED;
}

static bool list_internal_card_reader(char *name, std::size_t capacity)
{
	if (g_internal_card_fd < 0)
		g_internal_card_fd = unix_connect(card_sock());
	if (g_internal_card_fd < 0)
		return false;

	px4::card_command::Command command{};
	command.code = px4::card_command::Code::LIST_READERS;
	if (!exchange_card_command(command) || !command.reader_count)
		return false;

	/* 内蔵カードリーダーを持つ機器を複数同時に接続した場合に備え、カードが挿さった
	 * リーダーを優先する。どれにもなければ従来どおり先頭を使う。
	 * サーバーは STATUS で開いたカードを、その接続が閉じるまで開いたままにする。
	 * 録画用の接続で全リーダーを調べると、使わない機器の電源も録画終了まで
	 * 入ったままになるため、別の接続で調べて選択後に閉じる。 */
	const std::uint32_t reader_count = std::min<std::uint32_t>(command.reader_count,
		static_cast<std::uint32_t>(px4::card_command::MAX_READERS));
	wchar_t readers[px4::card_command::MAX_READERS][px4::card_command::MAX_READER_NAME] = {};
	std::memcpy(readers, command.readers, sizeof(readers));
	std::uint32_t selected = 0;
	if (reader_count > 1) {
		const int probe_fd = unix_connect(card_sock());
		for (std::uint32_t i = 0; probe_fd >= 0 && i < reader_count; ++i) {
			px4::card_command::Command status{};
			status.code = px4::card_command::Code::STATUS;
			std::wmemcpy(status.reader_name, readers[i], px4::card_command::MAX_READER_NAME);
			status.reader_name[px4::card_command::MAX_READER_NAME - 1] = L'\0';
			if (!send_all(probe_fd, &status, sizeof(status)) ||
			    !recv_all(probe_fd, &status, sizeof(status)))
				break;
			if (status.status == px4::card_command::Status::SUCCEEDED && status.card_present) {
				selected = i;
				break;
			}
		}
		if (probe_fd >= 0)
			close(probe_fd);
	}

	const wchar_t *reader = readers[selected];
	std::size_t n = 0;
	for (; reader[n] && n + 1 < capacity; ++n) {
		if (reader[n] < 0 || reader[n] > 0x7f)
			return false;
		name[n] = static_cast<char>(reader[n]);
	}
	if (reader[n] || !capacity)
		return false;
	name[n] = '\0';
	std::wmemcpy(g_internal_card_reader, reader, n + 1);
	return true;
}

extern "C" int px4_internal_card_list(char *name, std::size_t capacity)
{
	return name && list_internal_card_reader(name, capacity) ? 1 : 0;
}

/* 1: 接続済み、0: カード未挿入、-1: リーダー不一致・通信失敗・共有違反など。 */
extern "C" int px4_internal_card_connect(const char *reader_name)
{
	char current_name[px4::card_command::MAX_READER_NAME] = {};
	if (g_internal_card_fd < 0 || g_internal_card_reader[0] == L'\0') {
		if (!list_internal_card_reader(current_name, sizeof(current_name)))
			return -1;
	} else {
		for (std::size_t i = 0; i + 1 < sizeof(current_name) && g_internal_card_reader[i]; ++i)
			current_name[i] = static_cast<char>(g_internal_card_reader[i]);
	}

	if (!reader_name || std::strcmp(reader_name, current_name))
		return -1;

	px4::card_command::Command command{};
	command.code = px4::card_command::Code::CONNECT;
	command.share_mode = static_cast<std::uint32_t>(px4::card_command::ShareMode::SHARED);
	std::wmemcpy(command.reader_name, g_internal_card_reader,
		px4::card_command::MAX_READER_NAME);
	if (!exchange_card_command(command))
		return command.error == px4::card_command::Error::NO_CARD ? 0 : -1;

	command = {};
	command.code = px4::card_command::Code::STATUS;
	std::wmemcpy(command.reader_name, g_internal_card_reader,
		px4::card_command::MAX_READER_NAME);
	const bool status_ok = exchange_card_command(command);
	if (!status_ok || !command.card_present) {
		const int result = status_ok ? 0 : -1;
		command = {};
		command.code = px4::card_command::Code::DISCONNECT;
		(void)exchange_card_command(command);
		return result;
	}
	return 1;
}

/* close_connection=0 は SCardDisconnect(SCARD_RESET_CARD) に相当し、再接続前に
 * カードをリセットして T=1 の連番などの状態を破棄する。1 は終了処理。 */
extern "C" void px4_internal_card_disconnect(int close_connection)
{
	if (g_internal_card_fd < 0)
		return;
	px4::card_command::Command command{};
	if (!close_connection) {
		command.code = px4::card_command::Code::RESET;
		(void)exchange_card_command(command);
		command = {};
	}
	command.code = px4::card_command::Code::DISCONNECT;
	(void)exchange_card_command(command);
	if (close_connection)
		close_internal_card_socket();
}

extern "C" int px4_internal_card_transmit(const std::uint8_t *send_buffer,
	std::uint32_t send_length, std::uint8_t *receive_buffer,
	std::uint32_t *receive_length)
{
	if (!send_buffer || !receive_buffer || !receive_length ||
		send_length > px4::card_command::MAX_DATA_SIZE)
		return 0;
	px4::card_command::Command command{};
	command.code = px4::card_command::Code::TRANSMIT;
	command.data_length = send_length;
	command.data_capacity = static_cast<std::uint32_t>(
		std::min<std::size_t>(*receive_length, px4::card_command::MAX_DATA_SIZE));
	std::memcpy(command.data, send_buffer, send_length);
	if (!exchange_card_command(command))
		return 0;
	if (command.data_length > command.data_capacity)
		return 0;
	std::memcpy(receive_buffer, command.data, command.data_length);
	*receive_length = command.data_length;
	return 1;
}

// ---------------------------------------------------------------------------
// Channel → frequency (kHz) conversion
//
// Formulas from driver/ptx_chrdev.c (PTX_SET_CHANNEL handler):
//   ISDB-T UHF ch13-62 (PTX internal ID = ch+50, range 63-112):
//     freq_kHz = 95143 + ptx_id * 6000
//              = 95143 + (physical_ch + 50) * 6000
//              = 395143 + physical_ch * 6000
//
//   ISDB-S BS  (transponder 0-11):
//     freq_kHz = 1049480 + 38360 * tp
//
//   ISDB-S CS110 (transponder 0-11, logical 12-23):
//     freq_kHz = 1613000 + 40000 * tp
// ---------------------------------------------------------------------------

static uint32_t isdb_t_freq_kHz(int ch)
{
	return 395143u + static_cast<uint32_t>(ch) * 6000u;
}

static uint32_t isdb_s_bs_freq_kHz(int tp)
{
	return 1049480u + 38360u * static_cast<uint32_t>(tp);
}

static uint32_t isdb_s_cs_freq_kHz(int tp)
{
	return 1613000u + 40000u * static_cast<uint32_t>(tp);
}

// ---------------------------------------------------------------------------
// Cleanup on exit
// ---------------------------------------------------------------------------

static bool close_session(int ctrl_fd, int &data_fd);

static void send_close(int ctrl_fd)
{
	int data_fd = -1;
	(void)close_session(ctrl_fd, data_fd);
}

static bool open_session(int ctrl_fd, px4::SystemType sys_type, int tuner_idx,
	const char *sys_str, int &data_fd)
{
	// ---- OPEN --------------------------------------------------------------
	// CtrlOpenCmd contains ReceiverInfo with wchar_t[96] fields.
	// On macOS wchar_t = 4 bytes, so the struct is ~824 bytes total.
	// Use a heap-allocated, zero-initialised buffer to avoid large stack frames.
	uint32_t data_id = 0;
	{
		const std::size_t cmd_size = sizeof(px4::command::CtrlOpenCmd);
		auto *open_cmd = static_cast<px4::command::CtrlOpenCmd *>(
			calloc(1, cmd_size));
		if (!open_cmd) {
			fprintf(stderr, "Error: out of memory\n");
			return false;
		}
		open_cmd->cmd                   = px4::command::CtrlCmdCode::OPEN;
		open_cmd->status                = px4::command::CtrlStatusCode::NONE;
		open_cmd->receiver_info.systems = sys_type;
		open_cmd->receiver_info.index   = tuner_idx; // -1 = any

		bool ok = send_all(ctrl_fd, open_cmd, cmd_size) &&
			  recv_all(ctrl_fd, open_cmd, cmd_size);
		if (!ok ||
		    open_cmd->status != px4::command::CtrlStatusCode::SUCCEEDED) {
			fprintf(stderr,
				"Error: OPEN failed – no %s receiver available"
				" (index %d)\n", sys_str, tuner_idx);
			free(open_cmd);
			return false;
		}
		data_id = open_cmd->receiver_info.data_id;
		free(open_cmd);
	}

	// OPEN で ID は有効になる。読み手は受信開始を待てるため、キャプチャより先に接続する。
	// ---- Open data socket and send SET_DATA_ID -----------------------------
	data_fd = unix_connect(data_sock());
	if (data_fd < 0) {
		fprintf(stderr, "Error: cannot connect to %s: %s\n",
			data_sock(), strerror(errno));
		send_close(ctrl_fd);
		return false;
	}
	{
		px4::command::DataCmd dcmd{};
		dcmd.cmd     = px4::command::DataCmdCode::SET_DATA_ID;
		dcmd.data_id = data_id;
		if (!send_all(data_fd, &dcmd, sizeof(dcmd))) {
			fprintf(stderr, "Error: SET_DATA_ID failed\n");
			close(data_fd);
			data_fd = -1;
			send_close(ctrl_fd);
			return false;
		}
	}

	return true;
}

static bool tune_session(int ctrl_fd, px4::SystemType sys_type, uint32_t freq_kHz,
	bool is_c, int64_t tsmf, const char *sys_str, int channel)
{
	// ---- SET_PARAMS --------------------------------------------------------
	{
		const uint32_t num = is_c ? 2 : 0;
		// params[1] の末尾を拡張し、送受信とも同じ長さを使う。T/S の固定長は維持する。
		const std::size_t size = sizeof(px4::command::CtrlParamsCmd) +
			(num > 1 ? (num - 1) * sizeof(px4::command::Parameter) : 0);
		auto *cmd = static_cast<px4::command::CtrlParamsCmd *>(calloc(1, size));
		bool ok = false;
		if (cmd) {
			cmd->cmd = px4::command::CtrlCmdCode::SET_PARAMS;
			cmd->status = px4::command::CtrlStatusCode::NONE;
			cmd->param_set.system = sys_type;
			cmd->param_set.freq = freq_kHz;
			cmd->param_set.num = num;
			if (is_c) {
				using P = px4::command::ParameterType;
				/* TSMF の TS 番号は TS0 が既定。ヘッダーのない搬送波は TS0 の指定でそのまま出力される。 */
				const px4::command::Parameter params[] = {
					{P::BANDWIDTH, 6},
					{P::TSMF_STREAM, static_cast<uint32_t>(tsmf < 0 ? 0 : tsmf)},
				};
				memcpy(cmd->param_set.params, params, num * sizeof(params[0]));
			}
			ok = send_all(ctrl_fd, cmd, size) && recv_all(ctrl_fd, cmd, size) &&
				cmd->status == px4::command::CtrlStatusCode::SUCCEEDED;
			free(cmd);
		}
		if (!ok) {
			fprintf(stderr, "Error: SET_PARAMS failed\n");
			return false;
		}
	}

	// ---- TUNE --------------------------------------------------------------
	{
		px4::command::CtrlTuneCmd cmd{};
		cmd.cmd     = px4::command::CtrlCmdCode::TUNE;
		cmd.status  = px4::command::CtrlStatusCode::NONE;
		cmd.timeout = 30000; // 30 s
		if (!send_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    !recv_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    cmd.status != px4::command::CtrlStatusCode::SUCCEEDED) {
			fprintf(stderr, "Error: TUNE failed (no lock on %s ch%d)\n",
				sys_str, channel);
			return false;
		}
	}

	return true;
}

static bool set_capture(int ctrl_fd, bool capture)
{
	px4::command::CtrlCaptureCmd cmd{};
	cmd.cmd = px4::command::CtrlCmdCode::SET_CAPTURE;
	cmd.status = px4::command::CtrlStatusCode::NONE;
	cmd.capture = capture;
	if (send_all(ctrl_fd, &cmd, sizeof(cmd)) &&
	    recv_all(ctrl_fd, &cmd, sizeof(cmd)) &&
	    cmd.status == px4::command::CtrlStatusCode::SUCCEEDED)
		return true;
	fprintf(stderr, "Error: SET_CAPTURE(%s) failed\n", capture ? "true" : "false");
	return false;
}

// 停止応答を待ってデータ接続を切り、CLOSE 応答で受信機と data_id の解放を確認する。
static bool close_session(int ctrl_fd, int &data_fd)
{
	bool ok = set_capture(ctrl_fd, false);
	if (data_fd >= 0) close(data_fd);
	data_fd = -1;
	px4::command::CtrlCloseCmd cmd{};
	cmd.cmd = px4::command::CtrlCmdCode::CLOSE;
	cmd.status = px4::command::CtrlStatusCode::NONE;
	if (!send_all(ctrl_fd, &cmd, sizeof(cmd)) ||
	    !recv_all(ctrl_fd, &cmd, sizeof(cmd)) ||
	    cmd.status != px4::command::CtrlStatusCode::SUCCEEDED) {
		fprintf(stderr, "Error: CLOSE failed\n");
		ok = false;
	}
	return ok;
}

static void usage()
{
	fprintf(stderr,
		"Usage: px4rec [options] T|S|C <channel> [output|-]\n"
		"  -i N  tuner index; -d N  duration seconds; --b25  B-CAS decode\n"
		"  T  ISDB-T physical channel 13-62\n"
		"  S  ISDB-S transponder 0-11=BS, 12-23=CS110\n"
		"  C  ISDB-C CATV channel 13-63 (experimental)\n"
		"  C only: --freq-khz <1-4294967295> --freq-offset <signed kHz>\n"
		"          --tsmf <0-14> (TSMF TS number, default: 0)\n");
}

// 数値の欠落・桁あふれを選局前に検出し、誤った周波数で受信を開始しない。
static bool parse_number(const char *text, int64_t min, int64_t max, int64_t &value)
{
	char *end = nullptr;
	errno = 0;
	value = strtoll(text, &end, 10);
	return text[0] && end != text && !*end && errno != ERANGE &&
		value >= min && value <= max;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
	int         tuner_idx  = -1;    // -1 → first available
	int         duration   = -1;    // -1 → run until signal
	bool        use_b25    = false;
	bool        output_seen = false;
	const char *sys_str    = nullptr;
	int         channel    = -1;
	const char *outfile    = "-";   // stdout

	bool c_options = false, has_freq = false;
	int64_t direct_freq = 0, freq_offset = 0, tsmf = -1;

	// Parse arguments
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage();
			return 0;
		} else if (!strcmp(argv[i], "--freq-khz") || !strcmp(argv[i], "--freq-offset") ||
			   !strcmp(argv[i], "--tsmf")) {
			const char *option = argv[i];
			int64_t *value = &freq_offset, min = INT32_MIN, max = INT32_MAX;
			c_options = true;
			if (!strcmp(option, "--freq-khz")) {
				value = &direct_freq; min = 1; max = UINT32_MAX; has_freq = true;
			} else if (!strcmp(option, "--tsmf")) {
				value = &tsmf; min = 0; max = 14;
			}
			if (i + 1 >= argc || !parse_number(argv[++i], min, max, *value)) {
				fprintf(stderr, "Error: invalid value for %s\n", option);
				return 1;
			}
		} else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "-d")) {
			const bool is_index = !strcmp(argv[i], "-i");
			const char *option = argv[i];
			int64_t value;
			if (i + 1 >= argc || !parse_number(argv[++i], -1, INT_MAX, value)) {
				fprintf(stderr, "Error: invalid value for %s\n", option);
				return 1;
			}
			if (is_index) tuner_idx = static_cast<int>(value);
			else duration = static_cast<int>(value);
		} else if (strcmp(argv[i], "--b25") == 0) {
			use_b25 = true;
		} else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
			fprintf(stderr, "Error: unknown option %s\n", argv[i]);
			return 1;
		} else if (!sys_str) {
			sys_str = argv[i];
		} else if (channel < 0) {
			if (sys_str[0] == 'C' || sys_str[0] == 'c') {
				int64_t value;
				if (!parse_number(argv[i], 13, 63, value)) {
					fprintf(stderr, "Error: ISDB-C channel must be 13-63\n");
					return 1;
				}
				channel = static_cast<int>(value);
			} else {
				int64_t value;
				if (!parse_number(argv[i], 0, INT_MAX, value)) {
					fprintf(stderr, "Error: invalid channel: %s\n", argv[i]);
					return 1;
				}
				channel = static_cast<int>(value);
			}
		} else if (!output_seen) {
			outfile = argv[i];
			output_seen = true;
		} else {
			fprintf(stderr, "Error: unexpected argument: %s\n", argv[i]);
			return 1;
		}
	}

	if (!sys_str || channel < 0) {
		usage();
		return 1;
	}
	if (!sys_str[0] || sys_str[1] != '\0' || !strchr("TtSsCc", sys_str[0])) {
		fprintf(stderr, "Error: unknown broadcast system: %s\n", sys_str);
		return 1;
	}

	bool is_c = (sys_str[0] == 'C' || sys_str[0] == 'c');
	if (c_options && !is_c) {
		fprintf(stderr, "Error: --freq-khz, --freq-offset and --tsmf require C\n");
		return 1;
	}
	bool is_t = (sys_str[0] == 'T' || sys_str[0] == 't');
	px4::SystemType sys_type =
		is_c ? px4::SystemType::ISDB_C :
		is_t ? px4::SystemType::ISDB_T : px4::SystemType::ISDB_S;

	// Validate channel and compute frequency
	uint32_t freq_kHz = 0;
	if (is_c) {
		// ChSet の C13-C22 は物理3-12、C23-C63 は物理22-62に対応する。
		int physical_ch = channel <= 22 ? channel - 10 : channel - 1;
		int64_t center = has_freq ? direct_freq :
			93000 + physical_ch * 6000 + (physical_ch == 12 ? 2000 : 0);
		center += freq_offset;
		if (center <= 0 || center > UINT32_MAX) {
			fprintf(stderr, "Error: adjusted ISDB-C frequency is out of range\n");
			return 1;
		}
		freq_kHz = static_cast<uint32_t>(center);
	} else if (is_t) {
		if (channel < 13 || channel > 62) {
			fprintf(stderr, "Error: ISDB-T channel must be 13-62 (got %d)\n",
				channel);
			return 1;
		}
		freq_kHz = isdb_t_freq_kHz(channel);
	} else {
		if (channel < 0 || channel > 23) {
			fprintf(stderr, "Error: ISDB-S transponder must be 0-23"
				" (BS: 0-11, CS110: 12-23) (got %d)\n", channel);
			return 1;
		}
		freq_kHz = (channel < 12)
			? isdb_s_bs_freq_kHz(channel)
			: isdb_s_cs_freq_kHz(channel - 12);
	}

	// Signal handlers
	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	// Open output
	int outfd = STDOUT_FILENO;
	if (outfile[0] != '-' || outfile[1] != '\0') {
		outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (outfd < 0) {
			fprintf(stderr, "Error: cannot open '%s': %s\n",
				outfile, strerror(errno));
			return 1;
		}
	}

	// Connect to ctrl socket, auto-starting the sibling daemon when needed.
	bool started_driver_host = false;
	int ctrl_fd = connect_ctrl_socket(&started_driver_host);
	if (ctrl_fd < 0) {
		fprintf(stderr, "Error: cannot connect to %s: %s\n"
			"Make sure DriverHost_PX4 is available next to px4rec, or start it manually.\n",
			ctrl_sock(), strerror(errno));
		if (outfd != STDOUT_FILENO) close(outfd);
		return 1;
	}

	// ---- GET_VERSION -------------------------------------------------------
	{
		px4::command::CtrlVersionCmd cmd{};
		cmd.cmd    = px4::command::CtrlCmdCode::GET_VERSION;
		cmd.status = px4::command::CtrlStatusCode::NONE;
		if (!send_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    !recv_all(ctrl_fd, &cmd, sizeof(cmd))) {
			fprintf(stderr, "Error: GET_VERSION I/O failed\n");
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		if (cmd.status != px4::command::CtrlStatusCode::SUCCEEDED) {
			fprintf(stderr, "Error: GET_VERSION returned failure\n");
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
	}

	int data_fd = -1;
	if (!open_session(ctrl_fd, sys_type, tuner_idx, sys_str, data_fd)) {
		close(ctrl_fd);
		if (outfd != STDOUT_FILENO) close(outfd);
		return 1;
	}

	// ---- Initialize B25 decoder if requested --------------------------------
	ARIB_STD_B25 *b25  = nullptr;
	B_CAS_CARD   *bcas = nullptr;

	if (use_b25) {
		b25 = create_arib_std_b25();
		if (!b25) {
			fprintf(stderr, "Error: create_arib_std_b25() failed\n");
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		b25->set_multi2_round(b25, 4);
		b25->set_strip(b25, 0);
		b25->set_emm_proc(b25, 0);

		bcas = create_b_cas_card();
		if (!bcas) {
			fprintf(stderr, "Error: create_b_cas_card() failed\n");
			b25->release(b25);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		int bc = bcas->init(bcas);
		if (bc < 0) {
			fprintf(stderr, "Error: B_CAS_CARD::init() failed (code=%d)\n", bc);
			if (bc == B_CAS_CARD_ERROR_NO_SMART_CARD)
				fprintf(stderr, "Hint: B-CAS card reader found but no card inserted\n");
			else if (bc == B_CAS_CARD_ERROR_NO_SMART_CARD_READER)
				fprintf(stderr, "Hint: no B-CAS card reader found\n");
			bcas->release(bcas);
			b25->release(b25);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		bc = b25->set_b_cas_card(b25, bcas);
		if (bc < 0) {
			fprintf(stderr, "Error: ARIB_STD_B25::set_b_cas_card() failed (code=%d)\n", bc);
			bcas->release(bcas);
			b25->release(b25);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		fprintf(stderr, "B25 decoder initialized\n");
	}

	// ---- Stream TS data to output ------------------------------------------
	int result = 0;
	{
		// 188 * 256 = 48 KB per read — typical TS chunk size
		const std::size_t BUF_SIZE = 188 * 256;
		auto *rbuf = static_cast<uint8_t *>(malloc(BUF_SIZE));
		if (!rbuf) {
			fprintf(stderr, "Error: out of memory\n");
			if (b25)  { b25->release(b25);   b25  = nullptr; }
			if (bcas) { bcas->release(bcas); bcas = nullptr; }
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}

		// 地デジと BS/CS は、PX-MLT5PE の実測で良好だった、選局・PURGE 後にキャプチャを始める順序を保つ。
		// B25 初期化と読み出しバッファ確保は受信開始前に済ませる。
		/* ISDB-C の選局は、ロック後に受信した TS から TSMF のヘッダーを探して完了を判断する。
		 * キャプチャを始めていないと TS が届かず選局が期限切れになるため、選局より先に始める。
		 * 前の搬送波の TS は選局後の PURGE で破棄する。 */
		if (is_c && !set_capture(ctrl_fd, true)) {
			free(rbuf);
			if (b25) b25->release(b25);
			if (bcas) bcas->release(bcas);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}

		if (!tune_session(ctrl_fd, sys_type, freq_kHz, is_c, tsmf, sys_str, channel)) {
			free(rbuf);
			if (b25) b25->release(b25);
			if (bcas) bcas->release(bcas);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}

		// 選局後にホストのリングバッファと TSMF の途中状態を破棄する。
		{
			px4::command::DataCmd cmd{};
			cmd.cmd = px4::command::DataCmdCode::PURGE;
			if (!send_all(data_fd, &cmd, sizeof(cmd))) {
				fprintf(stderr, "Error: PURGE failed\n");
				free(rbuf);
				if (b25) b25->release(b25);
				if (bcas) bcas->release(bcas);
				close(data_fd);
				send_close(ctrl_fd);
				close(ctrl_fd);
				if (outfd != STDOUT_FILENO) close(outfd);
				return 1;
			}
		}

		if (!is_c && !set_capture(ctrl_fd, true)) {
			free(rbuf);
			if (b25) b25->release(b25);
			if (bcas) bcas->release(bcas);
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}

		/* 壁時計の秒境界や時刻補正で指定時間より早く終了しない。 */
		const auto start = std::chrono::steady_clock::now();

		while (!g_quit) {
			if (duration > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration))
				break;

			struct pollfd pfd{};
			pfd.fd     = data_fd;
			pfd.events = POLLIN;

			int r = poll(&pfd, 1, 1000);
			if (r < 0) {
				if (errno == EINTR) continue;
				fprintf(stderr, "Error: data socket poll failed: %s\n", strerror(errno));
				result = 1;
				break;
			}
			if (r == 0) continue; // timeout → loop and re-check g_quit

			if (!(pfd.revents & POLLIN)) {
				fprintf(stderr, "Error: data socket closed or failed (events=0x%x)\n", pfd.revents);
				result = 1;
				break;
			}

			ssize_t n = recv(data_fd, rbuf, BUF_SIZE, 0);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) continue;
				fprintf(stderr, "Error: data socket %s\n", n == 0 ? "closed unexpectedly" : strerror(errno));
				result = 1;
				break;
			}

			if (b25) {
				ARIB_STD_B25_BUFFER sbuf{};
				ARIB_STD_B25_BUFFER dbuf{};
				sbuf.data = rbuf;
				sbuf.size = static_cast<int32_t>(n);
				if (b25->put(b25, &sbuf) < 0 || b25->get(b25, &dbuf) < 0) {
					fprintf(stderr, "Error: B25 decode failed\n");
					result = 1;
					break;
				}
				const char *p    = reinterpret_cast<const char *>(dbuf.data);
				std::size_t left = static_cast<std::size_t>(dbuf.size);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w < 0 && errno == EINTR) continue;
					if (w <= 0) { fprintf(stderr, "Error: output write failed\n"); result = 1; break; }
					p    += w;
					left -= static_cast<std::size_t>(w);
				}
			} else {
				const char *p    = reinterpret_cast<const char *>(rbuf);
				std::size_t left = static_cast<std::size_t>(n);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w < 0 && errno == EINTR) continue;
					if (w <= 0) { fprintf(stderr, "Error: output write failed\n"); result = 1; break; }
					p    += w;
					left -= static_cast<std::size_t>(w);
				}
			}
			if (result) break;
		}

		// Flush remaining B25 data
		if (b25) {
			ARIB_STD_B25_BUFFER dbuf{};
			if (b25->flush(b25) < 0) {
				fprintf(stderr, "Error: B25 flush failed\n");
				result = 1;
			}
			if (b25->get(b25, &dbuf) < 0) {
				fprintf(stderr, "Error: B25 final output failed\n");
				result = 1;
			} else if (dbuf.size > 0) {
				const char *p    = reinterpret_cast<const char *>(dbuf.data);
				std::size_t left = static_cast<std::size_t>(dbuf.size);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w < 0 && errno == EINTR) continue;
					if (w <= 0) { fprintf(stderr, "Error: output write failed\n"); result = 1; break; }
					p    += w;
					left -= static_cast<std::size_t>(w);
				}
			}
		}

		free(rbuf);
	}

	// ---- Cleanup -----------------------------------------------------------
	if (b25)  { b25->release(b25);   b25  = nullptr; }
	if (bcas) { bcas->release(bcas); bcas = nullptr; }
	if (!close_session(ctrl_fd, data_fd)) result = 1;
	close(ctrl_fd);
	if (outfd != STDOUT_FILENO && close(outfd) < 0) {
		fprintf(stderr, "Error: output close failed: %s\n", strerror(errno));
		result = 1;
	}

	return result;
}
