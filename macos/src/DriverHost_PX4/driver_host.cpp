// driver_host.cpp - macOS DriverHost implementation

#define msg_prefix "DriverHost_PX4"

#include "driver_host.hpp"

#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>   /* flock */
#include <csignal>

#include "msg.h"
#include "pipe_server.hpp"   /* pipe_server_cleanup declaration */

/* Declared in pipe_server.cpp */
namespace px4 { void pipe_server_cleanup(); }

namespace px4 {

static const char kLockFile[] = "/tmp/px4_drv_host.lock";
static int g_lock_fd = -1;

/* シグナルハンドラから書き換えるため、非同期シグナル安全な型で保持します。 */
static volatile sig_atomic_t g_stop_requested = 0;

void DriverHost::RequestStop() noexcept
{
	g_stop_requested = 1;
}

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

void DriverHost::Run(unsigned int idle_timeout_sec)
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

	if (idle_timeout_sec)
		msg_info("DriverHost_PX4 started. (idle timeout: %u sec)\n", idle_timeout_sec);
	else
		msg_info("DriverHost_PX4 started. (resident)\n");

	/*
	 * 終了要求は 1 秒間隔で確認します。
	 * 待機の刻みを広く取ると SIGTERM から実際の終了までの時間が延び、
	 * 受信中の停止処理が遅れるため、待機はアイドル判定の粒度と分けています。
	 */
	unsigned int idle = 0;

	while (!g_stop_requested) {
		::sleep(1);

		if (ctrl_server_->GetActiveConnectionCount() ||
		    stream_server_->GetActiveConnectionCount()) {
			idle = 0;
			continue;
		}

		/* idle_timeout_sec == 0 は常駐指定のため、無接続でも終了しません。 */
		if (idle_timeout_sec && ++idle >= idle_timeout_sec)
			break;
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
