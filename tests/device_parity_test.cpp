// device_parity_test.cpp - OS 間で利用できる機種に差が無いことを確かめる。
//
// - macOS 版と WinUSB 版の DriverHost_PX4.ini が、同じ機種 (Type、VID、PID、
//   受信機の System) を定義していること
// - すべての Type をホスト層の device factory が扱えること
// - Linux カーネルモジュールの USB ID 表が同じ VID/PID を持つこと
//
// INI は ASCII の key だけを読むため、WinUSB 版の CP932 と macOS 版の UTF-8 の
// どちらもそのまま扱える。

#include <cctype>
#include <cstdio>
#include <tuple>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "device_factory.hpp"

namespace {

/*
 * WinUSB 版の INI は VID/PID を持たず、INF が割り当てる DeviceInterfaceGUID で機種を
 * 識別する。比較は Type、DeviceInterfaceGUID、受信機の System で行い、VID/PID は
 * macOS 版の値を Linux 版との照合に使う。
 */
struct Device {
	std::string type;
	std::string interface_guid;
	unsigned vid = 0, pid = 0;
	unsigned xfer_packets = 0;
	std::set<std::string> systems;
	bool operator<(const Device &o) const
	{
		return std::tie(type, interface_guid, systems) <
			std::tie(o.type, o.interface_guid, o.systems);
	}
};

std::string Upper(std::string s)
{
	for (auto &c : s)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

std::string Trim(std::string s)
{
	while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	size_t b = s.find_first_not_of(" \t");
	s = b == std::string::npos ? "" : s.substr(b);
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		s = s.substr(1, s.size() - 2);
	return s;
}

std::vector<Device> LoadIni(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::map<std::string, Device> by_def;
	std::string line, section;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == ';')
			continue;
		if (line[0] == '[') {
			section = line.substr(1, line.find(']') - 1);
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos || section.rfind("DeviceDefinition", 0) != 0)
			continue;
		const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
		const std::string def = section.substr(0, section.find('.'));
		Device &d = by_def[def];
		if (section == def && key == "Type")
			d.type = value;
		else if (section == def && key == "DeviceInterfaceGUID")
			d.interface_guid = Upper(value);
		else if (section == def + ".Config" && key == "VendorId")
			d.vid = std::stoul(value, nullptr, 0);
		else if (section == def + ".Config" && key == "ProductId")
			d.pid = std::stoul(value, nullptr, 0);
		else if (section == def + ".Config" && key == "XferPackets")
			d.xfer_packets = std::stoul(value, nullptr, 0);
		else if (section.find(".Receiver") != std::string::npos && key == "System") {
			std::stringstream ss(value);
			std::string sys;
			while (std::getline(ss, sys, ','))
				d.systems.insert(Trim(sys));
		}
	}
	std::vector<Device> out;
	for (auto &kv : by_def)
		if (!kv.second.type.empty())
			out.push_back(kv.second);
	return out;
}

std::wstring Widen(const std::string &s)
{
	return std::wstring(s.begin(), s.end());
}

/* driver/linux/px4_usb.c の USB_DEVICE(vid, pid) と、PID 定数の #define を読む。 */
std::set<std::pair<unsigned, unsigned>> LoadLinuxIds(const std::string &root)
{
	std::map<std::string, unsigned> defs;
	std::set<std::pair<unsigned, unsigned>> ids;
	for (const char *f : { "/driver/linux/px4_usb.h", "/driver/linux/px4_usb.c" }) {
		std::ifstream in(root + f);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::regex def_re(R"(#define\s+(USB_(?:PID|VID)_\w+)\s+(0x[0-9a-fA-F]+))");
		for (std::sregex_iterator it(text.begin(), text.end(), def_re), end; it != end; ++it)
			defs[(*it)[1]] = std::stoul((*it)[2], nullptr, 16);
		std::regex id_re(R"(USB_DEVICE\(\s*(\w+)\s*,\s*(\w+)\s*\))");
		auto value = [&](const std::string &v) -> unsigned {
			return v.rfind("0x", 0) == 0 ? std::stoul(v, nullptr, 16) : defs[v];
		};
		for (std::sregex_iterator it(text.begin(), text.end(), id_re), end; it != end; ++it)
			ids.insert({ value((*it)[1]), value((*it)[2]) });
	}
	return ids;
}

} // namespace

int main()
{
	const std::string root = PX4_REPO_ROOT;
	const auto mac = LoadIni(root + "/macos/DriverHost_PX4.ini");
	const auto win = LoadIni(root + "/winusb/pkg/DriverHost_PX4/DriverHost_PX4.ini");
	bool ok = true;

	const std::set<Device> mac_set(mac.begin(), mac.end()), win_set(win.begin(), win.end());
	for (const auto &d : mac_set)
		if (!win_set.count(d)) {
			std::fprintf(stderr, "WinUSB INI lacks %s %s\n", d.type.c_str(), d.interface_guid.c_str());
			ok = false;
		}
	for (const auto &d : win_set)
		if (!mac_set.count(d)) {
			std::fprintf(stderr, "macOS INI lacks %s %s\n", d.type.c_str(), d.interface_guid.c_str());
			ok = false;
		}
	for (const auto &d : mac_set)
		if (px4::ParseDeviceType(Widen(d.type)) == px4::DeviceType::UNKNOWN) {
			std::fprintf(stderr, "device factory does not know Type=%s\n", d.type.c_str());
			ok = false;
		}
	for (const auto &[os_name, devices] : {
		std::pair<const char *, const std::vector<Device> *>{"macOS", &mac},
		std::pair<const char *, const std::vector<Device> *>{"WinUSB", &win}}) {
		bool found_t230c = false;
		for (const auto &d : *devices) {
			if (d.type != "T230C")
				continue;
			found_t230c = true;
			if (d.xfer_packets != 22) {
				std::fprintf(stderr, "%s T230C XferPackets=%u, expected 22\n",
					os_name, d.xfer_packets);
				ok = false;
			}
		}
		if (!found_t230c) {
			std::fprintf(stderr, "%s INI lacks T230C transfer configuration\n", os_name);
			ok = false;
		}
	}

	/*
	 * Linux 版の未対応機種。ここに無い差は失敗にする。
	 */
	const std::set<std::pair<unsigned, unsigned>> linux_pending = {
	};
	const auto linux_ids = LoadLinuxIds(root);
	if (linux_ids.empty()) {
		std::fprintf(stderr, "Linux USB ID table could not be read\n");
		ok = false;
	}
	for (const auto &d : mac_set) {
		const std::pair<unsigned, unsigned> id(d.vid, d.pid);
		const bool in_linux = linux_ids.count(id) != 0;
		if (!in_linux && !linux_pending.count(id)) {
			std::fprintf(stderr, "Linux lacks %s %04x:%04x\n", d.type.c_str(), d.vid, d.pid);
			ok = false;
		} else if (!in_linux) {
			std::printf("pending on Linux: %s %04x:%04x\n", d.type.c_str(), d.vid, d.pid);
		} else if (linux_pending.count(id)) {
			std::fprintf(stderr, "remove %04x:%04x from linux_pending\n", d.vid, d.pid);
			ok = false;
		}
	}
	std::printf("devices: %zu, Linux USB IDs: %zu\n", mac_set.size(), linux_ids.size());
	return ok ? 0 : 1;
}
