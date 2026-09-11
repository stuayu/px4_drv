// driver_host.hpp - macOS DriverHost (no Windows HANDLE mutex / notify icon)

#pragma once

#include <memory>
#include <string>
#include <stdexcept>

#include "config.hpp"
#include "device_definition_set.hpp"
#include "device_manager.hpp"
#include "receiver_manager.hpp"
#include "ctrl_server.hpp"
#include "stream_server.hpp"
#include "util.hpp"

namespace px4 {

class DriverHost final {
public:
	DriverHost();
	~DriverHost();

	DriverHost(const DriverHost &) = delete;
	DriverHost &operator=(const DriverHost &) = delete;
	DriverHost(DriverHost &&) = delete;
	DriverHost &operator=(DriverHost &&) = delete;

	/*
	 * idle_timeout_sec が 0 の場合は接続数によらず常駐し、SIGINT / SIGTERM を
	 * 受け取るまで終了しません。0 以外の場合は、全サーバーの接続数が 0 の状態が
	 * その秒数継続したときに終了します。
	 */
	void Run(unsigned int idle_timeout_sec);

	/* シグナルハンドラから呼び出す終了要求。非同期シグナル安全な処理だけを行います。 */
	static void RequestStop() noexcept;

private:
	px4::ConfigSet configs_;
	px4::DeviceDefinitionSet dev_defs_;
	px4::ReceiverManager receiver_manager_;

	std::unique_ptr<px4::DeviceManager> device_manager_;
	std::unique_ptr<px4::CtrlServer>    ctrl_server_;
	std::unique_ptr<px4::StreamServer>  stream_server_;
};

class DriverHostError : public std::runtime_error {
public:
	explicit DriverHostError(const std::string &what_arg)
		: runtime_error(what_arg.c_str()) {}
};

} // namespace px4
