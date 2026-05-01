// main.cpp - macOS entry point for DriverHost_PX4

#define msg_prefix "DriverHost_PX4"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cwchar>

#include <unistd.h>
#include <signal.h>
#include <libusb.h>

#include "driver_host.hpp"
#include "util.hpp"
#include "msg.h"

int main(int /*argc*/, char * /*argv*/[])
{
	msg_set_mode(MSG_MODE_CONSOLE);

	/* Ignore SIGPIPE so broken socket writes don't kill the process */
	signal(SIGPIPE, SIG_IGN);

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
		host.Run();
	} catch (const std::runtime_error &e) {
		msg_err("%s\n", e.what());
		libusb_exit(nullptr);
		return 1;
	}

	msg_info("Exiting...\n");

	libusb_exit(nullptr);
	return 0;
}
