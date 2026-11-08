/*
 * Derived from Linux drivers/media/dvb-frontends/si2168.c and si2157.c.
 * Copyright (C) 2014 Antti Palosaari <crope@iki.fi>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t230c_protocol.hpp"
#include "si2168.h"
#include "si2141.h"
#include <algorithm>

namespace px4::t230c {
TsSynchronizer::TsSynchronizer(std::size_t limit)
    : storage_(std::max<std::size_t>(limit, T230C_TS_PACKET_SIZE * T230C_TS_SYNC_COUNT))
{
    t230c_ts_sync_init(&sync_, storage_.data(), storage_.size());
}
void TsSynchronizer::Reset()
{
    t230c_ts_sync_reset(&sync_);
}
void TsSynchronizer::Push(const std::uint8_t *data, std::size_t size,
                          const std::function<void(const std::uint8_t *)> &emit)
{
    auto callback = [](void *context, const std::uint8_t *packet) {
        (*static_cast<const std::function<void(const std::uint8_t *)> *>(context))(packet);
    };
    (void)t230c_ts_sync_push(&sync_, data, size, callback,
                             const_cast<std::function<void(const std::uint8_t *)> *>(&emit));
}
std::vector<Bytes> DemodTuneCommands(std::uint32_t symbol_rate_ksps, std::uint32_t bandwidth_mhz, bool j83b)
{
    si2168_command commands[SI2168_MAX_COMMANDS] = {};
    std::size_t count = 0;
    const auto annex = j83b ? SI2168_CABLE_ANNEX_B : SI2168_CABLE_ANNEX_A;
    if (si2168_build_cable_tune_commands(annex, symbol_rate_ksps, bandwidth_mhz,
                                         SI2168_T230C_TS_MODE, true,
                                         commands, SI2168_MAX_COMMANDS, &count))
        return {};
    std::vector<Bytes> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        result.emplace_back(commands[i].data, commands[i].data + commands[i].length);
    return result;
}
std::vector<Bytes> TunerTuneCommands(std::uint32_t frequency_khz, std::uint32_t bandwidth_mhz)
{
    si2141_command commands[SI2141_MAX_COMMANDS] = {};
    std::size_t count = 0;
    if (si2141_build_tune_commands(frequency_khz, bandwidth_mhz,
                                   commands, SI2141_MAX_COMMANDS, &count))
        return {};
    std::vector<Bytes> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        result.emplace_back(commands[i].data, commands[i].data + commands[i].length);
    return result;
}
int ValidateFirmware(const Bytes &firmware) noexcept
{
    return si2168_validate_firmware(firmware.data(), firmware.size()) ? -1 : 0;
}
}
