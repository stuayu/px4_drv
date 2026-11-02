// ctrl_server.cpp
//
// 制御サーバーの Windows 版と macOS 版の共通実装。
// Windows の名前付きパイプはメッセージ境界を保つため、1回の Read で1コマンドを受ける。
// macOS の UNIX stream socket には境界が無いため、ヘッダーで固定長を読み、可変長の要素を後から読む。
// どちらの OS でも、読んだ長さを CheckCommandLength() で検査してからコマンド構造体として扱う。

#include "ctrl_server.hpp"

#include <cstddef>
#include <cstring>

namespace px4 {

namespace {

#if defined(_WIN32)
/* Windows の ReceiverInfo は wchar_t が2 byte のため、この大きさで全コマンドが収まる。 */
constexpr std::size_t kCtrlBufferSize = 512;
#else
/* macOS は wchar_t が4 byte のため ReceiverInfo が約820 byte になり、512 byte では収まらない。 */
constexpr std::size_t kCtrlBufferSize = 4096;
#endif

#if !defined(_WIN32)
/*
 * UNIX stream socket は分割・連結されるため、指定した長さが揃うまで読み続ける。
 * 途中で切断されたか読み出しが中止された場合は false を返し、呼び出し側は接続を閉じる。
 */
bool ReadExact(Pipe &pipe, std::uint8_t *buf, std::size_t size, HANDLE cancel) noexcept
{
	while (size) {
		std::size_t read = 0;
		if (!pipe.Read(buf, size, read, cancel) || !read)
			return false;
		buf += read;
		size -= read;
	}
	return true;
}

/* UNIX stream socket にはメッセージ境界がないため、コマンドごとの固定部分の長さで区切る。 */
std::size_t FixedCommandSize(px4::command::CtrlCmdCode cmd) noexcept
{
	switch (cmd) {
	case px4::command::CtrlCmdCode::GET_VERSION: return sizeof(px4::command::CtrlVersionCmd);
	case px4::command::CtrlCmdCode::OPEN: return sizeof(px4::command::CtrlOpenCmd);
	case px4::command::CtrlCmdCode::GET_INFO: return sizeof(px4::command::CtrlReceiverInfoCmd);
	case px4::command::CtrlCmdCode::SET_CAPTURE: return sizeof(px4::command::CtrlCaptureCmd);
	case px4::command::CtrlCmdCode::GET_PARAMS:
	case px4::command::CtrlCmdCode::SET_PARAMS: return sizeof(px4::command::CtrlParamsCmd);
	case px4::command::CtrlCmdCode::TUNE: return sizeof(px4::command::CtrlTuneCmd);
	case px4::command::CtrlCmdCode::CHECK_LOCK: return sizeof(px4::command::CtrlCheckLockCmd);
	case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE: return sizeof(px4::command::CtrlLnbVoltageCmd);
	case px4::command::CtrlCmdCode::READ_STATS: return sizeof(px4::command::CtrlStatsCmd);
	default: return sizeof(px4::command::CtrlCmdHeader);
	}
}
#endif

/*
 * 1コマンド分を buf へ読み、読んだ長さを read へ返す。
 * CtrlCmdHeader より短い場合は false を返す。
 */
bool ReadCommand(Pipe &pipe, std::uint8_t *buf, std::size_t size, std::size_t &read, HANDLE cancel) noexcept
{
#if defined(_WIN32)
	/*
	 * メッセージモードでは1回の Read が1メッセージになる。
	 * 短い長さで読むと ERROR_MORE_DATA で失敗するため、必ずバッファ全体を渡す。
	 */
	read = 0;
	if (!pipe.Read(buf, size, read, cancel))
		return false;

	return read >= sizeof(px4::command::CtrlCmdHeader);
#else
	read = sizeof(px4::command::CtrlCmdHeader);
	if (size < read || !ReadExact(pipe, buf, read, cancel))
		return false;

	const auto *hdr = reinterpret_cast<const px4::command::CtrlCmdHeader *>(buf);
	const std::size_t fixed_size = FixedCommandSize(hdr->cmd);
	if (fixed_size > size || !ReadExact(pipe, buf + read, fixed_size - read, cancel))
		return false;
	read = fixed_size;

	/* 可変長の要素は、固定部分の num から個数を得て続きを読む。 */
	std::uint32_t count = 0;
	std::size_t element_size = 0;
	if (hdr->cmd == px4::command::CtrlCmdCode::GET_PARAMS || hdr->cmd == px4::command::CtrlCmdCode::SET_PARAMS) {
		count = reinterpret_cast<const px4::command::CtrlParamsCmd *>(buf)->param_set.num;
		element_size = sizeof(px4::command::Parameter);
	} else if (hdr->cmd == px4::command::CtrlCmdCode::READ_STATS) {
		count = reinterpret_cast<const px4::command::CtrlStatsCmd *>(buf)->stat_set.num;
		element_size = sizeof(px4::command::Stat);
	}

	if (count > 1) {
		/* 個数を先に検証し、乗算の溢れと受信バッファ超過を防ぐ。超過した接続は同期を回復できないため閉じる。 */
		if (count - 1 > (size - read) / element_size)
			return false;

		const std::size_t extra = (count - 1) * element_size;
		if (!ReadExact(pipe, buf + read, extra, cancel))
			return false;
		read += extra;
	}

	return true;
#endif
}

} // namespace

CtrlServer::CtrlServer(px4::ReceiverManager &receiver_manager)
	: ServerBase(L"px4_ctrl_pipe", receiver_manager)
{
	pipe_config_.in_buffer_size = kCtrlBufferSize;
	pipe_config_.out_buffer_size = kCtrlBufferSize;
	pipe_config_.stream_pipe = false;
	pipe_config_.stream_read = false;
	pipe_config_.default_timeout = 2000;
}

px4::ServerBase::Connection* CtrlServer::CreateConnection(std::unique_ptr<px4::PipeServer> &pipe)
{
	return new CtrlConnection(*this, pipe);
}

CtrlServer::CtrlConnection::CtrlConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: Connection(parent, pipe)
{

}

bool CtrlServer::CtrlConnection::CheckCommandLength(const std::uint8_t *buf,
						    px4::command::CtrlCmdCode cmd,
						    std::size_t length) noexcept
{
	/*
	 * ParameterSet と StatSet は要素数1の配列を可変長として使うため、
	 * 実際に届いた長さから収容できる要素数を求め、num がそれを超えないことを確かめる
	 */
	auto check_variable = [buf, length](std::size_t fixed_size, std::size_t element_size,
					    std::size_t num_offset) {
		/* 要素数そのものが届いていない段階で読まないよう、先に長さを確かめる */
		if (length < fixed_size)
			return false;

		std::uint32_t num;

		memcpy(&num, buf + num_offset, sizeof(num));
		if (!num)
			return true;

		return (num - 1) <= ((length - fixed_size) / element_size);
	};

	switch (cmd) {
	case px4::command::CtrlCmdCode::GET_VERSION:
		return length >= sizeof(px4::command::CtrlVersionCmd);

	case px4::command::CtrlCmdCode::OPEN:
		return length >= sizeof(px4::command::CtrlOpenCmd);

	case px4::command::CtrlCmdCode::GET_INFO:
		return length >= sizeof(px4::command::CtrlReceiverInfoCmd);

	case px4::command::CtrlCmdCode::SET_CAPTURE:
		return length >= sizeof(px4::command::CtrlCaptureCmd);

	case px4::command::CtrlCmdCode::GET_PARAMS:
	case px4::command::CtrlCmdCode::SET_PARAMS:
		return check_variable(sizeof(px4::command::CtrlParamsCmd),
				      sizeof(px4::command::Parameter),
				      offsetof(px4::command::CtrlParamsCmd, param_set.num));

	case px4::command::CtrlCmdCode::TUNE:
		return length >= sizeof(px4::command::CtrlTuneCmd);

	case px4::command::CtrlCmdCode::CHECK_LOCK:
		return length >= sizeof(px4::command::CtrlCheckLockCmd);

	case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE:
		return length >= sizeof(px4::command::CtrlLnbVoltageCmd);

	case px4::command::CtrlCmdCode::READ_STATS:
		return check_variable(sizeof(px4::command::CtrlStatsCmd),
				      sizeof(px4::command::Stat),
				      offsetof(px4::command::CtrlStatsCmd, stat_set.num));

	default:
		/* ヘッダーだけで完結するコマンドは冒頭の検査で足りる */
		return true;
	}
}

void CtrlServer::CtrlConnection::Worker() noexcept
{
	std::size_t size = config_.in_buffer_size;
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[size]);
	px4::command::ReceiverInfo info = { 0 };
	px4::ReceiverBase *receiver = nullptr;

	while (true) {
		bool ret = true;
		std::size_t read = 0;

		if (!ReadCommand(*conn_, buf.get(), size, read, quit_event_))
			break;

		px4::command::CtrlCmdHeader *hdr = reinterpret_cast<px4::command::CtrlCmdHeader *>(buf.get());

		if (!CheckCommandLength(buf.get(), hdr->cmd, read)) {
			std::size_t written;

			hdr->status = px4::command::CtrlStatusCode::FAILED;
			if (!conn_->Write(buf.get(), read, written) || read != written)
				break;

			continue;
		}

		switch (hdr->cmd) {
		case px4::command::CtrlCmdCode::GET_VERSION:
		{
			px4::command::CtrlVersionCmd *version = reinterpret_cast<px4::command::CtrlVersionCmd *>(buf.get());

			version->status = px4::command::CtrlStatusCode::SUCCEEDED;
			version->driver_version = 0x00040000;
			version->cmd_version = px4::command::VERSION;

			break;
		}

		case px4::command::CtrlCmdCode::OPEN:
		{
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
			}

			px4::command::CtrlOpenCmd *open = reinterpret_cast<px4::command::CtrlOpenCmd *>(buf.get());
			std::uint32_t data_id;

			receiver = receiver_manager_.SearchAndOpen(open->receiver_info, info, data_id);
			if (receiver) {
				open->receiver_info = info;
				open->receiver_info.data_id = data_id;
			}

			open->status = (receiver) ? px4::command::CtrlStatusCode::SUCCEEDED : px4::command::CtrlStatusCode::FAILED;
			break;
		}

		case px4::command::CtrlCmdCode::CLOSE:
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
				info = { 0 };

				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}

			break;

		case px4::command::CtrlCmdCode::GET_INFO:
		{
			px4::command::CtrlReceiverInfoCmd *receiver_info = reinterpret_cast<px4::command::CtrlReceiverInfoCmd *>(buf.get());

			if (receiver) {
				receiver_info->receiver_info = info;
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}

			break;
		}

		case px4::command::CtrlCmdCode::SET_CAPTURE:
		{
			px4::command::CtrlCaptureCmd *capture = reinterpret_cast<px4::command::CtrlCaptureCmd *>(buf.get());

			if (receiver && !receiver->SetCapture((capture->capture) ? true : false))
				capture->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				capture->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::GET_PARAMS:
		{
			px4::command::CtrlParamsCmd *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(buf.get());

			if (receiver && receiver->GetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::SET_PARAMS:
		{
			px4::command::CtrlParamsCmd *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(buf.get());

			if (receiver && receiver->SetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::CLEAR_PARAMS:
			if (receiver && (receiver->ClearParameters(), true))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;

			break;

		case px4::command::CtrlCmdCode::TUNE:
		{
			px4::command::CtrlTuneCmd *tune = reinterpret_cast<px4::command::CtrlTuneCmd *>(buf.get());

			if (receiver && receiver->Tune(tune->timeout))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::CHECK_LOCK:
		{
			px4::command::CtrlCheckLockCmd *check_lock = reinterpret_cast<px4::command::CtrlCheckLockCmd *>(buf.get());

			if (receiver && !receiver->CheckLock(check_lock->locked))
				check_lock->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				check_lock->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE:
		{
			px4::command::CtrlLnbVoltageCmd *lnb = reinterpret_cast<px4::command::CtrlLnbVoltageCmd *>(buf.get());

			if (receiver && !receiver->SetLnbVoltage(lnb->voltage))
				lnb->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				lnb->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::READ_STATS:
		{
			px4::command::CtrlStatsCmd *stats = reinterpret_cast<px4::command::CtrlStatsCmd *>(buf.get());

			if (receiver && receiver->ReadStats(stats->stat_set))
				stats->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				stats->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		default:
			hdr->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}

		if (!ret)
			break;

		std::size_t written;

		if (!conn_->Write(buf.get(), read, written) || read != written)
			break;
	}

	if (receiver) {
		receiver->Close();
		receiver_manager_.ClearDataId(receiver);
	}

	delete this;
}

} // namespace px4
