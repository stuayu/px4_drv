/* SPDX-License-Identifier: GPL-2.0-only */

#include "t230c_silicon.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <thread>

namespace px4::t230c {
namespace {
struct FirmwareLoadContext {
	const TunerFirmwareLoader *loader;
	Bytes firmware;
	std::string *error;
};

Deadline DeadlineFromMilliseconds(t230c_u64 value)
{
	if (value == ~(t230c_u64)0)
		return (Deadline::max)();
	return Deadline(std::chrono::duration_cast<Deadline::duration>(
		std::chrono::milliseconds(value)));
}
}

Silicon::Silicon(I2cTransfer transfer, Delay delay)
	: transfer_(std::move(transfer)), delay_(std::move(delay))
{
	i2c_master_.gate_ctrl = GateCtrl;
	i2c_master_.request = I2cRequest;
	i2c_master_.priv = this;
	demod_.i2c = &i2c_master_;
	demod_.clock_context = this;
	demod_.now_ms = NowMs;
	demod_.sleep_us = SleepUs;
	demod_.set_deadline = SetDeadline;
	demod_.ts_mode = SI2168_T230C_TS_MODE;
	demod_.ts_clock_inv = true;
	tuner_.i2c = &i2c_master_;
	tuner_.clock_context = this;
	tuner_.now_ms = NowMs;
	tuner_.sleep_ms = SleepMs;
	tuner_.set_deadline = SetDeadline;
}

int Silicon::I2cRequest(void *context, const i2c_comm_request *requests,
			int count)
{
	auto &self = *static_cast<Silicon *>(context);
	if (!requests || (count != 1 && count != 2))
		return -EINVAL;
	const auto &first = requests[0];
	if (first.len <= 0 || !first.data)
		return -EINVAL;
	Bytes write;
	Bytes read;
	if (count == 1) {
		if (first.req == i2c_comm_request::I2C_WRITE_REQUEST)
			write.assign(first.data, first.data + first.len);
		else if (first.req == i2c_comm_request::I2C_READ_REQUEST)
			read.resize(first.len);
		else
			return -EINVAL;
	} else {
		const auto &second = requests[1];
		if (first.req != i2c_comm_request::I2C_WRITE_REQUEST ||
		    second.req != i2c_comm_request::I2C_READ_REQUEST ||
		    first.addr != second.addr || second.len <= 0 || !second.data)
			return -EINVAL;
		write.assign(first.data, first.data + first.len);
		read.resize(second.len);
	}
	int ret;
	try {
		ret = self.transfer_(static_cast<std::uint8_t>(first.addr),
				     write, read, self.active_deadline_);
	} catch (...) {
		return -EIO;
	}
	if (ret)
		return ret;
	if (count == 1 && first.req == i2c_comm_request::I2C_READ_REQUEST) {
		if (read.size() != static_cast<std::size_t>(first.len))
			return -EIO;
		std::copy(read.begin(), read.end(), first.data);
	} else if (count == 2) {
		const auto &second = requests[1];
		if (read.size() != static_cast<std::size_t>(second.len))
			return -EIO;
		std::copy(read.begin(), read.end(), second.data);
	}
	return 0;
}

int Silicon::GateCtrl(void *, bool)
{
	/* ホスト側の I2C 転送ラッパーが Si2141 ゲートを開閉する。 */
	return 0;
}

t230c_u64 Silicon::NowMs(void *)
{
	const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return value < 0 ? 0 : static_cast<t230c_u64>(value);
}

void Silicon::SleepUs(void *context, t230c_u32 usec)
{
	auto &self = *static_cast<Silicon *>(context);
	if (self.delay_)
		self.delay_(static_cast<unsigned int>((usec + 999) / 1000));
}

void Silicon::SleepMs(void *context, t230c_u32 msec)
{
	auto &self = *static_cast<Silicon *>(context);
	if (self.delay_)
		self.delay_(msec);
}

void Silicon::SetDeadline(void *context, t230c_u64 deadline_ms)
{
	static_cast<Silicon *>(context)->active_deadline_ =
		DeadlineFromMilliseconds(deadline_ms);
}

int Silicon::LoadTunerFirmware(void *context, t230c_u8 rom_id,
			       const t230c_u8 **data, std::size_t *size)
{
	auto &load = *static_cast<FirmwareLoadContext *>(context);
	if (!load.loader || !data || !size)
		return -EINVAL;
	int ret = (*load.loader)(rom_id, load.firmware, *load.error);
	if (ret)
		return ret;
	ret = ValidateFirmware(load.firmware);
	if (ret) {
		*load.error = "firmware must contain valid 17-byte records";
		return -EINVAL;
	}
	*data = load.firmware.data();
	*size = load.firmware.size();
	return 0;
}

int Silicon::Initialize(const Bytes &demod_firmware,
		const TunerFirmwareLoader &load_tuner_firmware,
		std::string &error)
{
	error.clear();
	active_deadline_ = (Deadline::max)();
	int ret = si2168_init(&demod_, &i2c_master_, this, NowMs, SleepUs,
			      SetDeadline, demod_firmware.data(),
			      demod_firmware.size(), SI2168_T230C_TS_MODE, true);
	if (ret)
		return ret;
	FirmwareLoadContext loader = { &load_tuner_firmware, {}, &error };
	t230c_u8 rom_id = 0;
	t230c_u8 part_id = 0;
	ret = si2141_init(&tuner_, &i2c_master_, this, NowMs, SleepMs,
			  SetDeadline, LoadTunerFirmware, &loader, 0,
			  &rom_id, &part_id);
	if (ret == -ENODEV && part_id != 41)
		error = "unexpected Si2141 part ID " + std::to_string(part_id);
	return ret;
}

int Silicon::Tune(std::uint32_t frequency_khz,
		  std::uint32_t symbol_rate_ksps,
		  std::uint32_t bandwidth_mhz, Deadline deadline)
{
	const t230c_u64 deadline_ms = deadline == (Deadline::max)()
		? ~(t230c_u64)0
		: static_cast<t230c_u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline.time_since_epoch()).count());
	int ret = si2141_tune(&tuner_, frequency_khz, bandwidth_mhz, deadline_ms);
	if (ret)
		return ret;
	(void)si2141_wait_tune(&tuner_, deadline_ms);
	return si2168_tune(&demod_, symbol_rate_ksps, bandwidth_mhz, deadline_ms);
}

int Silicon::ReadLock(bool &locked, std::int32_t &cnr, Deadline deadline)
{
	const t230c_u64 deadline_ms = deadline == (Deadline::max)()
		? ~(t230c_u64)0
		: static_cast<t230c_u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline.time_since_epoch()).count());
	return si2168_read_lock(&demod_, &locked, &cnr, deadline_ms);
}

int LoadFirmwareFile(const std::wstring &path, Bytes &firmware,
		     std::string &error)
{
	std::error_code ec;
	const auto file_path = std::filesystem::path(path);
	const auto status = std::filesystem::status(file_path, ec);
	if (ec == std::errc::no_such_file_or_directory ||
	    (!ec && !std::filesystem::exists(status))) {
		error = "firmware file not found";
		return -ENOENT;
	}
	if (ec || !std::filesystem::is_regular_file(status)) {
		error = "firmware file cannot be accessed";
		return -EIO;
	}
	std::ifstream file(file_path, std::ios::binary);
	if (!file) {
		error = "firmware file cannot be opened";
		return -EIO;
	}
	firmware.assign(std::istreambuf_iterator<char>(file), {});
	if (file.bad()) {
		error = "firmware file cannot be read";
		return -EIO;
	}
	if (ValidateFirmware(firmware)) {
		error = "firmware must contain valid 17-byte records";
		return -EINVAL;
	}
	error.clear();
	return 0;
}

/* ROM 固有ファイルが無い場合だけ A10 ファイルへ切り替える。 */
int LoadTunerFirmware(std::uint8_t rom_id, const TunerFirmwarePaths &paths,
		      Bytes &firmware, std::string &error)
{
	const std::wstring *primary = rom_id == 0x60 ? &paths.rom60 :
		rom_id == 0x61 ? &paths.rom61 : nullptr;
	int ret = primary ? LoadFirmwareFile(*primary, firmware, error) : -ENOENT;
	if (ret == -ENOENT)
		ret = LoadFirmwareFile(paths.fallback, firmware, error);
	if (!ret) {
		error.clear();
	} else {
		std::ostringstream message;
		message << "Si2141 ROM 0x" << std::hex << std::setw(2)
			<< std::setfill('0') << static_cast<unsigned int>(rom_id)
			<< ": " << error;
		error = message.str();
	}
	return ret;
}
}
