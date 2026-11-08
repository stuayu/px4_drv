// bon_driver.cpp

#include <algorithm>
#include <cstring>
#include <chrono>
#include <string>
#include <map>
#include <stdexcept>
#include <utility>

#include <windows.h>
#include <aclapi.h>
#include <shlwapi.h>

#include "bon_driver.hpp"
#include "deferred_release.hpp"
#include "config.hpp"
#include "chset.hpp"
#include "security_attributes.hpp"
#include "pipe_client.hpp"
#include "command.hpp"
#include "util.hpp"
#include "msg.h"
#include "isdbc_channel.h"

namespace px4 {

namespace {

px4::DeferredReleasePool<BonDriver>& ReleasedBonDrivers()
{
	// ProxyEx は Release 前に TS reader を join する。Open の10秒期限の3倍を猶予にし、再接続burst用に最大8殻を残す。
	static px4::DeferredReleasePool<BonDriver> drivers(std::chrono::milliseconds(30000), 8);
	return drivers;
}

px4::DeferredReleasePool<IoQueue::IoBuffer>& ReleasedBuffers()
{
	// 返却済みbufferを一時保持し、既定188*1024 byteなら合計768KiB未満に抑える。
	static px4::DeferredReleasePool<IoQueue::IoBuffer> buffers(std::chrono::milliseconds(30000), 4);
	return buffers;
}

void SweepReleasedBonDriverPools()
{
	ReleasedBonDrivers().SweepExpired();
	ReleasedBuffers().SweepExpired();
}

void RetainReleasedBonDriver(BonDriver *driver, std::size_t bytes)
{
	ReleasedBonDrivers().Retain(driver, bytes);
}

void RetainReleasedBuffer(std::unique_ptr<IoQueue::IoBuffer> buffer)
{
	if (!buffer)
		return;
	const std::size_t capacity = buffer->capacity;
	ReleasedBuffers().Retain(std::move(buffer), capacity);
}

} // namespace

BonDriver::BonDriver() noexcept
	: mtx_(),
	released_(false),
	terminated_(false),
	driver_host_path_(),
	configs_(),
	name_(),
	systems_(px4::SystemType::UNSPECIFIED),
	chset_(),
	receivers_(),
	lnb_power_(false),
	lnb_power_state_(false),
	pipe_timeout_(2000),
	tune_timeout_(5000),
	ctrl_client_(nullptr),
	data_pipe_(nullptr),
	open_(FALSE),
	space_(0),
	ch_(0),
	display_error_message_(false),
	ioq_(),
	iorp_(*this),
	current_ofs_(0),
	quit_event_(nullptr)
{

}

BonDriver::~BonDriver()
{
	Term();
}

bool BonDriver::Init()
{
	try {
		const std::wstring &dir_path = px4::util::path::GetDir();

		if (!configs_.Load(px4::util::path::GetFileBase() + L".ini"))
			return false;

		std::size_t num_packets = 1024;
		std::uintptr_t max_buffers = 64, min_buffers = 4;
		int data_ignore_count = 1;

		if (configs_.Exists(L"BonDriver")) {
			const px4::Config &bon_config = configs_.Get(L"BonDriver");
			const std::wstring &mode = bon_config.Get(L"System", L"ISDB-T");
			WCHAR path[MAX_PATH];

			if (!px4::util::ParseSystemStr(mode, systems_))
				return false;

			driver_host_path_ = bon_config.Get(L"DriverHostPath", dir_path + L"DriverHost_PX4.exe");
			if (PathIsRelativeW(driver_host_path_.c_str()) && PathCanonicalizeW(path, (dir_path + driver_host_path_).c_str()))
				driver_host_path_ = path;

			name_ = bon_config.Get(L"Name", L"PX4");

			try {
				pipe_timeout_ = px4::util::wtoui32(bon_config.Get(L"PipeConnectTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				tune_timeout_ = px4::util::wtoui32(bon_config.Get(L"TuneTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				num_packets = px4::util::wtoui(bon_config.Get(L"NumberOfPacketsPerBuffer"));
				if (num_packets <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				max_buffers = px4::util::wtoui(bon_config.Get(L"MaximumNumberOfBuffers"));
				if (max_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				min_buffers = px4::util::wtoui(bon_config.Get(L"MinimumNumberOfBuffers"));
				if (min_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			if (max_buffers < min_buffers)
				return false;

			try {
				data_ignore_count = px4::util::wtoi(bon_config.Get(L"NumberOfBuffersToIgnoreAfterPurge"));
				if (data_ignore_count < 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				int display_error_message = px4::util::wtoi(bon_config.Get(L"DisplayErrorMessage"));
				if (display_error_message == 1) {
					display_error_message_ = true;
				}
			} catch (const std::out_of_range &) {}
		} else {
			systems_ = px4::SystemType::ISDB_T;
			driver_host_path_ = dir_path + L"DriverHost_PX4.exe";
			name_ = L"PX4";
		}

		if ((systems_ & px4::SystemType::ISDB_T) == px4::SystemType::ISDB_T) {
			px4::ChannelSet chset_t;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.ISDB-T").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_t.Load(chset_path, px4::SystemType::ISDB_T);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_t.Clear();
				result = chset_t.Load(dir_path + L"BonDriver_PX4-T.ChSet.txt", px4::SystemType::ISDB_T);
			}

			if (result)
				chset_.Merge(chset_t);
		}

		if ((systems_ & px4::SystemType::ISDB_S) == px4::SystemType::ISDB_S) {
			px4::ChannelSet chset_s;
			bool result;

			try {
				auto &config = configs_.Get(L"BonDriver.ISDB-S");

				try {
					lnb_power_ = !!px4::util::wtoi(config.Get(L"LNBPower"));
				} catch (const std::out_of_range &) {}

				std::wstring chset_path = config.Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_s.Load(chset_path, px4::SystemType::ISDB_S);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_s.Clear();
				result = chset_s.Load(dir_path + L"BonDriver_PX4-S.ChSet.txt", px4::SystemType::ISDB_S);
			}

			if (result)
				chset_.Merge(chset_s);
		}

		if ((systems_ & px4::SystemType::ISDB_C) == px4::SystemType::ISDB_C) {
			px4::ChannelSet chset_c;
			std::wstring chset_path;
			bool has_chset_path = false;
			/*
			 * 既定の BonDriver_PX4-C.ChSet.txt は PX-MLT 用の周波数補正を含む。
			 * 同じフォルダーに置かれた別機種の BonDriver が指定 ChSet を読めなかったとき、
			 * その補正で選局しないよう、INI の ChSetFallback=0 で切り替えを止められる。
			 * 既存の INI は未指定のため、従来どおり既定 ChSet へ切り替える。
			 */
			bool allow_fallback = true;

			try {
				auto &config = configs_.Get(L"BonDriver.ISDB-C");

				try {
					allow_fallback = !!px4::util::wtoi(config.Get(L"ChSetFallback"));
				} catch (const std::out_of_range &) {}

				chset_path = config.Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				has_chset_path = true;
			} catch (const std::out_of_range&) {}

			const bool result = chset_c.LoadWithFallback(has_chset_path ? &chset_path : nullptr,
				dir_path + L"BonDriver_PX4-C.ChSet.txt", allow_fallback, px4::SystemType::ISDB_C);

			if (result)
				chset_.Merge(chset_c);
			else if (!allow_fallback)
				msg_err("Init: ISDB-C ChSet is missing or could not be loaded (%ls); fallback is disabled by ChSetFallback=0\n", chset_path.c_str());
		}

		if ((systems_ & px4::SystemType::J83B) == px4::SystemType::J83B) {
			px4::ChannelSet chset_j;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.J.83B").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_j.Load(chset_path, px4::SystemType::J83B);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_j.Clear();
				result = chset_j.Load(dir_path + L"BonDriver_PX4-J83B.ChSet.txt", px4::SystemType::J83B);
			}

			if (result)
				chset_.Merge(chset_j);
		}

		receivers_.Load(configs_);

		quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!quit_event_)
			return false;

		ioq_.Install(std::make_shared<px4::IoQueue>(px4::IoQueue::IoOperation::READ, iorp_, 188 * num_packets, max_buffers, min_buffers, data_ignore_count));
	} catch (const std::runtime_error &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	}

	return true;
}

void BonDriver::Term() noexcept
{
	std::lock_guard<std::mutex> lock(mtx_);
	TermLocked(false);
}

void BonDriver::TermLocked(bool retain_returned_buffer)
{
	if (terminated_)
		return;

	released_ = true;
	CloseTunerLocked();
	auto queue = ioq_.Reset();
	if (queue) {
		queue->Stop();
		if (retain_returned_buffer)
			RetainReleasedBuffer(queue->DetachLastReturnedBuffer());
	}

	if (quit_event_) {
		CloseHandle(quit_event_);
		quit_event_ = nullptr;
	}

	terminated_ = true;
}

std::size_t BonDriver::GetRetainedMemoryBytes() const noexcept
{
	return sizeof(*this) + (driver_host_path_.capacity() + name_.capacity()) * sizeof(wchar_t) +
		configs_.GetDynamicMemoryBytes() + chset_.GetDynamicMemoryBytes() + receivers_.GetDynamicMemoryBytes();
}

const BOOL BonDriver::OpenTuner()
{
	BOOL ret = TRUE;
	std::lock_guard<std::mutex> lock(mtx_);
	if (released_ || terminated_)
		return FALSE;

	if (open_)
		return TRUE;

	try {
		// CloseTuner() が残した終了通知を消し、新しい読み出しワーカーだけを動かす
		if (!ResetEvent(quit_event_))
			throw BonDriverError("BonDriver::OpenTuner: ResetEvent() failed.");

		HANDLE startup_event;
		DWORD st;

		{
			SecurityAttributes sa(EVENT_ALL_ACCESS);
			startup_event = CreateEventW(sa.Get(), TRUE, FALSE, L"Global\\DriverHost_PX4_StartupEvent");
		}

		if (!startup_event) {
			throw BonDriverError("BonDriver::OpenTuner: CreateEventW(\"DriverHost_PX4_StartupEvent\") failed.");
		} else if (GetLastError() != ERROR_ALREADY_EXISTS) {
			STARTUPINFO si;
			PROCESS_INFORMATION pi;

			memset(&si, 0, sizeof(si));
			si.cb = sizeof(si);
			memset(&pi, 0, sizeof(pi));

			if (!CreateProcessW(driver_host_path_.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
				CloseHandle(startup_event);
				throw BonDriverError("BonDriver::OpenTuner: CreateProcessW() failed.");
			}

			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}

		st = WaitForSingleObject(startup_event, 10000);
		CloseHandle(startup_event);

		if (st != WAIT_OBJECT_0)
			throw BonDriverError("BonDriver::OpenTuner: WaitForSingleObject() failed.");

		std::unique_ptr<px4::PipeClient> ctrl_pipe(new px4::PipeClient());
		px4::PipeClient::PipeClientConfig ctrl_pipe_config;
		px4::command::ReceiverInfo ri_res;

		ctrl_pipe_config.stream_read = false;
		ctrl_pipe_config.timeout = pipe_timeout_;

		if (!ctrl_pipe->Connect(L"px4_ctrl_pipe", ctrl_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: control pipe: cannot connect.");

		ctrl_client_.SetPipe(ctrl_pipe);

		try {
			for (std::size_t i = 0; ; i++) {
				const px4::command::ReceiverInfo &ri = receivers_.Get(i);

				if (ctrl_client_.Open(ri, systems_, &ri_res))
					break;
			}
		} catch (const std::out_of_range &) {
			throw BonDriverError("BonDriver::OpenTuner: no receivers available.");
		}

		px4::PipeClient::PipeClientConfig data_pipe_config;

		data_pipe_config.stream_read = true;
		data_pipe_config.timeout = pipe_timeout_;

		data_pipe_.reset(new px4::PipeClient());

		if (!data_pipe_->Connect(L"px4_data_pipe", data_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: data pipe: cannot connect.");

		px4::command::DataCmd data_cmd;
		std::size_t ret_size;

		data_cmd.cmd = px4::command::DataCmdCode::SET_DATA_ID;
		data_cmd.data_id = ri_res.data_id;

		if (!data_pipe_->Write(&data_cmd, sizeof(data_cmd), ret_size))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");

		if (!ctrl_client_.SetCapture(true))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");

		auto queue = ioq_.GetInstalled();
		if (!queue || !queue->Start())
			throw BonDriverError("BonDriver::OpenTuner: stream queue cannot start.");

		open_ = TRUE;
		if (!ioq_.Enable()) {
			open_ = FALSE;
			throw BonDriverError("BonDriver::OpenTuner: stream queue is unavailable.");
		}
	} catch (const std::exception &e) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	} catch (...) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	}

	if (!open_) {
		if (data_pipe_) {
			if (quit_event_)
				SetEvent(quit_event_);
			auto queue = ioq_.Disable();
			if (queue)
				queue->Stop();
			data_pipe_.reset();
		}

		ctrl_client_.Close();
		ctrl_client_.ClearPipe();
	}

	return ret;
}

void BonDriver::CloseTuner()
{
	std::lock_guard<std::mutex> lock(mtx_);
	CloseTunerLocked();
}


void BonDriver::CloseTunerLocked()
{
	if (!open_)
		return;

	open_ = FALSE;
	auto queue = ioq_.Disable();
	if (data_pipe_) {
		if (quit_event_)
			SetEvent(quit_event_);
		if (queue)
			queue->Stop();
		data_pipe_.reset();
	}

	if (lnb_power_state_) {
		ctrl_client_.SetLnbVoltage(0);
		lnb_power_state_ = false;
	}

	ctrl_client_.Close();
	ctrl_client_.ClearPipe();

	space_ = 0;
	ch_ = 0;

	return;
}

const BOOL BonDriver::SetChannel(const BYTE bCh)
{
	return SetChannel(0, bCh);
}

const float BonDriver::GetSignalLevel(void)
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (released_ || !open_)
		return 0.0f;

	px4::command::StatSet stat_set;

	stat_set.num = 1;
	stat_set.data[0].type = px4::command::StatType::CNR;
	stat_set.data[0].value = 0;

	if (!ctrl_client_.ReadStats(stat_set))
		return 0.0f;

	return static_cast<float>(stat_set.data[0].value) / 1000.0f;
}

const DWORD BonDriver::WaitTsStream(const DWORD dwTimeOut)
{
	auto queue = ioq_.Acquire();
	return (queue && queue->WaitDataBuffer(std::chrono::milliseconds((dwTimeOut == INFINITE) ? 5000 : dwTimeOut))) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

const DWORD BonDriver::GetReadyCount(void)
{
	auto queue = ioq_.Acquire();
	return queue ? static_cast<::DWORD>(queue->GetDataBufferCount() + (queue->HaveReadingBuffer() ? 1 : 0)) : 0;
}

const BOOL BonDriver::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *src;

	if (!pDst)
		return FALSE;

	// コピー型はコピー先の容量を受け取らないため、他の BonDriver と同じくポインター型で取り出した1つのバッファー分をコピーする
	if (!GetTsStream(&src, pdwSize, pdwRemain))
		return FALSE;

	std::memcpy(pDst, src, *pdwSize);

	return TRUE;
}

const BOOL BonDriver::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!ppDst || !pdwSize)
		return FALSE;

	std::size_t size, remain;

	auto queue = ioq_.Acquire();
	if (!queue || !queue->ReadBuffer(reinterpret_cast<void **>(ppDst), size, remain, false))
		return FALSE;

	*pdwSize = static_cast<::DWORD>(size);
	if (pdwRemain)
		*pdwRemain = static_cast<::DWORD>(remain);

	return TRUE;
}

void BonDriver::PurgeTsStream(void)
{
	std::lock_guard<std::mutex> lock(mtx_);
	PurgeTsStreamLocked();
}

void BonDriver::PurgeTsStreamLocked()
{
	if (released_ || !open_ || !data_pipe_)
		return;

	px4::command::DataCmd data_cmd;
	std::size_t ret_size;
	data_cmd.cmd = px4::command::DataCmdCode::PURGE;
	data_pipe_->Write(&data_cmd, sizeof(data_cmd), ret_size);

	auto queue = ioq_.Acquire();
	if (queue)
		queue->PurgeDataBuffer();
}

void BonDriver::Release(void)
{
	std::lock_guard<std::mutex> lock(mtx_);
	if (released_ || terminated_)
		return;

	const std::size_t retained_bytes = GetRetainedMemoryBytes();
	TermLocked(true);
	RetainReleasedBonDriver(this, retained_bytes);
}

LPCTSTR BonDriver::GetTunerName(void)
{
	return name_.c_str();
}

const BOOL BonDriver::IsTunerOpening(void)
{
	std::lock_guard<std::mutex> lock(mtx_);
	return open_;
}

LPCTSTR BonDriver::EnumTuningSpace(const DWORD dwSpace)
{
	try {
		return chset_.GetSpaceName(dwSpace).c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

LPCTSTR BonDriver::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	try {
		return chset_.GetChannel(dwSpace, dwChannel).name.c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

const BOOL BonDriver::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	std::unique_lock<std::mutex> lock(mtx_);
	if (released_ || !open_)
		return FALSE;

	if (!chset_.ExistsChannel(dwSpace, dwChannel))
		return FALSE;

	const px4::ChannelSet::ChannelInfo &channel = chset_.GetChannel(dwSpace, dwChannel);
	px4::SystemType system = chset_.GetSpaceSystem(dwSpace);
	std::uint32_t real_freq, num_param = 0;

	switch (system) {
	case px4::SystemType::ISDB_T:
		if ((channel.ptx_ch >= 3 && channel.ptx_ch <= 12) || (channel.ptx_ch >= 22 && channel.ptx_ch <= 62)) {
			// CATV C13-22ch, C23-63ch
			real_freq = 93143 + (channel.ptx_ch * 6000);

			if (channel.ptx_ch == 12)
				real_freq += 2000;
		} else if (channel.ptx_ch >= 63 && channel.ptx_ch <= 112) {
			// UHF 13-62ch
			real_freq = 95143 + (channel.ptx_ch * 6000);
		} else {
			// unknown channel
			return FALSE;
		}

		break;

	case px4::SystemType::ISDB_S:
		if (channel.ptx_ch <= 11) {
			// BS
			real_freq = 1049480 + (channel.ptx_ch * 38360);
		} else if (channel.ptx_ch <= 23) {
			// CS 110
			real_freq = 1613000 + ((channel.ptx_ch - 12) * 40000);
		} else {
			// unknown channel
			return FALSE;
		}

		num_param = 1;

		break;

	case px4::SystemType::ISDB_C:
	{
		// 物理チャンネル番号から中心周波数への変換は、Linux 版と isdbc_channel.h で共有する
		int khz = isdbc_channel_to_khz(static_cast<int>(channel.ptx_ch));

		if (khz < 0) {
			// unknown channel
			return FALSE;
		}

		real_freq = static_cast<std::uint32_t>(khz);
		num_param = 1;

		break;
	}

	case px4::SystemType::J83B:
		// J.83 Annex B (スカパー！プレミアムサービス光) は、ChSet に中心周波数 (kHz) を書く
		real_freq = channel.freq;

		break;

	default:
		// unknown system
		return FALSE;
	}

	/*
	 * CtrlCmdClient::SetParams() は固定長の ParameterSet を値でコピーするため、
	 * params を使わない地上波でも構造体1個分を確保しないと末尾を範囲外参照する
	 * 使わない領域もそのまま送るので、確保した時点で0で埋めておく
	 */
	px4::command::ParameterSet *param_set = reinterpret_cast<px4::command::ParameterSet*>(new std::uint8_t[std::max<std::size_t>(sizeof(*param_set), sizeof(*param_set) - sizeof(param_set->params) + (sizeof(param_set->params[0]) * num_param))]());

	param_set->system = system;
	param_set->freq = real_freq;
	param_set->num = num_param;

	if (system == px4::SystemType::ISDB_S && num_param) {
		param_set->params[0].type = px4::command::ParameterType::STREAM_ID;
		param_set->params[0].value = channel.tsid;
	}

	if (system == px4::SystemType::ISDB_C) {
		param_set->params[0].type = px4::command::ParameterType::TSMF_STREAM;
		param_set->params[0].value = channel.tsmf_stream;
	}

	bool ret = true;

	try {
		if (lnb_power_) {
			if (system == px4::SystemType::ISDB_S) {
				if (!lnb_power_state_) {
					ret = ctrl_client_.SetLnbVoltage(15);
					/* 給電に成功した場合だけ通電状態にする。 */
					lnb_power_state_ = ret;
				}
			} else if (lnb_power_state_) {
				ret = ctrl_client_.SetLnbVoltage(0);
				lnb_power_state_ = false;
			}
		}

		if (ret) {
			ret = ctrl_client_.SetParams(*param_set);
			if (ret)
				ret = ctrl_client_.Tune(tune_timeout_);
		}
	} catch (const std::exception &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	}

	delete[] reinterpret_cast<std::uint8_t*>(param_set);

	// TSMF で選んだ TS は毎秒数十 KB のことがあり、バッファーが埋まるまで待つと最初のデータまで数秒かかるため、ISDB-C だけ 200ms で途中まででも渡す
	// 地デジや BS/CS は 192KB を 0.1 秒前後で埋めるので、従来どおり埋まるまで待つ
	if (ret) {
		// succeeded
		space_ = dwSpace;
		ch_ = dwChannel;
	}
	if (auto queue = ioq_.Acquire())
		queue->SetReadFlushInterval(std::chrono::milliseconds((system == px4::SystemType::ISDB_C) ? 200 : 0));
	PurgeTsStreamLocked();

	return !!ret;
}

const DWORD BonDriver::GetCurSpace(void)
{
	std::lock_guard<std::mutex> lock(mtx_);
	return space_;
}

const DWORD BonDriver::GetCurChannel(void)
{
	std::lock_guard<std::mutex> lock(mtx_);
	return ch_;
}

bool BonDriver::ReadProvider::Start()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	return true;
}

void BonDriver::ReadProvider::Stop()
{
	return;
}

bool BonDriver::ReadProvider::Do(void *buf, std::size_t &size)
{
	return parent_.data_pipe_->Read(buf, size, size, parent_.quit_event_);
}

} // namespace px4

#pragma warning(disable: 4273)
extern "C" IBonDriver* CreateBonDriver()
{
	try {
		px4::SweepReleasedBonDriverPools();
		auto bon = new px4::BonDriver();

		if (!bon)
			return nullptr;

		if (!bon->Init()) {
			delete bon;
			return nullptr;
		}

		return bon;
	} catch (const std::exception &e) {
		MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	} catch (...) {
		MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	}
}
#pragma warning(default: 4273)
