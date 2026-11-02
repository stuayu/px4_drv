#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "msg.h"
#include "receiver_base.hpp"
/* 登録表のロックを試験側から保持し、検索がそのロック下で行われることを確かめる。 */
#define private public
#include "receiver_manager.hpp"
#undef private

/*
 * 共通の StreamBuffer を単体で動かし、データ接続の停止要求・USB 異常の通知と
 * 配信 handler のロック範囲を検証する。待機の上限は失敗を検出するためだけに使う。
 */

namespace {

using StreamBuffer = px4::ReceiverBase::StreamBuffer;
using ReadSession = StreamBuffer::ReadSession;
using ReadResult = StreamBuffer::ReadResult;

constexpr auto kReturnLimit = std::chrono::seconds(2);
constexpr std::size_t kPacket = 188;

std::shared_ptr<StreamBuffer> MakeBuffer(bool start)
{
	auto buffer = std::make_shared<StreamBuffer>();
	if (!buffer->Alloc(kPacket * 64))
		return nullptr;
	buffer->SetThresholdSize(1);
	if (start)
		buffer->Start();
	return buffer;
}

void WritePackets(StreamBuffer &buffer, unsigned int count, std::uint8_t first)
{
	std::vector<std::uint8_t> data(kPacket * count, 0);
	for (unsigned int i = 0; i < count; ++i) {
		data[i * kPacket] = 0x47;
		data[i * kPacket + 1] = static_cast<std::uint8_t>(first + i);
	}
	std::size_t size = data.size();
	buffer.Write(data.data(), size);
	buffer.NotifyWrite();
}

/* ring buffer がTS packetの途中までしか受け入れない場合、成功を返さない。 */
bool ShortRingWriteReportsFailure()
{
	StreamBuffer buffer;
	if (!buffer.Alloc(250))
		return false;
	buffer.Start();
	std::uint8_t packet[kPacket] = {};
	packet[0] = 0x47;
	std::size_t first_size = sizeof(packet);
	const bool first_ret = buffer.Write(packet, first_size);
	std::size_t second_size = sizeof(packet);
	const bool second_ret = buffer.Write(packet, second_size);
	buffer.Stop();
	const bool ok = first_ret && first_size == sizeof(packet) &&
		!second_ret && second_size == 62;
	if (!ok)
		std::fprintf(stderr, "short ring write: first=(%d,%zu) second=(%d,%zu)\n",
			first_ret, first_size, second_ret, second_size);
	return ok;
}

/* TSMFを選別しない通常TS経路では、PIDとscrambling controlを含むpacketを保存する。 */
bool ScrambledPacketPassesUnchanged()
{
	StreamBuffer buffer;
	if (!buffer.Alloc(kPacket * 2))
		return false;
	buffer.SetThresholdSize(1);
	buffer.Start();

	std::uint8_t packet[kPacket];
	for (std::size_t i = 0; i < sizeof(packet); i++)
		packet[i] = static_cast<std::uint8_t>((i * 23 + 5) & 0xff);
	packet[0] = 0x47;
	packet[1] = 0x5a;
	packet[2] = 0xbc;
	packet[3] = 0xd1;
	std::size_t write_size = sizeof(packet);
	const bool write_ok = buffer.Write(packet, write_size) &&
		write_size == sizeof(packet);
	buffer.NotifyWrite();

	ReadSession session;
	std::vector<std::uint8_t> received;
	const ReadResult read_result = buffer.HandleRead(kPacket,
		[&](const void *data, std::size_t size) {
			const auto *bytes = static_cast<const std::uint8_t *>(data);
			received.assign(bytes, bytes + size);
			buffer.StopRequest(session);
			return true;
	}, &session);
	buffer.Stop();
	const bool ok = write_ok && read_result == ReadResult::Ended &&
		received == std::vector<std::uint8_t>(packet, packet + sizeof(packet));
	if (!ok)
		std::fprintf(stderr, "scrambled packet was filtered or changed on the ordinary TS path\n");
	return ok;
}

/* TSMF の選別は一度の Write() に複数 TS packet が含まれても全 packet を処理する。 */
bool TsmfFiltersEveryPacketInBatch()
{
	constexpr std::size_t detect_packets = 53 * 8;
	constexpr std::size_t input_packets = detect_packets + 53;
	constexpr std::size_t output_packets = 52;
	StreamBuffer buffer;
	if (!buffer.Alloc(kPacket * input_packets))
		return false;
	buffer.SetThresholdSize(1);
	buffer.Start();
	if (!buffer.StartTsmf(0, false, 0)) {
		buffer.Stop();
		return false;
	}

	std::vector<std::uint8_t> input(kPacket * input_packets);
	for (std::size_t i = 0; i < input_packets; ++i) {
		auto *packet = input.data() + i * kPacket;
		for (std::size_t j = 0; j < kPacket; ++j)
			packet[j] = static_cast<std::uint8_t>((i + j) & 0xff);
		packet[0] = 0x47;
		packet[1] = 0x01; // PID 0x0100-0x01ff, not a TSMF header or NULL packet.
		packet[2] = static_cast<std::uint8_t>(i);
	}

	std::size_t size = input.size();
	const bool write_ok = buffer.Write(input.data(), size);
	const bool selected = buffer.GetTsmfState() == TSMF_FILTER_FOUND;
	const bool complete_frame = size == kPacket * output_packets;
	if (!write_ok || !selected || !complete_frame) {
		buffer.Stop();
		std::fprintf(stderr, "TSMF batch: write=%d selected=%d output_size=%zu\n",
			write_ok, selected, size);
		return false;
	}

	buffer.NotifyWrite();
	ReadSession session;
	std::vector<std::uint8_t> received;
	const ReadResult read_result = buffer.HandleRead(kPacket * output_packets,
		[&](const void *data, std::size_t read_size) {
			const auto *bytes = static_cast<const std::uint8_t *>(data);
			received.assign(bytes, bytes + read_size);
			buffer.StopRequest(session);
			return true;
	}, &session);
	buffer.Stop();

	const auto first = input.begin() + static_cast<std::ptrdiff_t>((detect_packets - 1) * kPacket);
	const auto last = first + static_cast<std::ptrdiff_t>(output_packets * kPacket);
	const bool ok = read_result == ReadResult::Ended &&
		received == std::vector<std::uint8_t>(first, last);
	if (!ok)
		std::fprintf(stderr, "TSMF batch: selected packets were not preserved\n");
	return ok;
}

/* TSMF は 188 byte 未満・端数付きの入力を packet として読まず、失敗を返す。 */
bool TsmfRejectsPartialPacket()
{
	StreamBuffer buffer;
	if (!buffer.Alloc(kPacket * 2))
		return false;
	buffer.Start();
	if (!buffer.StartTsmf(0, false, 0)) {
		buffer.Stop();
		return false;
	}

	std::uint8_t partial[kPacket - 1] = {};
	partial[0] = 0x47;
	std::size_t size = sizeof(partial);
	const bool write_ok = buffer.Write(partial, size);
	buffer.Stop();
	if (write_ok || size != 0)
		std::fprintf(stderr, "TSMF partial packet: write=%d size=%zu\n", write_ok, size);
	return !write_ok && size == 0;
}

/* PURGE の前に蓄積した TSMF packet を、PURGE 後のフレームへ混ぜない。 */
bool TsmfPurgeDropsPartialFrame()
{
	constexpr std::size_t detect_packets = 53 * 8;
	constexpr std::size_t before_purge_packets = 20;
	constexpr std::size_t after_purge_packets = 52 - before_purge_packets;
	StreamBuffer buffer;
	if (!buffer.Alloc(kPacket * 64))
		return false;
	buffer.Start();
	if (!buffer.StartTsmf(0, false, 0)) {
		buffer.Stop();
		return false;
	}

	auto make_packets = [](std::size_t first, std::size_t count) {
		std::vector<std::uint8_t> packets(kPacket * count, 0);
		for (std::size_t i = 0; i < count; ++i) {
			auto *packet = packets.data() + i * kPacket;
			packet[0] = 0x47;
			packet[1] = 0x01;
			packet[2] = static_cast<std::uint8_t>(first + i);
		}
		return packets;
	};
	auto before = make_packets(0, detect_packets + before_purge_packets - 1);
	std::size_t before_size = before.size();
	const bool before_ok = buffer.Write(before.data(), before_size) && before_size == 0;
	const bool purge_ok = buffer.Purge();
	auto after = make_packets(detect_packets + before_purge_packets - 1,
		after_purge_packets);
	std::size_t after_size = after.size();
	const bool after_ok = buffer.Write(after.data(), after_size);
	buffer.Stop();
	if (!before_ok || !purge_ok || !after_ok || after_size != 0)
		std::fprintf(stderr, "TSMF purge: before=%d purge=%d after=%d output=%zu\n",
			before_ok, purge_ok, after_ok, after_size);
	return before_ok && purge_ok && after_ok && after_size == 0;
}

/* USB 側の Write 中に NotifyWrite が write_size_ を同時に更新しない。 */
bool TsmfNotifyWriteConcurrentWithBatch()
{
	StreamBuffer buffer;
	constexpr std::size_t input_packets = 100000;
	if (!buffer.Alloc(kPacket * input_packets))
		return false;
	buffer.SetThresholdSize(1);
	buffer.Start();
	if (!buffer.StartTsmf(0, false, 0)) {
		buffer.Stop();
		return false;
	}

	std::vector<std::uint8_t> packets(kPacket * input_packets, 0);
	for (std::size_t i = 0; i < input_packets; ++i) {
		auto *packet = packets.data() + i * kPacket;
		packet[0] = 0x47;
		packet[1] = 0x01;
		packet[2] = static_cast<std::uint8_t>(i);
	}
	std::atomic_bool start{false};
	std::thread writer([&] {
		while (!start.load(std::memory_order_acquire))
			std::this_thread::yield();
		std::size_t size = packets.size();
		buffer.Write(packets.data(), size);
		buffer.NotifyWrite();
	});
	std::thread tuner([&] {
		while (!start.load(std::memory_order_acquire))
			std::this_thread::yield();
		while (buffer.GetInputPacketCount() < input_packets)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		buffer.NotifyWrite();
	});
	start.store(true, std::memory_order_release);
	writer.join();
	tuner.join();
	buffer.Stop();
	return true;
}

template <typename F>
bool ReturnsWithin(F &&function)
{
	auto result = std::async(std::launch::async, std::forward<F>(function));
	return result.wait_for(kReturnLimit) == std::future_status::ready;
}

/* SET_DATA_ID 直後の切断では、読み出しスレッドより先に停止要求が届く。 */
bool StopBeforeReadStarts()
{
	auto buffer = MakeBuffer(true);
	ReadSession session;
	buffer->StopRequest(session);
	ReadResult result = ReadResult::Failed;
	auto reader = std::async(std::launch::async, [&] {
		result = buffer->HandleRead(kPacket, [](const void *, std::size_t) { return true; }, &session);
	});
	bool ok = reader.wait_for(kReturnLimit) == std::future_status::ready;
	if (!ok) {
		buffer->Stop();
		reader.wait();
	}
	ok = ok && result == ReadResult::Ended;
	if (!ok)
		std::fprintf(stderr, "stop requested before HandleRead() was lost\n");
	return ok;
}

/* 待機条件の確認と待機の間に停止要求が入っても、読み出しが終わる。 */
bool StopRaceRepeated()
{
	auto buffer = MakeBuffer(true);
	for (unsigned int i = 0; i < 2000; ++i) {
		ReadSession session;
		std::thread reader([&] {
			buffer->HandleRead(kPacket, [](const void *, std::size_t) { return true; }, &session);
		});
		if (i & 1)
			std::this_thread::yield();
		buffer->StopRequest(session);
		auto joined = std::async(std::launch::async, [&] { reader.join(); });
		if (joined.wait_for(kReturnLimit) != std::future_status::ready) {
			std::fprintf(stderr, "stop request lost at iteration %u\n", i);
			buffer->Stop();
			joined.wait();
			return false;
		}
	}
	return true;
}

/* 別のデータ接続の停止要求は、この接続の読み出しを止めない。 */
bool SessionsAreIndependent()
{
	auto buffer = MakeBuffer(true);
	ReadSession current, previous;
	std::mutex mtx;
	std::condition_variable cond;
	std::size_t delivered = 0;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket * 64, [&](const void *, std::size_t size) {
			std::lock_guard<std::mutex> lock(mtx);
			delivered += size;
			cond.notify_all();
			return true;
		}, &current);
	});
	buffer->StopRequest(previous);
	WritePackets(*buffer, 2, 1);
	bool ok;
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return delivered == kPacket * 2; });
	}
	buffer->StopRequest(current);
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->Stop();
		std::fprintf(stderr, "another session's stop request ended this session\n");
	}
	return ok;
}

/* 配信先への書き込みが詰まっている間も、通知側は待たされない。 */
bool NotifiersDoNotWaitForDelivery()
{
	auto buffer = MakeBuffer(true);
	ReadSession session;
	std::mutex mtx;
	std::condition_variable cond;
	bool entered = false, release = false;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [&](const void *, std::size_t) {
			std::unique_lock<std::mutex> lock(mtx);
			entered = true;
			cond.notify_all();
			cond.wait(lock, [&] { return release; });
			return true;
		}, &session);
	});
	WritePackets(*buffer, 1, 1);
	bool ok;
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return entered; });
	}
	ok = ok && ReturnsWithin([&] { WritePackets(*buffer, 1, 2); });
	ok = ok && ReturnsWithin([&] { buffer->NotifyStreamFailure(); });
	ok = ok && ReturnsWithin([&] { buffer->StopRequest(session); });
	{
		std::lock_guard<std::mutex> lock(mtx);
		release = true;
		cond.notify_all();
	}
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->Stop();
		std::fprintf(stderr, "notifier waited for a blocked delivery\n");
	}
	return ok;
}

/* USB 異常の通知後も受信済みのデータを配信し、その後に異常として戻る。 */
bool FailureDrainsThenReports()
{
	auto buffer = MakeBuffer(true);
	ReadSession session;
	WritePackets(*buffer, 3, 1);
	buffer->NotifyStreamFailure();
	std::vector<std::uint8_t> received;
	ReadResult result = ReadResult::Ended;
	bool ok = ReturnsWithin([&] {
		result = buffer->HandleRead(kPacket, [&](const void *data, std::size_t size) {
			const auto *p = static_cast<const std::uint8_t *>(data);
			received.insert(received.end(), p, p + size);
			return true;
		}, &session);
	});
	if (!ok)
		buffer->Stop();
	ok = ok && result == ReadResult::Failed && received.size() == kPacket * 3 &&
		received[1] == 1 && received[kPacket + 1] == 2 && received[kPacket * 2 + 1] == 3;
	if (!ok)
		std::fprintf(stderr, "failure did not drain then report (bytes=%zu)\n", received.size());
	return ok;
}

/* 受信待ちの読み出しも USB 異常の通知で起床する。 */
bool FailureWakesWaitingReader()
{
	auto buffer = MakeBuffer(true);
	ReadSession session;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [](const void *, std::size_t) { return true; }, &session);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	buffer->NotifyStreamFailure();
	bool ok = reader.wait_for(kReturnLimit) == std::future_status::ready;
	if (!ok) {
		buffer->Stop();
		reader.wait();
	}
	ok = ok && reader.get() == ReadResult::Failed;
	if (!ok)
		std::fprintf(stderr, "waiting reader was not woken by a stream failure\n");
	return ok;
}

/*
 * キャプチャ開始前の受信待ちは、前回の USB ストリームの異常で終わらない。
 * Start() で異常状態を消し、開始後のデータを配信する。
 */
bool FailureBeforeStartIsCleared()
{
	auto buffer = MakeBuffer(false);
	ReadSession session;
	std::mutex mtx;
	std::condition_variable cond;
	std::size_t delivered = 0;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [&](const void *, std::size_t size) {
			std::lock_guard<std::mutex> lock(mtx);
			delivered += size;
			cond.notify_all();
			return true;
		}, &session);
	});
	buffer->NotifyStreamFailure();
	bool ok = reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
	buffer->Start();
	WritePackets(*buffer, 1, 1);
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return delivered == kPacket; }) && ok;
	}
	buffer->StopRequest(session);
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->Stop();
		reader.wait();
	}
	ok = ok && reader.get() == ReadResult::Ended;
	if (!ok)
		std::fprintf(stderr, "failure before Start() leaked into the next capture\n");
	return ok;
}

/* キャプチャ停止は、実行中の読み出しを従来どおり終了させる。 */
bool CaptureStopEndsRunningRead()
{
	auto buffer = MakeBuffer(true);
	ReadSession session;
	std::mutex mtx;
	std::condition_variable cond;
	bool delivered = false;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [&](const void *, std::size_t) {
			std::lock_guard<std::mutex> lock(mtx);
			delivered = true;
			cond.notify_all();
			return true;
		}, &session);
	});
	WritePackets(*buffer, 1, 1);
	bool ok;
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return delivered; });
	}
	buffer->Stop();
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->StopRequest(session);
		reader.wait();
	}
	ok = ok && reader.get() == ReadResult::Ended;
	if (!ok)
		std::fprintf(stderr, "Stop() did not end a running read\n");
	return ok;
}

/* Windows のデータ接続が使う、セッションなしの呼び出しも従来どおり止まる。 */
bool LegacyStopRequestEndsRunningRead()
{
	auto buffer = MakeBuffer(true);
	std::mutex mtx;
	std::condition_variable cond;
	bool delivered = false;
	auto reader = std::async(std::launch::async, [&] {
		buffer->HandleRead(kPacket, [&](const void *, std::size_t) {
			std::lock_guard<std::mutex> lock(mtx);
			delivered = true;
			cond.notify_all();
			return true;
		});
	});
	WritePackets(*buffer, 1, 1);
	bool ok;
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return delivered; });
	}
	buffer->StopRequest();
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->Stop();
		reader.wait();
		std::fprintf(stderr, "legacy StopRequest() did not end a running read\n");
	}
	return ok;
}

/*
 * 受信機を閉じた後も前の接続の配信が詰まったまま残り、別の接続が同じ受信機を開き直す。
 * 前の接続の書き込みを待たずに、新しい接続へ TS を配信する。
 */
bool BlockedPreviousSessionDoesNotStarveNext()
{
	auto buffer = MakeBuffer(true);
	ReadSession previous, next;
	std::mutex mtx;
	std::condition_variable cond;
	bool entered = false, release = false;
	std::size_t delivered = 0;
	auto blocked = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [&](const void *, std::size_t) {
			std::unique_lock<std::mutex> lock(mtx);
			entered = true;
			cond.notify_all();
			cond.wait(lock, [&] { return release; });
			return true;
		}, &previous);
	});
	WritePackets(*buffer, 1, 1);
	bool ok;
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return entered; });
	}
	/* 制御接続の Close() と再度の SET_CAPTURE に相当する。 */
	buffer->Stop();
	buffer->Start();
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(kPacket, [&](const void *, std::size_t size) {
			std::lock_guard<std::mutex> lock(mtx);
			delivered += size;
			cond.notify_all();
			return true;
		}, &next);
	});
	WritePackets(*buffer, 2, 2);
	{
		std::unique_lock<std::mutex> lock(mtx);
		ok = cond.wait_for(lock, kReturnLimit, [&] { return delivered == kPacket * 2; }) && ok;
		release = true;
		cond.notify_all();
	}
	ok = blocked.wait_for(kReturnLimit) == std::future_status::ready && ok;
	ok = ok && blocked.get() == ReadResult::Ended;
	buffer->StopRequest(next);
	ok = reader.wait_for(kReturnLimit) == std::future_status::ready && ok;
	if (!ok) {
		buffer->Stop();
		std::fprintf(stderr, "blocked delivery of the previous session starved the next session\n");
	}
	return ok;
}

class FakeReceiver final : public px4::ReceiverBase {
public:
	FakeReceiver() : ReceiverBase(0) {}
	int Open() override { return 0; }
	void Close() override {}
	int CheckLock(bool &locked) override { locked = true; return 0; }
	int SetLnbVoltage(std::int32_t) override { return 0; }
	int SetCapture(bool) override { return 0; }
	int ReadStat(px4::command::StatType, std::int32_t &value) override { value = 0; return 0; }

protected:
	int SetFrequency() override { return 0; }
	int SetStreamId() override { return 0; }
};

class IsdbCTimingReceiver final : public px4::ReceiverBase {
public:
	IsdbCTimingReceiver() : ReceiverBase(0)
	{
		params_.system = px4::SystemType::ISDB_C;
		params_.freq = 473000;
	}
	int Open() override { return 0; }
	void Close() override {}
	int CheckLock(bool &locked) override
	{
		/* 最初の確認ではロック前に前の搬送波の TS が届いた状態を作る。 */
		if (++lock_checks_ == 1) {
			std::uint8_t packet[kPacket] = {};
			packet[0] = 0x47;
			std::size_t size = sizeof(packet);
			stream_buf_->Write(packet, size);
			locked = false;
			return 0;
		}
		locked = true;
		return 0;
	}
	int SetLnbVoltage(std::int32_t) override { return 0; }
	int SetCapture(bool) override { return 0; }
	int ReadStat(px4::command::StatType, std::int32_t &value) override { value = 0; return 0; }

protected:
	int SetFrequency() override { return 0; }
	int SetStreamId() override { return 0; }

private:
	unsigned int lock_checks_ = 0;
};

/* ISDB-C の選局では、ロック前に届いた TS を最初の TS として報告しない。 */
bool IsdbCTuneTimingStartsAtLock()
{
	char path[] = "/tmp/px4-tune-timing-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0)
		return false;
	close(fd);
	std::wstring wide_path;
	for (const char c : std::string(path))
		wide_path.push_back(static_cast<unsigned char>(c));
	msg_set_mode(MSG_MODE_LOG_FILE);
	if (!msg_open_file(wide_path.c_str())) {
		std::remove(path);
		return false;
	}

	IsdbCTimingReceiver receiver;
	bool tuned = receiver.Tune(1000);
	std::uint8_t packet[kPacket] = {};
	packet[0] = 0x47;
	std::size_t size = sizeof(packet);
	receiver.GetStreamBuffer()->Write(packet, size);
	msg_close_file();
	msg_set_mode(MSG_MODE_NONE);

	std::ifstream log(path);
	std::string line;
	unsigned int timing_lines = 0;
	while (std::getline(log, line))
		if (line.find("Tune timing:") != std::string::npos)
			timing_lines++;
	log.close();
	std::remove(path);
	if (!tuned || timing_lines != 1)
		std::fprintf(stderr, "ISDB-C tune timing: tuned=%d timing_lines=%u\n", tuned, timing_lines);
	return tuned && timing_lines == 1;
}

bool OpenFake(px4::ReceiverManager &manager, FakeReceiver &receiver, std::uint32_t &data_id)
{
	px4::command::ReceiverInfo info{}, key{}, opened{};
	key.index = -1;
	return manager.Register(info, &receiver) &&
		manager.SearchAndOpen(key, opened, data_id) == &receiver;
}

/* データ ID は1回だけ使え、登録解除後の受信機からはバッファを返さない。 */
bool DataIdLookupContract()
{
	px4::ReceiverManager manager;
	std::unique_ptr<FakeReceiver> receiver(new FakeReceiver());
	std::uint32_t data_id = 0;
	bool ok = OpenFake(manager, *receiver, data_id);
	auto buffer = ok ? manager.SearchStreamBufferByDataId(data_id) : nullptr;
	ok = ok && buffer && buffer == receiver->GetStreamBuffer();
	ok = ok && !manager.SearchStreamBufferByDataId(data_id) && !manager.SearchByDataId(data_id);

	FakeReceiver other;
	std::uint32_t other_id = 0;
	ok = ok && OpenFake(manager, other, other_id);
	ok = ok && manager.Unregister(&other) && !manager.SearchStreamBufferByDataId(other_id);

	/* 取得後に受信機が解放されても、バッファは使い続けられる (ASan で検出)。 */
	manager.Unregister(receiver.get());
	receiver.reset();
	if (ok) {
		buffer->Alloc(kPacket * 4);
		buffer->Start();
		WritePackets(*buffer, 1, 1);
		ok = buffer->GetInputPacketCount() == 1;
	}
	if (!ok)
		std::fprintf(stderr, "data id lookup contract failed\n");
	return ok;
}

/* 登録解除が登録表のロックを持つ間は、検索もバッファの取得もしない。 */
bool DataIdLookupHoldsRegistrationLock()
{
	px4::ReceiverManager manager;
	FakeReceiver receiver;
	std::uint32_t data_id = 0;
	if (!OpenFake(manager, receiver, data_id))
		return false;
	std::unique_lock<std::shared_mutex> lock(manager.mtx_);
	auto lookup = std::async(std::launch::async, [&] { return manager.SearchStreamBufferByDataId(data_id); });
	bool ok = lookup.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
	lock.unlock();
	ok = lookup.wait_for(kReturnLimit) == std::future_status::ready && ok;
	ok = ok && lookup.get() == receiver.GetStreamBuffer();
	if (!ok)
		std::fprintf(stderr, "data id lookup did not wait for the registration lock\n");
	return ok;
}

} // namespace

int main()
{
	bool ok = true;
	ok = StopBeforeReadStarts() && ok;
	ok = StopRaceRepeated() && ok;
	ok = SessionsAreIndependent() && ok;
	ok = NotifiersDoNotWaitForDelivery() && ok;
	ok = FailureDrainsThenReports() && ok;
	ok = FailureWakesWaitingReader() && ok;
	ok = FailureBeforeStartIsCleared() && ok;
	ok = CaptureStopEndsRunningRead() && ok;
	ok = LegacyStopRequestEndsRunningRead() && ok;
	ok = BlockedPreviousSessionDoesNotStarveNext() && ok;
	ok = DataIdLookupContract() && ok;
	ok = DataIdLookupHoldsRegistrationLock() && ok;
	ok = IsdbCTuneTimingStartsAtLock() && ok;
	ok = ShortRingWriteReportsFailure() && ok;
	ok = ScrambledPacketPassesUnchanged() && ok;
	ok = TsmfFiltersEveryPacketInBatch() && ok;
	ok = TsmfRejectsPartialPacket() && ok;
	ok = TsmfPurgeDropsPartialFrame() && ok;
	ok = TsmfNotifyWriteConcurrentWithBatch() && ok;
	if (ok)
		std::puts("stream_buffer_test: passed");
	return ok ? 0 : 1;
}
