// receiver_base.hpp

#pragma once

#include <cstdint>
#include <chrono>
#include <cstddef>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>
#include <string>

#include "type.hpp"
#include "command.hpp"
#include "ringbuffer.hpp"
#include "ts_stats.hpp"
#include "tsmf.h"

namespace px4 {

class ReceiverBase {
public:
	struct Parameters {
		px4::SystemType system;
		std::uint32_t freq;
		std::uint32_t bandwidth;
		std::uint32_t stream_id;
		// ISDB-C だけで使う、TSMF で多重された搬送波で、ヘッダーに載っている有効な TS のうち、0 から数えて何番目 (TS0〜TS14) を選ぶか
		std::uint32_t tsmf_stream;
	};

	class StreamBuffer final {
	public:
		/* データ接続ごとの停止要求。読み出しスレッドの開始前に出した要求も保持する。 */
		class ReadSession final {
		public:
			ReadSession() noexcept : stop_(false) {}

			// cannot copy
			ReadSession(const ReadSession &) = delete;
			ReadSession& operator=(const ReadSession &) = delete;

		private:
			friend class StreamBuffer;
			std::atomic_bool stop_;
		};

		enum class ReadResult {
			Ended,	/* 停止要求、キャプチャ停止、配信先の終了 */
			Failed,	/* USB ストリームの異常通知後、受信済みデータを配信し終えた */
		};

		StreamBuffer();
		~StreamBuffer();

		void SetThresholdSize(std::size_t size) noexcept;
		bool Alloc(std::size_t size);
		void Start() noexcept;
		void Stop() noexcept;
		/* 実行中の全読み出しを止める。読み出し開始前の要求は残らない。 */
		void StopRequest() noexcept;
		void StopRequest(ReadSession &session) noexcept;
		/* USB ストリームを共有する全受信機へ、転送層が受信を続けられないことを知らせる。
		 * Start() で解除する。 */
		void NotifyStreamFailure() noexcept;
		/* USB から受け取った TS パケット数 (TSMF の選別より前)。 */
		void ResetInputPacketCount() noexcept;
		std::uint64_t GetInputPacketCount() const noexcept;
		/* 受信統計 (入力 packet 数、CC 欠落、TEI、同期外れ、無受信時間、ビットレート) を
		 * 初期化する。選局時と PURGE で呼ぶ。 */
		/* keep_tune_timing は PURGE 用。選局直後の PURGE で、計測中の選局時間内訳を消さない。 */
		void ResetStats(bool keep_tune_timing = false) noexcept;
		/* 選局の時間内訳を次の最初の TS packet まで保持する。freq_ms は周波数設定まで、
		 * lock_ms はロック確認までの時間 (start_ms からの経過)。 */
		void ArmTuneTiming(std::uint64_t start_ms, std::uint64_t freq_ms,
				   std::uint64_t lock_ms, px4::SystemType system,
				   std::uint32_t freq) noexcept;
		void CancelTuneTiming() noexcept;
		bool Write(const void *buf, std::size_t &size) noexcept;
		void NotifyWrite() noexcept;
		ReadResult HandleRead(std::size_t buf_size, std::function<bool(const void *buf, std::size_t size)> handler, ReadSession *session = nullptr);
		bool Purge() noexcept;
		bool Purge(std::chrono::steady_clock::time_point deadline) noexcept;
		// J.83B を除く ISDB-C の選局から次の選局までの間だけ、TSMF の指定した TS を選び、ヘッダーのない搬送波は TS0 の指定でそのまま出力する
		bool StartTsmf(std::uint32_t stream_index, bool discard_null_packets, unsigned int stale_packets) noexcept;
		void StopTsmf() noexcept;
		void RestartTsmf() noexcept;
		tsmf_filter_state GetTsmfState() noexcept;

	private:
		struct TuneTiming {
			bool pending;
			std::uint64_t start_ms;
			std::uint64_t freq_ms;
			std::uint64_t lock_ms;
			px4::SystemType system;
			std::uint32_t freq;
		};
		/* Write() から報告へ渡す値。ロックの外で出力するため、ロック内では組み立てるだけにする。 */
		struct WriteReport {
			bool stats_due;
			px4::TsStatsSnapshot stats;
			std::uint64_t elapsed_ms;
			std::uint64_t input_bytes_delta;
			std::uint64_t stats_generation;
			bool tune_due;
			TuneTiming tune;
			std::uint64_t tune_first_ms;
		};

		bool WriteFiltered(const void *buf, std::size_t &size, std::uint64_t packets, WriteReport &report) noexcept;
		/* tsmf_mtx_ の内側で呼ぶ。size はフィルタ前の入力長。 */
		void UpdateStatsLocked(const void *buf, std::size_t size, std::uint64_t packets, WriteReport &report) noexcept;
		void LogStats(const WriteReport &report) noexcept;
		void LogTune(const WriteReport &report) noexcept;

		/* 待機条件の確認と待機だけに使い、配信先への書き込み中は保持しない。 */
		std::mutex mtx_;
		/* 同じバッファを読む接続が重なっても、リングバッファからの複写を1つずつにする。配信中は保持しない。 */
		std::mutex read_mtx_;
		px4::RingBuffer ringbuf_;
		/* Stop() と引数なしの StopRequest() で進む。mtx_ で保護する。 */
		std::uint64_t stop_generation_;
		std::atomic_bool stream_failed_;
		std::atomic_uint64_t input_packet_count_;
		std::condition_variable cond_;
		std::size_t write_size_;
		std::size_t threshold_size_;

		std::atomic_bool tsmf_;
		/* TSMF の選別と、受信統計・選局の時間内訳を保護する。 */
		std::mutex tsmf_mtx_;
		// TLV の蓄積領域を含めて大きいため、J.83B を除く ISDB-C を選局した受信機だけに確保する
		std::unique_ptr<tsmf_filter> tsmf_filter_;
		px4::TsStatsCounter ts_stats_;
		std::uint64_t stats_report_ms_;
		std::uint64_t stats_report_input_bytes_;
		std::uint64_t stats_generation_;
		TuneTiming tune_timing_;
	};

	ReceiverBase(unsigned int options);
	virtual ~ReceiverBase();

	// cannot copy
	ReceiverBase(const ReceiverBase &) = delete;
	ReceiverBase& operator=(const ReceiverBase &) = delete;

	// cannot move
	ReceiverBase(ReceiverBase &&) = delete;
	ReceiverBase& operator=(ReceiverBase &&) = delete;

	bool GetParameters(px4::command::ParameterSet &param_set) noexcept;
	bool SetParameters(const px4::command::ParameterSet &param_set) noexcept;
	void ClearParameters() noexcept;
	bool Tune(std::uint32_t timeout);
	bool ReadStats(px4::command::StatSet &stat_set);
	std::shared_ptr<StreamBuffer> GetStreamBuffer() noexcept { return stream_buf_; }

	virtual int Open() = 0;
	virtual void Close() = 0;
	virtual int CheckLock(bool &locked) = 0;
	virtual int SetLnbVoltage(std::int32_t voltage) = 0;
	virtual int SetCapture(bool capture) = 0;
	virtual int ReadStat(px4::command::StatType type, std::int32_t &value) = 0;

protected:
	/* 機種固有の操作全体に要求期限を適用する場合だけ処理する。 */
	virtual bool TuneOverride(std::uint32_t, bool &) { return false; }
	/* 機種固有の要求の検査を周波数設定より先に行う。既存機種は変更なし。 */
	virtual int PrepareTune() { return 0; }
	virtual int SetFrequency() = 0;
	virtual int SetStreamId() = 0;

	unsigned int options_;
#define RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE	0x00000010
#define RECEIVER_SAT_SET_STREAM_ID_AFTER_TUNE	0x00000020
#define RECEIVER_WAIT_AFTER_LOCK		0x00000040
#define RECEIVER_WAIT_AFTER_LOCK_TC_T		0x00000080

	Parameters params_;
	std::shared_ptr<StreamBuffer> stream_buf_;

private:
	bool TuneIsdbC(std::uint32_t timeout, std::uint64_t tune_start_ms);
	void LogTuneFailure(std::uint64_t tune_start_ms, int result) noexcept;

	std::mutex lock_;
};

class ReceiverError : public std::runtime_error {
public:
	explicit ReceiverError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
