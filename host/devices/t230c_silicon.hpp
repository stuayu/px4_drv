#pragma once
#include "t230c_protocol.hpp"
#include <functional>
#include <chrono>
#include <string>
#include "si2168.h"
#include "si2141.h"
namespace px4::t230c {
using Deadline = std::chrono::steady_clock::time_point;
using I2cTransfer = std::function<int(std::uint8_t, const Bytes &, Bytes &, Deadline)>;
using Delay = std::function<void(unsigned int)>;
using TunerFirmwareLoader = std::function<int(std::uint8_t, Bytes &, std::string &)>;
struct TunerFirmwarePaths {
    std::wstring rom60;
    std::wstring rom61;
    std::wstring fallback;
};
class Silicon final {
public:
    Silicon(I2cTransfer transfer, Delay delay);
    int Initialize(const Bytes &demod_firmware, const TunerFirmwareLoader &load_tuner_firmware, std::string &error);
    /* j83b が true のとき、復調器を J.83 Annex B で選局する (ロック確認も Annex B の 0x98 を使う)。 */
    int Tune(std::uint32_t frequency_khz, std::uint32_t symbol_rate_ksps, std::uint32_t bandwidth_mhz, Deadline deadline, bool j83b = false);
    int ReadLock(bool &locked, std::int32_t &cnr_millibel, Deadline deadline);
private:
	static int I2cRequest(void *context, const i2c_comm_request *requests, int count);
	static int GateCtrl(void *context, bool open);
	static t230c_u64 NowMs(void *context);
	static void SleepUs(void *context, t230c_u32 usec);
	static void SleepMs(void *context, t230c_u32 msec);
	static void SetDeadline(void *context, t230c_u64 deadline_ms);
	static int LoadTunerFirmware(void *context, t230c_u8 rom_id,
				     const t230c_u8 **data, std::size_t *size);
	Deadline active_deadline_ = (Deadline::max)();
	struct i2c_comm_master i2c_master_{};
	struct si2168 demod_{};
	struct si2141 tuner_{};
    I2cTransfer transfer_;
    Delay delay_;
};
int LoadFirmwareFile(const std::wstring &path, Bytes &firmware, std::string &error);
int LoadTunerFirmware(std::uint8_t rom_id, const TunerFirmwarePaths &paths, Bytes &firmware, std::string &error);
}
