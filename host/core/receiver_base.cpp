// receiver_base.cpp

#include "receiver_base.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <new>
#include <windows.h>

#include "msg.h"

namespace px4 {

ReceiverBase::ReceiverBase(unsigned int options)
	: options_(options),
	lock_()
{
	memset(&params_, 0, sizeof(params_));
	stream_buf_.reset(new StreamBuffer());
}

ReceiverBase::~ReceiverBase()
{

}

bool ReceiverBase::GetParameters(px4::command::ParameterSet &param_set) noexcept
{
	std::lock_guard<std::mutex> lock(lock_);

	param_set.system = params_.system;
	param_set.freq = params_.freq;

	for (std::uint32_t i = 0; i < param_set.num; i++) {
		switch (param_set.params[i].type) {
		case px4::command::ParameterType::BANDWIDTH:
			param_set.params[i].value = params_.bandwidth;
			break;

		case px4::command::ParameterType::TSMF_STREAM:
			param_set.params[i].value = params_.tsmf_stream;
			break;

		case px4::command::ParameterType::STREAM_ID:
			param_set.params[i].value = params_.stream_id;
			break;

		default:
			// unknown parameter type
			return false;
		}
	}

	return true;
}

bool ReceiverBase::SetParameters(const px4::command::ParameterSet &param_set) noexcept
{
	std::lock_guard<std::mutex> lock(lock_);

	params_.system = param_set.system;
	params_.freq = param_set.freq;
	params_.tsmf_stream = 0;

	for (std::uint32_t i = 0; i < param_set.num; i++) {
		switch (param_set.params[i].type) {
		case px4::command::ParameterType::BANDWIDTH:
			params_.bandwidth = param_set.params[i].value;
			break;

		// J.83 Annex B は TSMF を使わないため、ISDB-C だけで受け付ける
		case px4::command::ParameterType::TSMF_STREAM:
			if (params_.system != px4::SystemType::ISDB_C || param_set.params[i].value > TSMF_STREAM_MAX)
				return false;
			params_.tsmf_stream = param_set.params[i].value;
			break;

		case px4::command::ParameterType::STREAM_ID:
			params_.stream_id = param_set.params[i].value;
			break;

		default:
			// unknown parameter type
			return false;
		}
	}

	/* 選局が変わると、受信統計は前の番組のものにならないよう初期化する。 */
	stream_buf_->ResetStats();

	return true;
}

void ReceiverBase::ClearParameters() noexcept
{
	std::lock_guard<std::mutex> lock(lock_);

	memset(&params_, 0, sizeof(params_));
}

void ReceiverBase::LogTuneFailure(std::uint64_t tune_start_ms, int result) noexcept
{
	const std::uint64_t now_ms = msg_now_ms();
	const std::uint64_t elapsed_ms = now_ms >= tune_start_ms ? now_ms - tune_start_ms : 0;

	stream_buf_->CancelTuneTiming();
	msg_log_at(result == -ECANCELED ? PX4_LOG_DEBUG : PX4_LOG_WARN,
		   PX4_LOG_CAT_TS,
		   "Tune failed: system=0x%08x freq_khz=%u elapsed_ms=%llu result=%d\n",
		   static_cast<unsigned int>(params_.system),
		   static_cast<unsigned int>(params_.freq),
		   static_cast<unsigned long long>(elapsed_ms), result);
}

bool ReceiverBase::Tune(std::uint32_t timeout)
{
	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);
	/* 選局の時間内訳は、この選局で最後に確定したものだけを報告する。 */
	const std::uint64_t tune_start_ms = msg_now_ms();
	stream_buf_->CancelTuneTiming();

	bool override_result = false;
	if (TuneOverride(timeout, override_result))
		return override_result;
	if (PrepareTune())
		return false;

	if (params_.system == px4::SystemType::ISDB_C || params_.system == px4::SystemType::J83B)
		return TuneIsdbC(timeout, tune_start_ms);

	if ((params_.system == px4::SystemType::ISDB_S) && (options_ & RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE)) {
		ret = SetStreamId();
		if (ret) {
			LogTuneFailure(tune_start_ms, ret);
			return false;
		}
	}

	ret = SetFrequency();
	if (ret) {
		LogTuneFailure(tune_start_ms, ret);
		return false;
	}
	const std::uint64_t freq_done_ms = msg_now_ms();

	int i;
	auto begin = std::chrono::steady_clock::now();
	bool locked = false;

	for (i = 0; true; i++) {
		ret = CheckLock(locked);
		if ((!ret && locked) || ret == -ECANCELED)
			break;

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
		if (duration.count() >= timeout) {
			ret = -ETIMEDOUT;
			break;
		}

		Sleep(20);
	}

	if (ret || !locked) {
		LogTuneFailure(tune_start_ms, ret);
		return false;
	}

	/* ロックを確認した時点で時間内訳の保持を始める。直後の待機より前に
	 * 届く最初の TS packet を取り逃がさないため、ここで設定する。 */
	stream_buf_->ArmTuneTiming(tune_start_ms, freq_done_ms - tune_start_ms,
				   msg_now_ms() - tune_start_ms, params_.system,
				   params_.freq);

	if ((params_.system == px4::SystemType::ISDB_T) && (options_ & RECEIVER_WAIT_AFTER_LOCK_TC_T) && (i < 35))
		Sleep((35 - i) * 10);

	if ((params_.system == px4::SystemType::ISDB_S) && !(options_ & RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE)) {
		ret = SetStreamId();
		if (ret) {
			LogTuneFailure(tune_start_ms, ret);
			return false;
		}
	}

	if (options_ & RECEIVER_WAIT_AFTER_LOCK)
		Sleep(200);

	return true;
}

bool ReceiverBase::TuneIsdbC(std::uint32_t timeout, std::uint64_t tune_start_ms)
{
	int ret = SetFrequency();
	if (ret) {
		LogTuneFailure(tune_start_ms, ret);
		return false;
	}
	const std::uint64_t freq_done_ms = msg_now_ms();

	// Sony の sony_integ_isdbc_WaitTSLock() と sony_integ_j83b_WaitTSLock() は、復調器のロックを1秒で打ち切る
	// 信号のない搬送波を TuneTimeout まで待つと、行の多い ChSet のスキャンが長引くため、ロックまでの待ちだけこの上限にする
	const std::uint32_t lock_timeout = (std::min<std::uint32_t>)(timeout, 1000);
	auto begin = std::chrono::steady_clock::now();
	bool lock_seen = false;

	while (true) {
		bool locked = false;
		ret = CheckLock(locked);

		if (ret == -ECANCELED) {
			LogTuneFailure(tune_start_ms, ret);
			return false;
		}

		if (!ret && locked) {
			// ロックより前に届いたヘッダーは前の搬送波のものかもしれないため、ロックしてから TSMF を解析し直す
			if (!lock_seen) {
				lock_seen = true;
				stream_buf_->RestartTsmf();
				/* 最初の TS packet までの時間は、ロックを確認した時点から保持する。 */
				stream_buf_->ArmTuneTiming(tune_start_ms, freq_done_ms - tune_start_ms,
							   msg_now_ms() - tune_start_ms, params_.system,
							   params_.freq);
			} else {
				switch (stream_buf_->GetTsmfState()) {
				case TSMF_FILTER_FOUND:
					return true;
				case TSMF_FILTER_NOT_FOUND:
					// 指定した番号の TS がない搬送波は、待っても現れないため、スキャンを長引かせずに失敗させる
					LogTuneFailure(tune_start_ms, -ENODATA);
					return false;
				default:
					break;
				}
			}
		}

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
		if (duration.count() >= ((lock_seen) ? timeout : lock_timeout)) {
			LogTuneFailure(tune_start_ms, -ETIMEDOUT);
			return false;
		}

		Sleep(20);
	}
}

bool ReceiverBase::ReadStats(px4::command::StatSet &stat_set)
{
	std::lock_guard<std::mutex> lock(lock_);

	for (std::uint32_t i = 0; i < stat_set.num; i++) {
		if (ReadStat(stat_set.data[i].type, stat_set.data[i].value))
			return false;
	}

	return true;
}

ReceiverBase::StreamBuffer::StreamBuffer()
	: stop_generation_(0),
	stream_failed_(false),
	input_packet_count_(0),
	write_size_(0),
	threshold_size_(0),
	tsmf_(false),
	ts_stats_(),
	stats_report_ms_(0),
	stats_report_input_bytes_(0),
	stats_generation_(0),
	tune_timing_{}
{
	ts_stats_.Reset(msg_now_ms());
	stats_report_ms_ = msg_now_ms();
}

ReceiverBase::StreamBuffer::~StreamBuffer()
{

}

void ReceiverBase::StreamBuffer::SetThresholdSize(std::size_t size) noexcept
{
	threshold_size_ = size;
}

bool ReceiverBase::StreamBuffer::Alloc(std::size_t size)
{
	return ringbuf_.Alloc(size);
}

void ReceiverBase::StreamBuffer::Start() noexcept
{
	/* 前回の USB ストリームの異常を、新しいキャプチャへ持ち越さない。 */
	stream_failed_ = false;
	write_size_ = 0;
	input_packet_count_ = 0;

	ringbuf_.Start();
	return;
}

void ReceiverBase::StreamBuffer::Stop() noexcept
{
	ringbuf_.Stop();

	StopRequest();

	return;
}

void ReceiverBase::StreamBuffer::StopRequest() noexcept
{
	/*
	 * 待機側は mtx_ を保持したまま条件を確かめてから待機へ入る。
	 * 同じロック下で状態を変えないと、その間の通知が失われて読み出しが戻らない。
	 */
	{
		/*
		 * 複写中の読み出しを待ってから世代を進める。停止後に読み出しへ入った前の接続は
		 * 世代の変化を見て複写せず、次のキャプチャのデータや Alloc() 中のバッファを読まない。
		 * 複写は待機を含まないため、ここで待つ時間は1回の memcpy に限られる。
		 */
		std::lock_guard<std::mutex> read_lock(read_mtx_);
		std::lock_guard<std::mutex> lock(mtx_);
		stop_generation_++;
	}
	cond_.notify_all();

	return;
}

void ReceiverBase::StreamBuffer::StopRequest(ReadSession &session) noexcept
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		session.stop_ = true;
	}
	cond_.notify_all();

	return;
}

void ReceiverBase::StreamBuffer::NotifyStreamFailure() noexcept
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		stream_failed_ = true;
	}
	cond_.notify_all();

	return;
}

void ReceiverBase::StreamBuffer::ResetInputPacketCount() noexcept
{
	input_packet_count_.store(0, std::memory_order_relaxed);
}

std::uint64_t ReceiverBase::StreamBuffer::GetInputPacketCount() const noexcept
{
	return input_packet_count_.load(std::memory_order_relaxed);
}

void ReceiverBase::StreamBuffer::ResetStats(bool keep_tune_timing) noexcept
{
	const std::uint64_t now = msg_now_ms();

	std::lock_guard<std::mutex> lock(tsmf_mtx_);
	stats_generation_++;
	ts_stats_.Reset(now);
	stats_report_ms_ = now;
	stats_report_input_bytes_ = 0;
	if (!keep_tune_timing)
		tune_timing_.pending = false;
}

void ReceiverBase::StreamBuffer::ArmTuneTiming(std::uint64_t start_ms, std::uint64_t freq_ms,
					       std::uint64_t lock_ms, px4::SystemType system,
					       std::uint32_t freq) noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);
	tune_timing_.pending = true;
	tune_timing_.start_ms = start_ms;
	tune_timing_.freq_ms = freq_ms;
	tune_timing_.lock_ms = lock_ms;
	tune_timing_.system = system;
	tune_timing_.freq = freq;
}

void ReceiverBase::StreamBuffer::CancelTuneTiming() noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);
	tune_timing_.pending = false;
}

void ReceiverBase::StreamBuffer::UpdateStatsLocked(const void *buf, std::size_t size,
						   std::uint64_t input_packets, WriteReport &report) noexcept
{
	const std::uint32_t interval_ms = msg_log_get_stats_interval_ms();
	const bool collect_stats = interval_ms &&
		px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	report.stats_generation = stats_generation_;
	if (!collect_stats && !tune_timing_.pending)
		return;

	const std::uint64_t now = msg_now_ms();

	/* 間隔が 0 のときは統計を数えない。受信経路の処理を増やさないため。 */
	if (collect_stats) {
		ts_stats_.AddInput(static_cast<const std::uint8_t *>(buf), size, now);
		if (now >= stats_report_ms_ && now - stats_report_ms_ >= interval_ms) {
			report.stats_due = true;
			report.stats = ts_stats_.Get();
			report.elapsed_ms = now - stats_report_ms_;
			report.input_bytes_delta = report.stats.input_bytes - stats_report_input_bytes_;
			stats_report_ms_ = now;
			stats_report_input_bytes_ = report.stats.input_bytes;
		}
	}

	/* 選局後の最初の TS packet を受けた時点で、時間内訳を1回だけ報告する。 */
	if (tune_timing_.pending && input_packets) {
		tune_timing_.pending = false;
		report.tune_due = true;
		report.tune = tune_timing_;
		report.tune_first_ms = now >= tune_timing_.start_ms ? now - tune_timing_.start_ms : 0;
	}
}

void ReceiverBase::StreamBuffer::LogStats(const WriteReport &report) noexcept
{
	const TsStatsSnapshot &s = report.stats;
	const double mbps = report.elapsed_ms ?
		static_cast<double>(report.input_bytes_delta) * 8.0 /
		(static_cast<double>(report.elapsed_ms) * 1000.0) : 0.0;
	char gap_pid[8] = "-";

	if (s.cc_gaps && s.last_gap_pid != 0xffff)
		std::snprintf(gap_pid, sizeof(gap_pid), "0x%04x", static_cast<unsigned int>(s.last_gap_pid));

	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		  "TS stats: in_packets=%llu in_bytes=%llu in_mbps=%.2f cc_gaps=%llu last_gap_pid=%s tei=%llu sync_errors=%llu null=%llu max_gap_ms=%llu\n",
		 static_cast<unsigned long long>(s.input_packets),
		 static_cast<unsigned long long>(s.input_bytes), mbps,
		 static_cast<unsigned long long>(s.cc_gaps), gap_pid,
		 static_cast<unsigned long long>(s.tei_packets),
		 static_cast<unsigned long long>(s.sync_errors),
		 static_cast<unsigned long long>(s.null_packets),
		 static_cast<unsigned long long>(s.max_gap_ms));
}

void ReceiverBase::StreamBuffer::LogTune(const WriteReport &report) noexcept
{
	const TuneTiming &t = report.tune;

	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		  "Tune timing: system=0x%08x freq_khz=%u set_ms=%llu lock_ms=%llu first_ts_ms=%llu\n",
		 static_cast<unsigned int>(t.system),
		 static_cast<unsigned int>(t.freq),
		 static_cast<unsigned long long>(t.freq_ms),
		 static_cast<unsigned long long>(t.lock_ms),
		 static_cast<unsigned long long>(report.tune_first_ms));
}

bool ReceiverBase::StreamBuffer::Write(const void *buf, std::size_t &size) noexcept
{
	/*
	 * TSMF の選別より前に入力 TS を数える。機種によっては 188 byte
	 * 境界にそろえた複数パケットを1回の Write() で渡すため、まとめて数える。
	 */
	std::uint64_t input_packets = 0;
	if (buf && size >= 188 && (size % 188) == 0) {
		const auto *packets = static_cast<const std::uint8_t *>(buf);
		std::uint64_t count = 0;
		for (std::size_t offset = 0; offset < size; offset += 188)
			count += packets[offset] == 0x47;
		input_packets = count;
		if (count)
			input_packet_count_.fetch_add(count, std::memory_order_relaxed);
	}

	/* 統計の集計は WriteFiltered() のロック内で行い、報告の出力はロックを離してから行う。 */
	WriteReport report{};
	const bool ret = WriteFiltered(buf, size, input_packets, report);
	if (report.stats_due) {
		bool current_generation;
		{
			std::lock_guard<std::mutex> lock(tsmf_mtx_);
			current_generation = report.stats_generation == stats_generation_;
		}
		if (current_generation)
			LogStats(report);
	}
	if (report.tune_due)
		LogTune(report);
	return ret;
}

bool ReceiverBase::StreamBuffer::WriteFiltered(const void *buf, std::size_t &size,
					       std::uint64_t input_packets, WriteReport &report) noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);
	bool ret;

	UpdateStatsLocked(buf, size, input_packets, report);

	if (tsmf_) {
		const std::uint8_t *out;

		/* tsmf_filter_packet() は 188 byte の1パケットずつ受け取る。 */
		size = tsmf_filter_packet(tsmf_filter_.get(), static_cast<const std::uint8_t *>(buf), &out);
		if (!size)
			return true;

		ret = ringbuf_.Write(out, size);
		write_size_ += size;
		return ret;
	}

	ret = ringbuf_.Write(buf, size);
	write_size_ += size;

	return ret;
}

void ReceiverBase::StreamBuffer::NotifyWrite() noexcept
{
	bool notify = false;

	// TSMF で選んだ TS は低レートのことがあり、しきい値までためると読み出しが秒単位で遅れるため、出力があるたびに通知する
	if (tsmf_) {
		if (write_size_) {
			notify = true;
			write_size_ = 0;
		}
	} else if (write_size_ >= threshold_size_) {
		notify = true;
		write_size_ -= threshold_size_;
	}

	if (notify) {
		/* 受信待ちが残量 0 を確かめてから待機へ入るまでの間に、通知を失わない。 */
		{
			std::lock_guard<std::mutex> lock(mtx_);
		}
		cond_.notify_all();
	}
	return;
}

ReceiverBase::StreamBuffer::ReadResult ReceiverBase::StreamBuffer::HandleRead(std::size_t buf_size, std::function<bool(const void *buf, std::size_t size)> handler, ReadSession *session)
{
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[buf_size]);
	std::uint8_t *p = buf.get();
	std::unique_lock<std::mutex> lock(mtx_);

	/* 入口より前の Stop() は前回の読み出しへの要求として扱い、従来どおり無視する。 */
	const std::uint64_t generation = stop_generation_;
	auto stopped = [&] {
		return stop_generation_ != generation || (session && session->stop_);
	};
	/* キャプチャ開始前の待機は、前回の USB ストリームの異常で終わらせない。 */
	auto failed = [&] {
		return stream_failed_ && ringbuf_.IsActive();
	};

	while (!ringbuf_.IsActive() && !stopped())
		cond_.wait(lock);

	while (true) {
		while (!ringbuf_.GetReadableSize() && !stopped() && !failed())
			cond_.wait(lock);

		if (stopped())
			return ReadResult::Ended;

		/* 異常通知の後も、受信済みのデータを配信し終えてから異常として戻る。 */
		if (!ringbuf_.GetReadableSize())
			return ReadResult::Failed;

		/*
		 * 配信先への書き込みは詰まる可能性がある。mtx_ を手放し、停止要求と
		 * 受信側の通知を待たせない。リングバッファは自身で同期する。
		 */
		lock.unlock();

		std::size_t size = buf_size;
		bool next;
		{
			/*
			 * 読み出しは接続ごとのバッファへの複写だけを排他する。配信中まで保持すると、
			 * 受信機を閉じた後も書き込みが詰まった前の接続が、開き直した接続の配信を止める。
			 */
			std::lock_guard<std::mutex> read_lock(read_mtx_);
			bool stale;
			{
				std::lock_guard<std::mutex> state_lock(mtx_);
				stale = stopped();
			}
			if (stale)
				size = 0;	/* 次の確認で Ended として戻る */
			next = stale || ringbuf_.Read(p, size);
		}
		if (next && size)
			next = handler(p, size);

		lock.lock();

		if (!next)
			return ReadResult::Ended;
	}
}

bool ReceiverBase::StreamBuffer::Purge() noexcept
{
	/* px4rec などは選局後に PURGE してから受信を始めるため、選局時間の計測は残す。 */
	ResetStats(true);
	return ringbuf_.Purge();
}

bool ReceiverBase::StreamBuffer::Purge(std::chrono::steady_clock::time_point deadline) noexcept
{
	ResetStats(true);
	return ringbuf_.Purge(deadline);
}

bool ReceiverBase::StreamBuffer::StartTsmf(std::uint32_t stream_index, bool discard_null_packets, unsigned int stale_packets) noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);

	// 選局はパイプの処理の noexcept の中から呼ばれ、例外が出ると DriverHost ごと終了して全受信機の録画が止まるため、確保の失敗は選局の失敗として返す
	if (!tsmf_filter_) {
		tsmf_filter_.reset(new (std::nothrow) tsmf_filter);
		if (!tsmf_filter_)
			return false;
	}

	tsmf_filter_init(tsmf_filter_.get(), static_cast<int>(stream_index), discard_null_packets, stale_packets);
	tsmf_ = true;
	return true;
}

void ReceiverBase::StreamBuffer::StopTsmf() noexcept
{
	tsmf_ = false;
}

void ReceiverBase::StreamBuffer::RestartTsmf() noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);

	if (tsmf_)
		tsmf_filter_init(tsmf_filter_.get(), tsmf_filter_->stream_index, tsmf_filter_->discard_null_packets, tsmf_filter_->stale_packets);
}

tsmf_filter_state ReceiverBase::StreamBuffer::GetTsmfState() noexcept
{
	std::lock_guard<std::mutex> lock(tsmf_mtx_);

	return (tsmf_) ? tsmf_filter_get_state(tsmf_filter_.get()) : TSMF_FILTER_FOUND;
}

} // namespace px4
