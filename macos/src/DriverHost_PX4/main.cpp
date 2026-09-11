// main.cpp - macOS entry point for DriverHost_PX4

#define msg_prefix "DriverHost_PX4"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cwchar>
#include <cstring>

#include <unistd.h>
#include <signal.h>
#include <libusb.h>

#include "driver_host.hpp"
#include "util.hpp"
#include "msg.h"

/* 無接続が続いた場合に終了するまでの既定の秒数 */
static const unsigned int kDefaultIdleTimeout = 15;

static void usage(const char *argv0)
{
	std::fprintf(stderr,
		     "usage: %s [options]\n"
		     "\n"
		     "options:\n"
		     "  -r, --resident            クライアントが接続していなくても終了せず常駐する\n"
		     "                            (--idle-timeout=0 と同じ)\n"
		     "  -t, --idle-timeout=<sec>  無接続がこの秒数続いたときに終了する (既定: %u, 0 で常駐)\n"
		     "  -h, --help                この使い方を表示する\n"
		     "\n"
		     "常駐中は SIGINT または SIGTERM で終了処理を開始します。\n",
		     argv0, kDefaultIdleTimeout);
}

static void signal_handler(int /*sig*/)
{
	px4::DriverHost::RequestStop();
}

int main(int argc, char *argv[])
{
	unsigned int idle_timeout = kDefaultIdleTimeout;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = nullptr;

		if (!std::strcmp(arg, "-h") || !std::strcmp(arg, "--help")) {
			usage(argv[0]);
			return 0;
		}

		if (!std::strcmp(arg, "-r") || !std::strcmp(arg, "--resident")) {
			idle_timeout = 0;
			continue;
		}

		if (!std::strcmp(arg, "-t") || !std::strcmp(arg, "--idle-timeout")) {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s: option '%s' requires a value\n", argv[0], arg);
				usage(argv[0]);
				return 1;
			}
			value = argv[++i];
		} else if (!std::strncmp(arg, "--idle-timeout=", 15)) {
			value = arg + 15;
		} else {
			std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
			usage(argv[0]);
			return 1;
		}

		char *end = nullptr;
		unsigned long v = std::strtoul(value, &end, 10);

		if (!*value || !end || *end || v > 86400) {
			std::fprintf(stderr, "%s: invalid idle timeout '%s'\n", argv[0], value);
			return 1;
		}

		idle_timeout = (unsigned int)v;
	}

	msg_set_mode(MSG_MODE_CONSOLE);

	/* Ignore SIGPIPE so broken socket writes don't kill the process */
	signal(SIGPIPE, SIG_IGN);

	/*
	 * 常駐時の停止手段として SIGINT と SIGTERM を受け取ります。
	 * 既定動作のままだと待機中に即座にプロセスが消え、受信機の停止と
	 * ソケットファイルの削除を行わずに終了するため、ハンドラを設定します。
	 */
	{
		struct sigaction sa = {};

		sa.sa_handler = signal_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, nullptr);
		sigaction(SIGTERM, &sa, nullptr);
	}

	/* Initialise libusb with the default context */
	if (libusb_init(nullptr) != LIBUSB_SUCCESS) {
		msg_err("libusb_init() failed\n");
		return 1;
	}

#if defined(_DEBUG) || defined(_DEBUG_MSG)
	libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
#endif

	/* Set executable directory as working directory so relative paths work */
	px4::util::path::Init(nullptr);
	{
		const std::wstring &dir = px4::util::path::GetDir();
		if (!dir.empty()) {
			char buf[4096];
			int i;
			for (i = 0; i < 4095 && dir[i]; i++) buf[i] = (char)dir[i];
			buf[i] = '\0';
			chdir(buf);
		}
	}

	msg_info("Start\n");

	try {
		px4::DriverHost host;
		host.Run(idle_timeout);
	} catch (const std::runtime_error &e) {
		msg_err("%s\n", e.what());
		libusb_exit(nullptr);
		return 1;
	}

	msg_info("Exiting...\n");

	libusb_exit(nullptr);
	return 0;
}
