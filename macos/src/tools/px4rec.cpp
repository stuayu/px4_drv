// px4rec.cpp - TS recording client for DriverHost_PX4 on macOS
//
// Usage:
//   px4rec [options] T|S <channel>
//   px4rec [options] T|S <channel> <output_file>
//
// Options:
//   -i N   Tuner index (0-based; default: -1 = first available)
//   -d N   Duration in seconds (default: run until SIGTERM/SIGINT)
//
// Channel encoding:
//   T (ISDB-T): physical UHF channel number  13 – 62
//   S (ISDB-S): transponder number  0–11 = BS,  12–23 = CS110
//
// Output:
//   File path, or '-' (or omitted) for stdout.
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
#include "misc_posix.h"     // GUID, HANDLE stubs, wchar_t helpers

// B25 decoder (ARIB STD-B25 / B-CAS)
extern "C" {
#include "arib_std_b25.h"
#include "b_cas_card.h"
#include "b_cas_card_error_code.h"
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char CTRL_SOCK[] = "/tmp/px4_ctrl_pipe.sock";
static const char DATA_SOCK[] = "/tmp/px4_data_pipe.sock";

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
	int fd = unix_connect(CTRL_SOCK);
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
		fd = unix_connect(CTRL_SOCK);
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

static void send_close(int ctrl_fd)
{
	// SET_CAPTURE(false)
	{
		px4::command::CtrlCaptureCmd cmd{};
		cmd.cmd     = px4::command::CtrlCmdCode::SET_CAPTURE;
		cmd.status  = px4::command::CtrlStatusCode::NONE;
		cmd.capture = false;
		send_all(ctrl_fd, &cmd, sizeof(cmd));
		recv_all(ctrl_fd, &cmd, sizeof(cmd));
	}
	// CLOSE
	{
		px4::command::CtrlCloseCmd cmd{};
		cmd.cmd    = px4::command::CtrlCmdCode::CLOSE;
		cmd.status = px4::command::CtrlStatusCode::NONE;
		send_all(ctrl_fd, &cmd, sizeof(cmd));
		recv_all(ctrl_fd, &cmd, sizeof(cmd));
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
	int         tuner_idx  = -1;    // -1 → first available
	int         duration   = -1;    // -1 → run until signal
	bool        use_b25    = false;
	const char *sys_str    = nullptr;
	int         channel    = -1;
	const char *outfile    = "-";   // stdout

	// Parse arguments
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			tuner_idx = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			duration = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--b25") == 0) {
			use_b25 = true;
		} else if (!sys_str) {
			sys_str = argv[i];
		} else if (channel < 0) {
			channel = atoi(argv[i]);
		} else {
			outfile = argv[i];
		}
	}

	if (!sys_str || channel < 0) {
		fprintf(stderr,
			"Usage: px4rec [-i tuner_index] [-d duration_sec] [--b25]"
			" T|S <channel> [output|-]\n"
			"  T    ISDB-T terrestrial: physical channel 13-62\n"
			"  S    ISDB-S satellite:   transponder 0-11=BS, 12-23=CS110\n"
			"  --b25  decode ARIB STD-B25 scrambling via B-CAS card\n");
		return 1;
	}

	bool is_t = (sys_str[0] == 'T' || sys_str[0] == 't');
	px4::SystemType sys_type =
		is_t ? px4::SystemType::ISDB_T : px4::SystemType::ISDB_S;

	// Validate channel and compute frequency
	uint32_t freq_kHz = 0;
	if (is_t) {
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
			CTRL_SOCK, strerror(errno));
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
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
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
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
		data_id = open_cmd->receiver_info.data_id;
		free(open_cmd);
	}

	// ---- SET_PARAMS --------------------------------------------------------
	{
		px4::command::CtrlParamsCmd cmd{};
		cmd.cmd            = px4::command::CtrlCmdCode::SET_PARAMS;
		cmd.status         = px4::command::CtrlStatusCode::NONE;
		cmd.param_set.system = sys_type;
		cmd.param_set.freq   = freq_kHz;
		cmd.param_set.num    = 0;
		if (!send_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    !recv_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    cmd.status != px4::command::CtrlStatusCode::SUCCEEDED) {
			fprintf(stderr, "Error: SET_PARAMS failed\n");
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
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
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
	}

	// ---- SET_CAPTURE(true) -------------------------------------------------
	{
		px4::command::CtrlCaptureCmd cmd{};
		cmd.cmd     = px4::command::CtrlCmdCode::SET_CAPTURE;
		cmd.status  = px4::command::CtrlStatusCode::NONE;
		cmd.capture = true;
		if (!send_all(ctrl_fd, &cmd, sizeof(cmd)) ||
		    !recv_all(ctrl_fd, &cmd, sizeof(cmd))) {
			fprintf(stderr, "Error: SET_CAPTURE(true) I/O failed\n");
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
	}

	// ---- Open data socket and send SET_DATA_ID -----------------------------
	int data_fd = unix_connect(DATA_SOCK);
	if (data_fd < 0) {
		fprintf(stderr, "Error: cannot connect to %s: %s\n",
			DATA_SOCK, strerror(errno));
		send_close(ctrl_fd);
		close(ctrl_fd);
		if (outfd != STDOUT_FILENO) close(outfd);
		return 1;
	}
	{
		px4::command::DataCmd dcmd{};
		dcmd.cmd     = px4::command::DataCmdCode::SET_DATA_ID;
		dcmd.data_id = data_id;
		if (!send_all(data_fd, &dcmd, sizeof(dcmd))) {
			fprintf(stderr, "Error: SET_DATA_ID failed\n");
			close(data_fd);
			send_close(ctrl_fd);
			close(ctrl_fd);
			if (outfd != STDOUT_FILENO) close(outfd);
			return 1;
		}
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

		time_t start = time(nullptr);

		while (!g_quit) {
			if (duration > 0 && (time(nullptr) - start) >= duration)
				break;

			struct pollfd pfd{};
			pfd.fd     = data_fd;
			pfd.events = POLLIN;

			int r = poll(&pfd, 1, 1000);
			if (r < 0) {
				if (errno == EINTR) continue;
				break;
			}
			if (r == 0) continue; // timeout → loop and re-check g_quit

			if (!(pfd.revents & POLLIN)) break;

			ssize_t n = recv(data_fd, rbuf, BUF_SIZE, 0);
			if (n <= 0) break;

			if (b25) {
				ARIB_STD_B25_BUFFER sbuf{};
				ARIB_STD_B25_BUFFER dbuf{};
				sbuf.data = rbuf;
				sbuf.size = static_cast<int32_t>(n);
				if (b25->put(b25, &sbuf) < 0 || b25->get(b25, &dbuf) < 0) {
					fprintf(stderr, "Error: B25 decode failed\n");
					g_quit = 1;
					break;
				}
				const char *p    = reinterpret_cast<const char *>(dbuf.data);
				std::size_t left = static_cast<std::size_t>(dbuf.size);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w <= 0) { g_quit = 1; break; }
					p    += w;
					left -= static_cast<std::size_t>(w);
				}
			} else {
				const char *p    = reinterpret_cast<const char *>(rbuf);
				std::size_t left = static_cast<std::size_t>(n);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w <= 0) { g_quit = 1; break; }
					p    += w;
					left -= static_cast<std::size_t>(w);
				}
			}
		}

		// Flush remaining B25 data
		if (b25) {
			ARIB_STD_B25_BUFFER dbuf{};
			b25->flush(b25);
			if (b25->get(b25, &dbuf) >= 0 && dbuf.size > 0) {
				const char *p    = reinterpret_cast<const char *>(dbuf.data);
				std::size_t left = static_cast<std::size_t>(dbuf.size);
				while (left > 0) {
					ssize_t w = write(outfd, p, left);
					if (w <= 0) break;
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
	close(data_fd);
	send_close(ctrl_fd);
	close(ctrl_fd);
	if (outfd != STDOUT_FILENO) close(outfd);

	return 0;
}
