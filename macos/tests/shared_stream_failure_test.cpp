#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "device_base.hpp"
/* 機種の参照数と受信機の状態を直接確かめる。 */
#define private public
#define protected public
#include "px4_device.hpp"
#include "pxmlt_device.hpp"
#include "isdb2056_device.hpp"
#include "isdbt2071_device.hpp"
#undef protected
#undef private

/*
 * PX4 系と PXMLT 系で複数の受信機が1つの USB ストリームを共有する場合の、
 * 全転送を失った後の開始要求を検証する。USB 転送層と IT930x の制御応答だけを
 * 模擬し、受信機の SetCapture()、機種の参照数、stream handler は実装を使う。
 */

namespace {

std::uint16_t g_vendor;
std::uint16_t g_product;
/* 直前に作った機種の device。IT930x の模擬 bus へ渡す。 */
device *g_device;

} // namespace

namespace px4 {

/* USB を開かず、機種判定に使う VID と PID だけを与える。 */
DeviceBase::DeviceBase(const std::wstring &, const DeviceDefinition &definition,
		       std::uintptr_t, ReceiverManager &manager)
	: device_def_(definition), receiver_manager_(manager)
{
	std::memset(&dev_, 0, sizeof(dev_));
	std::memset(&usb_dev_, 0, sizeof(usb_dev_));
	usb_dev_.descriptor.idVendor = g_vendor;
	usb_dev_.descriptor.idProduct = g_product;
	g_device = &dev_;
}

DeviceBase::~DeviceBase() = default;

} // namespace px4

namespace {

using StreamBuffer = px4::ReceiverBase::StreamBuffer;
using ReadSession = StreamBuffer::ReadSession;
using ReadResult = StreamBuffer::ReadResult;

constexpr auto kReturnLimit = std::chrono::seconds(2);

/* 模擬 USB 転送層。開始・停止の回数と、制御転送の途中で差し込む処理を持つ。 */
struct MockBus {
	std::atomic_int starts{0};
	std::atomic_int stops{0};
	std::atomic_bool streaming{false};
	/* 開始から戻る前に全転送を失った通知を送り、受信機の Start() より先に届く順序を作る。 */
	std::atomic_bool fail_on_start{false};
	std::mutex hook_mtx;
	std::function<void()> on_ctrl_tx;
	std::uint8_t seq = 0;
};

MockBus g_bus;

void ResetBus()
{
	g_bus.starts = 0;
	g_bus.stops = 0;
	g_bus.streaming = false;
	g_bus.fail_on_start = false;
	std::lock_guard<std::mutex> lock(g_bus.hook_mtx);
	g_bus.on_ctrl_tx = nullptr;
}

std::uint16_t Checksum(const std::uint8_t *b, std::size_t len)
{
	std::uint16_t c = 0;
	for (std::size_t i = 0; i + 1 < len; i += 2)
		c += static_cast<std::uint16_t>((b[i] << 8) | b[i + 1]);
	if (len % 2)
		c += static_cast<std::uint16_t>(b[len - 1] << 8);
	return static_cast<std::uint16_t>(~c);
}

int MockCtrlTx(struct itedtv_bus *, void *buf, int)
{
	g_bus.seq = static_cast<std::uint8_t *>(buf)[3];
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> lock(g_bus.hook_mtx);
		hook.swap(g_bus.on_ctrl_tx);
	}
	if (hook)
		hook();
	return 0;
}

/* IT930x の成功応答。読み出しはすべて 0 を返す。 */
int MockCtrlRx(struct itedtv_bus *, void *buf, int *len)
{
	constexpr int kPayload = 64;
	const int rlen = 3 + kPayload + 2;
	auto *b = static_cast<std::uint8_t *>(buf);
	std::memset(b, 0, rlen);
	b[0] = static_cast<std::uint8_t>(rlen - 1);
	b[1] = g_bus.seq;
	b[2] = 0;
	const std::uint16_t csum = Checksum(&b[1], rlen - 1 - 2);
	b[rlen - 2] = static_cast<std::uint8_t>(csum >> 8);
	b[rlen - 1] = static_cast<std::uint8_t>(csum);
	*len = rlen;
	return 0;
}

/* PSB の掃き出しは時間切れとして扱い、既存の許容経路を通す。 */
int MockStreamRx(struct itedtv_bus *, void *, int *len, int)
{
	*len = 0;
	return -ETIMEDOUT;
}

int MockStartStreaming(struct itedtv_bus *, itedtv_bus_stream_handler_t handler, void *context)
{
	g_bus.starts++;
	g_bus.streaming = true;
	if (g_bus.fail_on_start)
		handler(context, nullptr, 0);
	return 0;
}

int MockStopStreaming(struct itedtv_bus *)
{
	g_bus.stops++;
	g_bus.streaming = false;
	return 0;
}

bool SetupBridge(struct it930x_bridge &it930x, device &dev)
{
	it930x.dev = &dev;
	it930x.bus.dev = &dev;
	it930x.bus.type = ITEDTV_BUS_USB;
	it930x.bus.ops.ctrl_tx = MockCtrlTx;
	it930x.bus.ops.ctrl_rx = MockCtrlRx;
	it930x.bus.ops.stream_rx = MockStreamRx;
	it930x.bus.ops.start_streaming = MockStartStreaming;
	it930x.bus.ops.stop_streaming = MockStopStreaming;
	return it930x_init(&it930x) == 0;
}

/* 読み出しが異常で終わるか、待ち続けるかを判定する。待ち続けた場合は停止して回収する。 */
bool ReadFails(const std::shared_ptr<StreamBuffer> &buffer)
{
	ReadSession session;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(188, [](const void *, std::size_t) { return true; }, &session);
	});
	if (reader.wait_for(kReturnLimit) != std::future_status::ready) {
		buffer->StopRequest(session);
		reader.wait();
		return false;
	}
	return reader.get() == ReadResult::Failed;
}

/* 開始していないバッファは書き込みを受け付けない。 */
bool Started(const std::shared_ptr<StreamBuffer> &buffer)
{
	std::uint8_t packet[188] = {0x47};
	std::size_t size = sizeof(packet);
	const bool written = buffer->Write(packet, size);
	if (written)
		buffer->Purge();
	return written;
}

/* 受信機 id (1 始まり) の識別子を同期バイトへ持つ PX4 系の TS。 */
std::vector<std::uint8_t> TaggedPackets(unsigned int id, unsigned int count)
{
	std::vector<std::uint8_t> data(188 * count, 0);
	for (unsigned int i = 0; i < count; ++i) {
		data[i * 188] = static_cast<std::uint8_t>(0x07 | (id << 4));
		data[i * 188 + 1] = static_cast<std::uint8_t>(i + 1);
	}
	return data;
}

/* 受信機のバッファへ届いた packet 数を読み出しで確かめる。 */
bool Delivers(const std::shared_ptr<StreamBuffer> &buffer, std::size_t packets)
{
	ReadSession session;
	std::mutex mtx;
	std::size_t received = 0;
	auto reader = std::async(std::launch::async, [&] {
		return buffer->HandleRead(188 * 16, [&](const void *, std::size_t size) {
			std::lock_guard<std::mutex> lock(mtx);
			received += size;
			return received < 188 * packets;
		}, &session);
	});
	bool ok = reader.wait_for(kReturnLimit) == std::future_status::ready;
	if (!ok) {
		buffer->StopRequest(session);
		reader.wait();
	}
	std::lock_guard<std::mutex> lock(mtx);
	return ok && received == 188 * packets;
}

struct Px4Fixture {
	px4::DeviceDefinition definition{};
	px4::ReceiverManager manager;
	std::unique_ptr<px4::Px4Device> device;
	bool ready = false;

	Px4Fixture()
	{
		g_vendor = 0x0511;
		g_product = 0x083f;
		ResetBus();
		device.reset(new px4::Px4Device(L"", definition, 0, manager));
		device->config_.device.receiver_max_packets = 64;
		device->config_.device.psb_purge_timeout = 0;
		if (!SetupBridge(device->it930x_, *g_device))
			return;
		for (int i = 0; i < 4; ++i) {
			device->it930x_.config.input[i].i2c_bus = (i & 1) ? 2 : 3;
			device->it930x_.config.input[i].i2c_addr = static_cast<std::uint8_t>(0x10 + i * 2);
		}
		for (int i = 0; i < 4; ++i) {
			device->receivers_[i].reset(new px4::Px4Device::Px4Receiver(*device, i));
			device->receivers_[i]->init_ = true;
			device->receivers_[i]->open_ = true;
			device->stream_ctx_.stream_buf[i] = device->receivers_[i]->GetStreamBuffer();
		}
		ready = true;
	}

	~Px4Fixture()
	{
		if (!device)
			return;
		for (int i = 0; i < 4; ++i) {
			auto &receiver = device->receivers_[i];
			if (!receiver)
				continue;
			if (receiver->streaming_)
				receiver->SetCapture(false);
			receiver->open_ = false;
		}
		for (int i = 0; i < 4; ++i) {
			device->stream_ctx_.stream_buf[i] = nullptr;
			device->receivers_[i].reset();
		}
		if (ready)
			it930x_term(&device->it930x_);
	}

	px4::Px4Device::Px4Receiver &Receiver(int i) { return *device->receivers_[i]; }
	std::shared_ptr<StreamBuffer> Buffer(int i) { return device->receivers_[i]->GetStreamBuffer(); }

	void Fail() { px4::Px4Device::StreamHandler(device.get(), nullptr, 0); }

	void Feed(unsigned int id, unsigned int count)
	{
		auto data = TaggedPackets(id, count);
		px4::Px4Device::StreamHandler(device.get(), data.data(), static_cast<std::uint32_t>(data.size()));
	}
};

struct PxMltFixture {
	px4::DeviceDefinition definition{};
	px4::ReceiverManager manager;
	std::unique_ptr<px4::PxMltDevice> device;
	bool ready = false;

	PxMltFixture()
	{
		g_vendor = 0x0511;
		g_product = 0x024e;	/* PX-MLT5PE */
		ResetBus();
		device.reset(new px4::PxMltDevice(L"", definition, 0, manager));
		device->config_.device.receiver_max_packets = 64;
		device->config_.device.psb_purge_timeout = 0;
		if (!SetupBridge(device->it930x_, *g_device))
			return;
		for (int i = 0; i < device->receiver_num_; ++i) {
			device->receivers_[i].reset(new px4::PxMltDevice::PxMltReceiver(*device, i));
			device->receivers_[i]->open_ = true;
			device->stream_ctx_.stream_buf[i] = device->receivers_[i]->GetStreamBuffer();
		}
		ready = true;
	}

	~PxMltFixture()
	{
		if (!device)
			return;
		for (int i = 0; i < device->receiver_num_; ++i) {
			auto &receiver = device->receivers_[i];
			if (!receiver)
				continue;
			if (receiver->streaming_)
				receiver->SetCapture(false);
			receiver->open_ = false;
		}
		for (int i = 0; i < device->receiver_num_; ++i) {
			device->stream_ctx_.stream_buf[i] = nullptr;
			device->receivers_[i].reset();
		}
		if (ready)
			it930x_term(&device->it930x_);
	}

	px4::PxMltDevice::PxMltReceiver &Receiver(int i) { return *device->receivers_[i]; }
	std::shared_ptr<StreamBuffer> Buffer(int i) { return device->receivers_[i]->GetStreamBuffer(); }

	void Fail() { px4::PxMltDevice::StreamHandler(device.get(), nullptr, 0); }

	void Feed(unsigned int id, unsigned int count)
	{
		auto data = TaggedPackets(id, count);
		px4::PxMltDevice::StreamHandler(device.get(), data.data(), static_cast<std::uint32_t>(data.size()));
	}
};

/* 通常の 0x47 同期の TS。単一受信機の機種へ入力する。 */
std::vector<std::uint8_t> PlainPackets(unsigned int count)
{
	std::vector<std::uint8_t> data(188 * count, 0);
	for (unsigned int i = 0; i < count; ++i) {
		data[i * 188] = 0x47;
		data[i * 188 + 1] = static_cast<std::uint8_t>(i + 1);
	}
	return data;
}

struct Isdb2056Fixture {
	px4::DeviceDefinition definition{};
	px4::ReceiverManager manager;
	std::unique_ptr<px4::Isdb2056Device> device;
	bool ready = false;

	Isdb2056Fixture()
	{
		g_vendor = 0x0511;
		g_product = 0x004b;	/* ISDB2056 */
		ResetBus();
		device.reset(new px4::Isdb2056Device(L"", definition, 0, manager));
		device->config_.device.receiver_max_packets = 64;
		device->config_.device.psb_purge_timeout = 0;
		if (!SetupBridge(device->it930x_, *g_device))
			return;
		device->it930x_.config.input[0].i2c_bus = 2;
		device->receiver_.reset(new px4::Isdb2056Device::Isdb2056Receiver(*device, 0));
		device->receiver_->init_ = true;
		device->receiver_->open_ = true;
		/* 選局済みとして TS ピンの切り替えから SetCapture() を通す。 */
		device->receiver_->current_system_ = px4::SystemType::ISDB_T;
		device->stream_ctx_.stream_buf = device->receiver_->GetStreamBuffer();
		ready = true;
	}

	~Isdb2056Fixture()
	{
		if (!device)
			return;
		if (device->receiver_) {
			if (device->receiver_->streaming_)
				device->receiver_->SetCapture(false);
			device->receiver_->open_ = false;
		}
		device->stream_ctx_.stream_buf = nullptr;
		device->receiver_.reset();
		if (ready)
			it930x_term(&device->it930x_);
	}

	px4::Isdb2056Device::Isdb2056Receiver &Receiver(int) { return *device->receiver_; }
	std::shared_ptr<StreamBuffer> Buffer(int) { return device->receiver_->GetStreamBuffer(); }
	void Fail() { px4::Isdb2056Device::StreamHandler(device.get(), nullptr, 0); }
	void Feed(unsigned int, unsigned int count)
	{
		auto data = PlainPackets(count);
		px4::Isdb2056Device::StreamHandler(device.get(), data.data(), static_cast<std::uint32_t>(data.size()));
	}
};

struct Isdbt2071Fixture {
	px4::DeviceDefinition definition{};
	px4::ReceiverManager manager;
	std::unique_ptr<px4::Isdbt2071Device> device;
	bool ready = false;

	Isdbt2071Fixture()
	{
		g_vendor = 0x0511;
		g_product = 0x0052;
		ResetBus();
		device.reset(new px4::Isdbt2071Device(L"", definition, 0, manager));
		device->config_.device.receiver_max_packets = 64;
		device->config_.device.psb_purge_timeout = 0;
		if (!SetupBridge(device->it930x_, *g_device))
			return;
		device->it930x_.config.input[0].i2c_bus = 2;
		device->receiver_.reset(new px4::Isdbt2071Device::Isdbt2071Receiver(*device, 0));
		device->receiver_->init_ = true;
		device->receiver_->open_ = true;
		device->stream_ctx_.stream_buf = device->receiver_->GetStreamBuffer();
		ready = true;
	}

	~Isdbt2071Fixture()
	{
		if (!device)
			return;
		if (device->receiver_) {
			if (device->receiver_->streaming_)
				device->receiver_->SetCapture(false);
			device->receiver_->open_ = false;
		}
		device->stream_ctx_.stream_buf = nullptr;
		device->receiver_.reset();
		if (ready)
			it930x_term(&device->it930x_);
	}

	px4::Isdbt2071Device::Isdbt2071Receiver &Receiver(int) { return *device->receiver_; }
	std::shared_ptr<StreamBuffer> Buffer(int) { return device->receiver_->GetStreamBuffer(); }
	void Fail() { px4::Isdbt2071Device::StreamHandler(device.get(), nullptr, 0); }
	void Feed(unsigned int, unsigned int count)
	{
		auto data = PlainPackets(count);
		px4::Isdbt2071Device::StreamHandler(device.get(), data.data(), static_cast<std::uint32_t>(data.size()));
	}
};

#define EXPECT(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: %s: expected %s\n", __FILE__, __LINE__, name, #cond); return false; } } while (0)

/* 正常な複数チューナー受信: 1回だけ開始し、各受信機へ振り分け、最後の停止で1回だけ止める。 */
template <typename Fixture>
bool MultiTunerCapture(const char *name, int a, int b)
{
	Fixture f;
	EXPECT(f.ready);
	EXPECT(f.Receiver(a).SetCapture(true) == 0);
	EXPECT(f.Receiver(b).SetCapture(true) == 0);
	EXPECT(f.device->streaming_count_ == 2 && g_bus.starts == 1);
	f.Feed(a + 1, 4);
	f.Feed(b + 1, 8);
	EXPECT(Delivers(f.Buffer(a), 4));
	EXPECT(Delivers(f.Buffer(b), 8));
	EXPECT(f.Receiver(a).SetCapture(false) == 0);
	EXPECT(f.device->streaming_count_ == 1 && g_bus.stops == 0);
	EXPECT(f.Receiver(b).SetCapture(false) == 0);
	EXPECT(f.device->streaming_count_ == 0 && g_bus.stops == 1);
	return true;
}

/*
 * 全転送を失った後の追加開始は -EIO で失敗し、参照数と転送層の状態を変えない。
 * 既存の受信機は異常で終わり、全参照が閉じた後の開始は新しいストリームとして成功する。
 */
template <typename Fixture>
bool LateJoinerRejectedThenRecovers(const char *name, int a, int b)
{
	Fixture f;
	EXPECT(f.ready);
	EXPECT(f.Receiver(a).SetCapture(true) == 0);
	const auto open_count = f.device->open_count_;
	const auto lnb_count = f.device->lnb_power_count_;
	f.Fail();
	EXPECT(ReadFails(f.Buffer(a)));

	EXPECT(f.Receiver(b).SetCapture(true) == -EIO);
	EXPECT(!f.Receiver(b).streaming_);
	EXPECT(!Started(f.Buffer(b)));
	EXPECT(f.device->streaming_count_ == 1 && g_bus.starts == 1 && g_bus.stops == 0);
	EXPECT(f.device->open_count_ == open_count && f.device->lnb_power_count_ == lnb_count);

	/* 既存の受信機の停止で参照数が 0 になり、転送層を1回だけ止める。 */
	EXPECT(f.Receiver(a).SetCapture(false) == 0);
	EXPECT(f.device->streaming_count_ == 0 && g_bus.stops == 1);

	/* 全参照が閉じた後は、異常状態を消して新しいストリームを開始する。 */
	EXPECT(f.Receiver(b).SetCapture(true) == 0);
	EXPECT(f.device->streaming_count_ == 1 && g_bus.starts == 2);
	EXPECT(f.Receiver(a).SetCapture(true) == 0);
	EXPECT(f.device->streaming_count_ == 2 && g_bus.starts == 2);
	f.Feed(b + 1, 4);
	EXPECT(Delivers(f.Buffer(b), 4));
	EXPECT(f.Receiver(a).SetCapture(false) == 0);
	EXPECT(f.Receiver(b).SetCapture(false) == 0);
	EXPECT(f.device->streaming_count_ == 0 && g_bus.stops == 2);
	return true;
}

/*
 * PX4 系の開始要求は TS ピンを有効にしてから参照数を増やす。その制御転送の間に
 * 異常が届いた場合も開始を失敗させ、TS ピンを解除して保留状態を残さない。
 */
bool Px4FailureDuringStartRequest()
{
	const char *name = "Px4FailureDuringStartRequest";
	Px4Fixture f;
	EXPECT(f.ready);
	EXPECT(f.Receiver(0).SetCapture(true) == 0);
	int ctrl_after_failure = 0;
	{
		std::lock_guard<std::mutex> lock(g_bus.hook_mtx);
		g_bus.on_ctrl_tx = [&f] { f.Fail(); };
	}
	EXPECT(f.Receiver(2).SetCapture(true) == -EIO);
	{
		std::lock_guard<std::mutex> lock(g_bus.hook_mtx);
		ctrl_after_failure = g_bus.on_ctrl_tx ? -1 : 0;
	}
	EXPECT(ctrl_after_failure == 0);
	EXPECT(!f.Receiver(2).streaming_ && !f.Receiver(2).ts_pin_cleanup_pending_);
	EXPECT(f.device->streaming_count_ == 1 && g_bus.starts == 1);
	EXPECT(ReadFails(f.Buffer(0)));
	return true;
}

/*
 * StartCapture() の確認を通過した後、バッファの Start() より前に異常が届く順序。
 * Start() が異常状態を消しても、受信機は機種の状態を確かめ直して異常で終わる。
 */
template <typename Fixture>
bool FailureAfterJoinCheck(const char *name, int a, int b)
{
	Fixture f;
	EXPECT(f.ready);
	EXPECT(f.Receiver(a).SetCapture(true) == 0);
	EXPECT(f.device->StartCapture() == 0);
	f.Fail();
	auto buffer = f.Buffer(b);
	buffer->Alloc(188 * 64);
	buffer->Start();
	f.device->NotifyIfStreamFailed(*buffer);
	EXPECT(ReadFails(buffer));
	EXPECT(ReadFails(f.Buffer(a)));
	buffer->Stop();
	EXPECT(f.device->StopCapture() == 0);
	EXPECT(f.Receiver(a).SetCapture(false) == 0);
	EXPECT(f.device->streaming_count_ == 0 && g_bus.stops == 1);
	return true;
}

/*
 * 異常と開始要求を同時に実行する。開始が成功を返した場合は読み出しが異常で終わり、
 * 失敗した場合は -EIO で参照数を変えない。どちらの順序でも待ち続ける受信機を残さない。
 */
template <typename Fixture>
bool ConcurrentFailureAndStart(const char *name, int a, int b, unsigned int iterations)
{
	Fixture f;
	EXPECT(f.ready);
	unsigned int joined = 0, rejected = 0;
	for (unsigned int i = 0; i < iterations; ++i) {
		EXPECT(f.Receiver(a).SetCapture(true) == 0);
		/* 偶数回は異常側、奇数回は開始側を遅らせ、両方の前後関係を通す。 */
		const unsigned int delay = (i / 2 % 32) * 400;
		std::atomic_bool go{false};
		std::thread failure([&] {
			while (!go.load()) {}
			for (volatile unsigned int spin = 0; spin < ((i & 1) ? 0 : delay); ++spin) {}
			f.Fail();
		});
		go = true;
		for (volatile unsigned int spin = 0; spin < ((i & 1) ? delay : 0); ++spin) {}
		const int ret = f.Receiver(b).SetCapture(true);
		failure.join();
		if (ret == 0) {
			joined++;
			EXPECT(f.device->streaming_count_ == 2);
			EXPECT(ReadFails(f.Buffer(b)));
			EXPECT(f.Receiver(b).SetCapture(false) == 0);
		} else {
			rejected++;
			EXPECT(ret == -EIO);
			EXPECT(!f.Receiver(b).streaming_ && f.device->streaming_count_ == 1);
		}
		EXPECT(ReadFails(f.Buffer(a)));
		EXPECT(f.Receiver(a).SetCapture(false) == 0);
		EXPECT(f.device->streaming_count_ == 0);
		EXPECT(g_bus.starts == static_cast<int>(i + 1) && g_bus.stops == static_cast<int>(i + 1));
	}
	std::printf("%s: %u iterations, start succeeded then failed=%u, start rejected=%u\n",
		    name, iterations, joined, rejected);
	return true;
}

/*
 * 転送層が開始から戻った直後に全転送を失い、受信機がバッファを Start() する前に
 * 通知が届く順序。SetCapture(true) の後の読み出しは待ち続けず異常で終わる。
 * 停止して再び開始すると状態が消え、データを配信する。
 */
template <typename Fixture>
bool FailureRightAfterBusStart(const char *name, int index, unsigned int id)
{
	Fixture f;
	EXPECT(f.ready);
	g_bus.fail_on_start = true;
	EXPECT(f.Receiver(index).SetCapture(true) == 0);
	EXPECT(g_bus.starts == 1);
	EXPECT(ReadFails(f.Buffer(index)));
	EXPECT(f.Receiver(index).SetCapture(false) == 0);
	EXPECT(g_bus.stops == 1);

	g_bus.fail_on_start = false;
	EXPECT(f.Receiver(index).SetCapture(true) == 0);
	EXPECT(g_bus.starts == 2);
	f.Feed(id, 8);
	EXPECT(Delivers(f.Buffer(index), 8));
	EXPECT(f.Receiver(index).SetCapture(false) == 0);
	EXPECT(g_bus.stops == 2);
	return true;
}

} // namespace

int main()
{
	bool ok = true;
	ok = MultiTunerCapture<Px4Fixture>("Px4MultiTunerCapture", 0, 2) && ok;
	ok = MultiTunerCapture<PxMltFixture>("PxMltMultiTunerCapture", 0, 3) && ok;
	ok = LateJoinerRejectedThenRecovers<Px4Fixture>("Px4LateJoiner", 0, 2) && ok;
	ok = LateJoinerRejectedThenRecovers<PxMltFixture>("PxMltLateJoiner", 1, 4) && ok;
	ok = Px4FailureDuringStartRequest() && ok;
	ok = FailureAfterJoinCheck<Px4Fixture>("Px4FailureAfterJoinCheck", 1, 3) && ok;
	ok = FailureAfterJoinCheck<PxMltFixture>("PxMltFailureAfterJoinCheck", 0, 2) && ok;
	ok = ConcurrentFailureAndStart<Px4Fixture>("Px4ConcurrentFailureAndStart", 0, 2, 300) && ok;
	ok = ConcurrentFailureAndStart<PxMltFixture>("PxMltConcurrentFailureAndStart", 2, 4, 300) && ok;
	ok = FailureRightAfterBusStart<Isdb2056Fixture>("Isdb2056FailureRightAfterBusStart", 0, 0) && ok;
	ok = FailureRightAfterBusStart<Isdbt2071Fixture>("Isdbt2071FailureRightAfterBusStart", 0, 0) && ok;
	ok = FailureRightAfterBusStart<Px4Fixture>("Px4FailureRightAfterBusStart", 2, 3) && ok;
	ok = FailureRightAfterBusStart<PxMltFixture>("PxMltFailureRightAfterBusStart", 3, 4) && ok;
	if (ok)
		std::puts("shared_stream_failure_test: passed");
	return ok ? 0 : 1;
}
