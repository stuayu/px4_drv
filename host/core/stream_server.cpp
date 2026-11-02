// stream_server.cpp
//
// データ (TS) サーバーの Windows 版と macOS 版の共通実装。
// Windows の名前付きパイプはメッセージ単位で受け、macOS の UNIX stream socket は DataCmd の大きさまで読む。
// 読み出しスレッドの停止は接続ごとの ReadSession で伝え、スレッド開始前の停止要求も失わない。

/* macOS では強制取り込みの msg.h が既定値を定義しているため、先に外してから OS 別の名前を定義する。 */
#undef msg_prefix
#if defined(_WIN32)
#define msg_prefix "px4_winusb"
#else
#define msg_prefix "px4_libusb"
#endif

#include "stream_server.hpp"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include "misc_posix.h"
#endif

#include "msg.h"
#include "command.hpp"

namespace px4 {

namespace {

/*
 * DataCmd を1つ読み、読んだ長さを read へ返す。DataCmd より短い場合は false を返す。
 */
bool ReadDataCommand(Pipe &pipe, std::uint8_t *buf, std::size_t size, std::size_t &read, HANDLE cancel) noexcept
{
#if defined(_WIN32)
	/*
	 * メッセージモードでは1回の Read が1メッセージになる。
	 * 短い長さで読むと ERROR_MORE_DATA で失敗するため、必ずバッファ全体を渡す。
	 */
	read = 0;
	if (!pipe.Read(buf, size, read, cancel))
		return false;

	return read >= sizeof(px4::command::DataCmd);
#else
	/* UNIX stream socket は分割・連結されるため、DataCmd の大きさまで読み続ける。 */
	(void)size;
	read = 0;
	while (read < sizeof(px4::command::DataCmd)) {
		std::size_t part = 0;
		if (!pipe.Read(buf + read, sizeof(px4::command::DataCmd) - read, part, cancel) || !part)
			return false;
		read += part;
	}

	return read == sizeof(px4::command::DataCmd);
#endif
}

} // namespace

StreamServer::StreamServer(px4::ReceiverManager &receiver_manager) noexcept
	: ServerBase(L"px4_data_pipe", receiver_manager)
{
	pipe_config_.in_buffer_size = 512;
	pipe_config_.out_buffer_size = 188 * 4096;
	pipe_config_.stream_pipe = false;
	pipe_config_.stream_read = false;
	pipe_config_.default_timeout = 2000;
}

px4::ServerBase::Connection* StreamServer::CreateConnection(std::unique_ptr<px4::PipeServer> &pipe)
{
	return new StreamConnection(*this, pipe);
}

StreamServer::StreamConnection::StreamConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: Connection(parent, pipe)
{

}

void StreamServer::StreamConnection::Worker() noexcept
{
	std::size_t size = config_.in_buffer_size;
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[size]);
	/*
	 * 制御接続が受信機を閉じた後に USB が抜去されると、受信機は解放される。
	 * SET_DATA_ID 以後は受信機を参照せず、共有所有のバッファだけを使う。
	 */
	std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf;
	/* 読み出しスレッドの開始前に切断を検出しても、停止要求を失わない。 */
	px4::ReceiverBase::StreamBuffer::ReadSession session;
	std::unique_ptr<std::thread> stream_th;

	while (true) {
		std::size_t read = 0;
		if (!ReadDataCommand(*conn_, buf.get(), size, read, quit_event_))
			break;

		bool ret = true;
		px4::command::DataCmd *cmd = reinterpret_cast<px4::command::DataCmd *>(buf.get());

		switch (cmd->cmd) {
		case px4::command::DataCmdCode::SET_DATA_ID: {
			if (stream_buf)
				break;

			/*
			 * 検索後に受信機を参照すると、その間の登録解除と抜去による解放と競合する。
			 * 登録表のロック下でバッファを取得し、受信機は参照しない。
			 */
			stream_buf = receiver_manager_.SearchStreamBufferByDataId(cmd->data_id);
			if (!stream_buf) {
				ret = false;
				break;
			}

			try {
				stream_th.reset(new std::thread(
					&px4::StreamServer::StreamConnection::StreamWorker,
					this,
					stream_buf,
					&session));
			} catch (...) {
				ret = false;
			}
			break;
		}

		case px4::command::DataCmdCode::PURGE:
			if (!stream_buf)
				break;

			/* 初期化を諦めた場合は前チャンネルの TS が残るため、記録に残す */
			if (!stream_buf->Purge())
				msg_err("px4::StreamServer::StreamConnection::Worker: Purge() failed.\n");

			break;

		default:
			ret = false;
			break;
		}

		if (!ret)
			break;
	}

	if (stream_buf && stream_th) {
		stream_buf->StopRequest(session);

		stream_th->join();
		stream_th.reset();
	}

	delete this;
}

void StreamServer::StreamConnection::StreamWorker(
	std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf,
	px4::ReceiverBase::StreamBuffer::ReadSession *session) noexcept
{
	msg_dbg("px4::StreamServer::StreamConnection::StreamWorker\n");

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	const auto result = stream_buf->HandleRead(
		config_.out_buffer_size / 4,
		[this](const void *buf, std::size_t size) {
			return conn_->Write(buf, size, size, quit_event_);
		},
		session);

	/*
	 * 接続を残したままだと BonDriver 側が無受信を異常として検出できない。
	 * 送受信を終えて EOF を返し、接続の破棄は Worker() に任せる。
	 */
	if (result == px4::ReceiverBase::StreamBuffer::ReadResult::Failed) {
		msg_err("px4::StreamServer::StreamConnection::StreamWorker: USB stream failed; closing data connection\n");
		conn_->Shutdown();
	}

	msg_dbg("px4::StreamServer::StreamConnection::StreamWorker: exit\n");
}

} // namespace px4
