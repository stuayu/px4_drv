#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>
#include "t230c_ts_sync.h"

namespace px4::t230c {
using Bytes = std::vector<std::uint8_t>;
class TsSynchronizer final {
public:
    explicit TsSynchronizer(std::size_t limit);
    void Reset();
    void Push(const std::uint8_t *data, std::size_t size,
              const std::function<void(const std::uint8_t *)> &emit);
    std::size_t PendingSize() const noexcept { return sync_.pending_size; }
private:
    Bytes storage_;
    t230c_ts_sync sync_{};
};
/* Linux drivers/media/dvb-frontends/si2168.c and si2157.c command sequence. */
std::vector<Bytes> DemodTuneCommands(std::uint32_t symbol_rate_ksps, std::uint32_t bandwidth_mhz);
std::vector<Bytes> TunerTuneCommands(std::uint32_t frequency_khz, std::uint32_t bandwidth_mhz);
int ValidateFirmware(const Bytes &firmware) noexcept;
}
