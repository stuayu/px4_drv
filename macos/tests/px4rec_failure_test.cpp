#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <chrono>
#include <ctime>
#include "arib_std_b25.h"
#include "b_cas_card.h"

static int fake_connect(int, const struct sockaddr *, socklen_t);
static ssize_t fake_write(int, const void *, size_t);
static ARIB_STD_B25 *fake_create_b25();
static B_CAS_CARD *fake_create_card();
static time_t fake_time(time_t *);
#define connect fake_connect
#define write fake_write
#define create_arib_std_b25 fake_create_b25
#define create_b_cas_card fake_create_card
#define time fake_time
#define main px4rec_test_main
#include "../src/tools/px4rec.cpp"
#undef main
#undef create_b_cas_card
#undef create_arib_std_b25
#undef time
#undef write
#undef connect

static int ctrl_client = -1;
static int data_client = -1;
static int connect_calls = 0;
static bool fail_output = false;
static int fake_mode = 0;
static time_t fake_wall_time = 100;
static bool host_injected = false, output_attempted = false;
static bool b25_flush_attempted = false, b25_get_attempted = false, b25_put_attempted = false;
static std::vector<unsigned char> observed_output;

/* 秒の境界をまたぐと、実時間が経つ前に旧方式の -d 1 が終了する。 */
static time_t fake_time(time_t *out)
{
	time_t now = fake_mode == 0 ? fake_wall_time++ : ::time(nullptr);
	if (out) *out = now;
	return now;
}

static void fake_release(void *) {}
static int fake_option(void *, int32_t) { return 0; }
static int fake_set_card(void *, B_CAS_CARD *) { return 0; }
static int fake_card_init(void *) { return 0; }
static int fake_flush(void *) { b25_flush_attempted = true; return fake_mode == 4 ? -1 : 0; }
static int fake_put(void *, ARIB_STD_B25_BUFFER *) { b25_put_attempted = true; return fake_mode == 6 ? -1 : 0; }
static int fake_get(void *, ARIB_STD_B25_BUFFER *buf)
{
	buf->data = nullptr;
	buf->size = 0;
	b25_get_attempted = true;
	return fake_mode == 5 ? -1 : 0;
}
static ARIB_STD_B25 *fake_create_b25()
{
	static ARIB_STD_B25 b25{};
	b25.release = fake_release;
	b25.set_multi2_round = fake_option;
	b25.set_strip = fake_option;
	b25.set_emm_proc = fake_option;
	b25.set_b_cas_card = fake_set_card;
	b25.flush = fake_flush;
	b25.put = fake_put;
	b25.get = fake_get;
	return &b25;
}
static B_CAS_CARD *fake_create_card()
{
	static B_CAS_CARD card{};
	card.release = fake_release;
	card.init = fake_card_init;
	return &card;
}

static ssize_t fake_write(int fd, const void *buf, size_t size)
{
	output_attempted = true;
	if (fail_output) { errno = ENOSPC; return -1; }
	if (fake_mode == 8) {
		const auto *p = static_cast<const unsigned char *>(buf);
		observed_output.insert(observed_output.end(), p, p + size);
	}
	return ::write(fd, buf, size);
}

static int fake_connect(int fd, const struct sockaddr *addr, socklen_t)
{
	++connect_calls;
	const auto *un = reinterpret_cast<const sockaddr_un *>(addr);
	int *source = strstr(un->sun_path, "ctrl") ? &ctrl_client : &data_client;
	if (*source < 0) { errno = EACCES; return -1; }
	if (dup2(*source, fd) < 0) return -1;
	close(*source);
	*source = -1;
	return 0;
}

static bool bad_arguments()
{
	char old_dir[4096];
	char tmp[] = "/tmp/px4rec-args-XXXXXX";
	if (!getcwd(old_dir, sizeof(old_dir)) || !mkdtemp(tmp) || chdir(tmp)) return false;
	const char *cases[][5] = {
		{"-d", "abc", "T", "13", nullptr},
		{"-i", "abc", "T", "13", nullptr},
		{"S", "abc", nullptr, nullptr, nullptr},
		{"X", "13", nullptr, nullptr, nullptr},
		{"", "13", nullptr, nullptr, nullptr},
		{"-d", "999999999999999999999", "T", "13", nullptr},
		{"T", "13", "-d", nullptr, nullptr},
		{"T", "13", "--unknown", nullptr, nullptr}
	};
	bool ok = true;
	for (const auto &entry : cases) {
		char program[] = "px4rec";
		char *args[6] = {program};
		int argc = 1;
		for (const char *value : entry) {
			if (!value) break;
			args[argc++] = const_cast<char *>(value);
		}
		connect_calls = 0;
		int result = px4rec_test_main(argc, args);
		if (result == 0 || connect_calls != 0) {
			fprintf(stderr, "invalid argument '%s' reached connection or succeeded\n", entry[0]);
			ok = false;
		}
	}
	bool created = access("-d", F_OK) == 0 || access("--unknown", F_OK) == 0;
	unlink("-d");
	unlink("--unknown");
	chdir(old_dir);
	rmdir(tmp);
	return ok && !created;
}

static bool read_exact(int fd, void *buf, size_t size)
{
	auto *p = static_cast<unsigned char *>(buf);
	while (size) {
		ssize_t n = recv(fd, p, size, 0);
		if (n <= 0) return false;
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

/* mode: 通常終了、切断、書き込み失敗、停止失敗。 */
static void host(int ctrl, int data, int mode)
{
	using namespace px4::command;
	for (;;) {
		CtrlCmdHeader header{};
		if (!read_exact(ctrl, &header, sizeof(header))) break;
		size_t size = sizeof(header);
		switch (header.cmd) {
		case CtrlCmdCode::GET_VERSION: size = sizeof(CtrlVersionCmd); break;
		case CtrlCmdCode::OPEN: size = sizeof(CtrlOpenCmd); break;
		case CtrlCmdCode::SET_PARAMS: size = sizeof(CtrlParamsCmd); break;
		case CtrlCmdCode::TUNE: size = sizeof(CtrlTuneCmd); break;
		case CtrlCmdCode::SET_CAPTURE: size = sizeof(CtrlCaptureCmd); break;
		default: break;
		}
		std::vector<unsigned char> bytes(size);
		memcpy(bytes.data(), &header, sizeof(header));
		if (!read_exact(ctrl, bytes.data() + sizeof(header), size - sizeof(header))) break;
		auto *cmd = reinterpret_cast<CtrlCmdHeader *>(bytes.data());
		cmd->status = CtrlStatusCode::SUCCEEDED;
		bool capture = cmd->cmd == CtrlCmdCode::SET_CAPTURE &&
			reinterpret_cast<CtrlCaptureCmd *>(cmd)->capture;
		if (mode == 3 && cmd->cmd == CtrlCmdCode::SET_CAPTURE && !capture) {
			cmd->status = CtrlStatusCode::FAILED;
			host_injected = true;
		}
		if (send(ctrl, bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) break;
		if (capture && mode == 1) { host_injected = true; shutdown(data, SHUT_WR); }
		if (capture && (mode == 2 || mode == 6 || mode == 8)) {
			char packet[188] = {0x47};
			host_injected = true;
			(void)send(data, packet, sizeof(packet), 0);
		}
		if (capture && mode == 7) { host_injected = true; kill(getpid(), SIGTERM); }
	}
	close(ctrl);
	close(data);
}

static bool run(int mode)
{
	int ctrl[2], data[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctrl) || socketpair(AF_UNIX, SOCK_STREAM, 0, data)) return false;
	ctrl_client = ctrl[0];
	data_client = data[0];
	host_injected = output_attempted = false;
	b25_flush_attempted = b25_get_attempted = b25_put_attempted = false;
	observed_output.clear();
	std::thread server(host, ctrl[1], data[1], mode);
	g_quit = 0;
	fail_output = mode == 2;
	fake_mode = mode;
	fake_wall_time = 100;
	char arg0[] = "px4rec", arg1[] = "-d", arg2[] = "1", arg3[] = "T", arg4[] = "13", arg5[] = "/dev/null";
	char arg6[] = "--b25";
	char *args[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, nullptr};
	const auto start = std::chrono::steady_clock::now();
	int result = px4rec_test_main(mode >= 4 && mode <= 6 ? 7 : 6, args);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	server.join();
	std::vector<unsigned char> expected_packet(188, 0);
	expected_packet[0] = 0x47;
	if (((mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7 || mode == 8) && !host_injected) ||
	    (mode == 2 && !output_attempted) || (mode == 4 && !b25_flush_attempted) ||
	    (mode == 5 && !b25_get_attempted) || (mode == 6 && !b25_put_attempted) ||
	    (mode == 8 && observed_output != expected_packet)) {
		fprintf(stderr, "mode=%d: intended scenario was not exercised\n", mode);
		return false;
	}
	if (mode == 0 && elapsed < std::chrono::milliseconds(900)) {
		fprintf(stderr, "duration ended too early\n");
		return false;
	}
	if (((mode != 0 && mode != 7 && mode != 8) && result == 0) ||
	    ((mode == 0 || mode == 7 || mode == 8) && result != 0)) {
		fprintf(stderr, "mode=%d: unexpected exit %d\n", mode, result);
		return false;
	}
	return true;
}

int main()
{
	return bad_arguments() && run(1) && run(2) && run(3) && run(0) && run(4) && run(5) && run(6) && run(7) && run(8) ? 0 : 1;
}
