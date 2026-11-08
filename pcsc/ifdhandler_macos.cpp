// SPDX-License-Identifier: GPL-2.0-only
/* macOS の PC/SC IFD ハンドラー。カード操作は起動中の DriverHost_PX4 に委譲する。 */
extern "C" {
#include <wintypes.h>
/* Apple の wintypes.h が RESPONSECODE を定義するため、上流 IFD ヘッダー側の重複定義を止める。 */
#define RESPONSECODE_DEFINED_IN_WINTYPES_H
#include <ifdhandler.h>
}

/* PC/SC のマクロを避け、IPC 契約の名前付き定数を使う。 */
#undef MAX_ATR_SIZE

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <syslog.h>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "card_command.hpp"

namespace {

constexpr unsigned char MAX_READERS = 16;
constexpr const wchar_t *TORNE_READER_PREFIX = L"Sony CECH-ZD1 J Smart Card Reader (ID: ";

struct Reader {
	std::mutex lock;
	int fd = -1;
	std::wstring name;
	std::vector<std::uint8_t> atr;
	bool connected = true;
	bool card_connected = false;
	bool powered = false;
	DWORD protocol = SCARD_PROTOCOL_UNDEFINED;

	~Reader()
	{
		if (fd >= 0)
			close(fd);
	}
};

std::mutex readers_lock;
std::map<DWORD, std::shared_ptr<Reader>> readers;

std::string CardSocketPath()
{
	const char *runtime_dir = std::getenv("PX4_RUNTIME_DIR");
	std::string path = runtime_dir && runtime_dir[0] ? runtime_dir : "/tmp";
	if (path.back() != '/')
		path.push_back('/');
	path += "px4_card_pipe.sock";
	return path;
}

bool SendAll(int fd, const void *buffer, std::size_t length)
{
	const auto *data = static_cast<const std::uint8_t *>(buffer);
	while (length) {
		const ssize_t written = send(fd, data, length, 0);
		if (written <= 0)
			return false;
		data += written;
		length -= static_cast<std::size_t>(written);
	}
	return true;
}

bool ReceiveAll(int fd, void *buffer, std::size_t length)
{
	auto *data = static_cast<std::uint8_t *>(buffer);
	while (length) {
		const ssize_t received = recv(fd, data, length, 0);
		if (received <= 0)
			return false;
		data += received;
		length -= static_cast<std::size_t>(received);
	}
	return true;
}

int ConnectCardServer()
{
	const std::string path = CardSocketPath();
	if (path.size() >= sizeof(((sockaddr_un *)nullptr)->sun_path))
		return -1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	do {
		const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		int no_sigpipe = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) < 0) {
			close(fd);
			return -1;
		}
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
		if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
			return fd;
		const int error = errno;
		close(fd);
		if (error != ENOENT && error != ECONNREFUSED)
			return -1;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	} while (std::chrono::steady_clock::now() < deadline);
	return -1;
}

bool Exchange(Reader &reader, px4::card_command::Command &command)
{
	if (reader.fd < 0 || !SendAll(reader.fd, &command, sizeof(command)) ||
	    !ReceiveAll(reader.fd, &command, sizeof(command))) {
		if (reader.fd >= 0)
			close(reader.fd);
		reader.fd = -1;
		reader.card_connected = false;
		reader.powered = false;
		reader.protocol = SCARD_PROTOCOL_UNDEFINED;
		reader.atr.clear();
		return false;
	}
	return command.status == px4::card_command::Status::SUCCEEDED;
}

RESPONSECODE MapError(const px4::card_command::Command &command)
{
	if (command.status == px4::card_command::Status::SUCCEEDED)
		return IFD_SUCCESS;
	switch (command.error) {
	case px4::card_command::Error::NO_READER:
	case px4::card_command::Error::REMOVED:
		return IFD_NO_SUCH_DEVICE;
	case px4::card_command::Error::NO_CARD:
		return IFD_ICC_NOT_PRESENT;
	case px4::card_command::Error::TIMEOUT:
		return IFD_RESPONSE_TIMEOUT;
	case px4::card_command::Error::INSUFFICIENT_BUFFER:
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	case px4::card_command::Error::PROTOCOL:
		return IFD_COMMUNICATION_ERROR;
	case px4::card_command::Error::BUSY:
		return IFD_COMMUNICATION_ERROR;
	case px4::card_command::Error::INVALID_PARAMETER:
		return IFD_COMMUNICATION_ERROR;
	default:
		return IFD_COMMUNICATION_ERROR;
	}
}

std::wstring ReaderId(const std::wstring &name)
{
	if (name.compare(0, std::wcslen(TORNE_READER_PREFIX), TORNE_READER_PREFIX) != 0 ||
	    name.size() < std::wcslen(TORNE_READER_PREFIX) + 16)
		return {};
	return name.substr(std::wcslen(TORNE_READER_PREFIX), 16);
}

bool ParseDeviceBusAddress(const char *device_name, std::wstring &path)
{
	/* IFDHCreateChannelByName の USB 名は OS ごとに異なる。末尾の BUS:ADDRESS が
	 * DriverHost_PX4 が使う BUS:ADDRESS と一致する場合だけ複数個体の対応に使う。 */
	if (!device_name)
		return false;
	const std::string value(device_name);
	for (std::size_t end = value.size(); end > 0; --end) {
		if (value[end - 1] < '0' || value[end - 1] > '9')
			continue;
		std::size_t address_begin = end - 1;
		while (address_begin && value[address_begin - 1] >= '0' && value[address_begin - 1] <= '9')
			--address_begin;
		if (!address_begin || (value[address_begin - 1] != ':' && value[address_begin - 1] != '/'))
			continue;
		std::size_t bus_end = address_begin - 1;
		std::size_t bus_begin = bus_end;
		while (bus_begin && value[bus_begin - 1] >= '0' && value[bus_begin - 1] <= '9')
			--bus_begin;
		if (bus_begin == bus_end || !bus_begin ||
		    (value[bus_begin - 1] != ':' && value[bus_begin - 1] != '/'))
			continue;
		const std::string bus_text = value.substr(bus_begin, bus_end - bus_begin);
		const std::string address_text = value.substr(address_begin, end - address_begin);
		char *parse_end = nullptr;
		errno = 0;
		const unsigned long bus = std::strtoul(bus_text.c_str(), &parse_end, 10);
		if (errno || !parse_end || *parse_end)
			continue;
		errno = 0;
		const unsigned long address = std::strtoul(address_text.c_str(), &parse_end, 10);
		if (errno || !parse_end || *parse_end)
			continue;
		path = std::to_wstring(bus) + L":" + std::to_wstring(address);
		return true;
	}
	return false;
}

std::wstring HashDevicePath(const std::wstring &path)
{
	std::uint64_t hash = 14695981039346656037ULL;
	for (wchar_t character : path) {
		hash ^= static_cast<std::uint16_t>(character);
		hash *= 1099511628211ULL;
	}
	wchar_t id[17] = {};
	swprintf(id, 17, L"%016llX", static_cast<unsigned long long>(hash));
	return id;
}

bool SelectReader(Reader &reader, const char *device_name)
{
	px4::card_command::Command command{};
	command.code = px4::card_command::Code::LIST_READERS;
	if (!Exchange(reader, command) || command.reader_count > px4::card_command::MAX_READERS)
		return false;

	std::vector<std::wstring> candidates;
	for (std::uint32_t index = 0; index < command.reader_count; ++index) {
		if (!std::wmemchr(command.readers[index], L'\0', px4::card_command::MAX_READER_NAME))
			return false;
		const std::wstring name(command.readers[index]);
		if (!ReaderId(name).empty())
			candidates.push_back(name);
	}
	if (candidates.empty())
		return false;
	if (candidates.size() == 1) {
		reader.name = candidates.front();
		return true;
	}

	std::wstring device_path;
	if (ParseDeviceBusAddress(device_name, device_path)) {
		const std::wstring wanted_id = HashDevicePath(device_path);
		for (const auto &candidate : candidates)
			if (ReaderId(candidate) == wanted_id) {
				reader.name = candidate;
				return true;
		}
	}
	syslog(LOG_ERR, "Could not map macOS USB device '%s' to one of %zu torne readers",
		device_name ? device_name : "", candidates.size());
	return false;
}

std::shared_ptr<Reader> FindReader(DWORD lun)
{
	std::lock_guard<std::mutex> lock(readers_lock);
	auto found = readers.find(lun);
	return found == readers.end() ? nullptr : found->second;
}

template<class Function>
RESPONSECODE WithReader(DWORD lun, Function function)
{
	try {
		auto reader = FindReader(lun);
		if (!reader)
			return IFD_NO_SUCH_DEVICE;
		std::lock_guard<std::mutex> lock(reader->lock);
		if (!reader->connected)
			return IFD_NO_SUCH_DEVICE;
		try {
			return function(*reader);
		} catch (...) {
			return IFD_COMMUNICATION_ERROR;
		}
	} catch (...) {
		return IFD_COMMUNICATION_ERROR;
	}
}

px4::card_command::Command MakeCommand(px4::card_command::Code code,
					      const Reader &reader)
{
	px4::card_command::Command command{};
	command.code = code;
	const std::size_t length = std::min(reader.name.size(),
		px4::card_command::MAX_READER_NAME - 1);
	std::wmemcpy(command.reader_name, reader.name.data(), length);
	return command;
}

void SaveAtr(Reader &reader, const px4::card_command::Command &command)
{
	if (command.atr_length <= px4::card_command::MAX_ATR_SIZE)
		reader.atr.assign(command.atr, command.atr + command.atr_length);
	else
		reader.atr.clear();
}

} // namespace

extern "C" RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	if ((Lun & 0xffff) != 0)
		return IFD_NO_SUCH_DEVICE;
	try {
		auto reader = std::make_shared<Reader>();
		reader->fd = ConnectCardServer();
		if (reader->fd < 0 || !SelectReader(*reader, DeviceName))
			return IFD_NO_SUCH_DEVICE;
		std::lock_guard<std::mutex> lock(readers_lock);
		if (readers.size() >= MAX_READERS || readers.count(Lun))
			return IFD_COMMUNICATION_ERROR;
		readers.emplace(Lun, std::move(reader));
		return IFD_SUCCESS;
	} catch (...) {
		return IFD_COMMUNICATION_ERROR;
	}
}

extern "C" RESPONSECODE IFDHCreateChannel(DWORD, DWORD)
{
	return IFD_NOT_SUPPORTED;
}

extern "C" RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	std::shared_ptr<Reader> reader;
	{
		std::lock_guard<std::mutex> lock(readers_lock);
		auto found = readers.find(Lun);
		if (found == readers.end())
			return IFD_NO_SUCH_DEVICE;
		reader = found->second;
		readers.erase(found);
	}
	std::lock_guard<std::mutex> lock(reader->lock);
	if (reader->card_connected) {
		auto command = MakeCommand(px4::card_command::Code::DISCONNECT, *reader);
		(void)Exchange(*reader, command);
	}
	reader->connected = false;
	return IFD_SUCCESS;
}

extern "C" RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag,
	PDWORD Length, PUCHAR Value)
{
	if (!Length)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *Length;
	*Length = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		unsigned char value = 1;
		const void *data = &value;
		DWORD size = 1;
		if (Tag == TAG_IFD_ATR) {
			data = reader.atr.data();
			size = static_cast<DWORD>(reader.atr.size());
		} else if (Tag == TAG_IFD_SIMULTANEOUS_ACCESS) {
			value = MAX_READERS;
		} else if (Tag != TAG_IFD_THREAD_SAFE && Tag != TAG_IFD_SLOT_THREAD_SAFE &&
			   Tag != TAG_IFD_SLOTS_NUMBER) {
			return IFD_ERROR_TAG;
		}
		*Length = size;
		if (capacity < size)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		if (size && !Value)
			return IFD_COMMUNICATION_ERROR;
		if (size)
			std::memcpy(Value, data, size);
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHSetCapabilities(DWORD, DWORD, DWORD, PUCHAR)
{
	return IFD_ERROR_TAG;
}

extern "C" RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	return WithReader(Lun, [](Reader &reader) -> RESPONSECODE {
		auto command = MakeCommand(px4::card_command::Code::STATUS, reader);
		if (!Exchange(reader, command)) {
			reader.powered = false;
			reader.protocol = SCARD_PROTOCOL_UNDEFINED;
			reader.atr.clear();
			return MapError(command);
		}
		SaveAtr(reader, command);
		if (!command.card_present) {
			reader.powered = false;
			reader.protocol = SCARD_PROTOCOL_UNDEFINED;
		}
		return command.card_present ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
	});
}

extern "C" RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action,
	PUCHAR Atr, PDWORD AtrLength)
{
	if (!AtrLength)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *AtrLength;
	*AtrLength = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		if (Action == IFD_POWER_DOWN) {
			/* DriverHost の CONNECT を保ち、PC/SC の停止とカード参照の寿命を分ける。 */
			reader.powered = false;
			reader.protocol = SCARD_PROTOCOL_UNDEFINED;
			reader.atr.clear();
			return IFD_SUCCESS;
		}
		if (Action != IFD_POWER_UP && Action != IFD_RESET)
			return IFD_NOT_SUPPORTED;
		if (!Atr || capacity < px4::card_command::MAX_ATR_SIZE)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		const bool newly_connected = !reader.card_connected;
		if (newly_connected) {
			auto connect = MakeCommand(px4::card_command::Code::CONNECT, reader);
			connect.share_mode = static_cast<std::uint32_t>(px4::card_command::ShareMode::SHARED);
			if (!Exchange(reader, connect))
				return MapError(connect);
			reader.card_connected = true;
		}
		auto command = MakeCommand(newly_connected ?
			px4::card_command::Code::STATUS : px4::card_command::Code::RESET, reader);
		if (!Exchange(reader, command)) {
			reader.powered = false;
			reader.protocol = SCARD_PROTOCOL_UNDEFINED;
			reader.atr.clear();
			return MapError(command);
		}
		if (newly_connected && (!command.card_present || !command.card_initialized)) {
			reader.powered = false;
			reader.protocol = SCARD_PROTOCOL_UNDEFINED;
			reader.atr.clear();
			return IFD_ICC_NOT_PRESENT;
		}
		if (command.atr_length > px4::card_command::MAX_ATR_SIZE)
			return IFD_COMMUNICATION_ERROR;
		SaveAtr(reader, command);
		reader.powered = true;
		reader.protocol = SCARD_PROTOCOL_UNDEFINED;
		std::memcpy(Atr, reader.atr.data(), reader.atr.size());
		*AtrLength = static_cast<DWORD>(reader.atr.size());
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol,
	UCHAR Flags, UCHAR, UCHAR, UCHAR)
{
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		if (Protocol != SCARD_PROTOCOL_T1)
			return IFD_PROTOCOL_NOT_SUPPORTED;
		if (Flags)
			return IFD_ERROR_PTS_FAILURE;
		if (!reader.powered || !reader.card_connected)
			return IFD_COMMUNICATION_ERROR;
		reader.protocol = Protocol;
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
	PUCHAR TxBuffer, DWORD TxLength, PUCHAR RxBuffer, PDWORD RxLength,
	PSCARD_IO_HEADER RecvPci)
{
	if (!RxLength)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *RxLength;
	*RxLength = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		if (SendPci.Protocol != 1 || reader.protocol != SCARD_PROTOCOL_T1)
			return IFD_PROTOCOL_NOT_SUPPORTED;
		if (!reader.powered || !reader.card_connected || !TxBuffer || !TxLength ||
		    !RxBuffer || !capacity || TxLength > px4::card_command::MAX_DATA_SIZE ||
		    capacity > px4::card_command::MAX_DATA_SIZE)
			return IFD_COMMUNICATION_ERROR;
		auto command = MakeCommand(px4::card_command::Code::TRANSMIT, reader);
		command.data_length = TxLength;
		command.data_capacity = capacity;
		std::memcpy(command.data, TxBuffer, TxLength);
		if (!Exchange(reader, command))
			return MapError(command);
		if (command.data_length > capacity || command.data_length > px4::card_command::MAX_DATA_SIZE)
			return IFD_COMMUNICATION_ERROR;
		std::memcpy(RxBuffer, command.data, command.data_length);
		*RxLength = command.data_length;
		if (RecvPci) {
			RecvPci->Protocol = SCARD_PROTOCOL_T1;
			RecvPci->Length = sizeof(*RecvPci);
		}
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHControl(DWORD, DWORD, PUCHAR, DWORD, PUCHAR, DWORD,
	LPDWORD BytesReturned)
{
	if (BytesReturned)
		*BytesReturned = 0;
	return IFD_NOT_SUPPORTED;
}
