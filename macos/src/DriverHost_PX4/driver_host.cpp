// driver_host.cpp - macOS DriverHost implementation

#define msg_prefix "DriverHost_PX4"

#include "driver_host.hpp"

#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>   /* flock */

#include "msg.h"
#include "pipe_server.hpp"   /* pipe_server_cleanup declaration */

/* Declared in pipe_server.cpp */
namespace px4 { void pipe_server_cleanup(); }

namespace px4 {

static const char kLockFile[] = "/tmp/px4_drv_host.lock";
static int g_lock_fd = -1;

DriverHost::DriverHost()
{
	configs_.Load(px4::util::path::GetFileBase() + L".ini");
	dev_defs_.Load(configs_);
}

DriverHost::~DriverHost()
{
	ctrl_server_.reset();
	stream_server_.reset();
	device_manager_.reset();
}

void DriverHost::Run()
{
	/* Single-instance guard via advisory lock on a lock file */
	g_lock_fd = ::open(kLockFile, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (g_lock_fd < 0)
		throw DriverHostError("DriverHost::Run: could not open lock file");

	if (::flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
		::close(g_lock_fd);
		g_lock_fd = -1;
		/* Another instance is running – exit quietly */
		msg_info("DriverHost_PX4 is already running.\n");
		return;
	}

	device_manager_.reset(new px4::DeviceManager(dev_defs_, receiver_manager_));

	ctrl_server_.reset(new px4::CtrlServer(receiver_manager_));
	stream_server_.reset(new px4::StreamServer(receiver_manager_));

	ctrl_server_->Start();
	stream_server_->Start();

	msg_info("DriverHost_PX4 started.\n");

	/* Idle loop: exit 15 s after the last client disconnects */
	int idle = 0;
	while (idle < 3) {
		::sleep(5);
		if (!ctrl_server_->GetActiveConnectionCount() &&
		    !stream_server_->GetActiveConnectionCount())
			idle++;
		else
			idle = 0;
	}

	msg_info("DriverHost_PX4 shutting down.\n");

	stream_server_.reset();
	ctrl_server_.reset();
	device_manager_.reset();

	px4::pipe_server_cleanup();

	::flock(g_lock_fd, LOCK_UN);
	::close(g_lock_fd);
	g_lock_fd = -1;
	::unlink(kLockFile);
}

} // namespace px4
