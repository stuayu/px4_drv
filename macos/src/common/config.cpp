// config.cpp - POSIX INI file parser for macOS

#include "config.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwchar>
#include <cstdlib>

namespace px4 {

// Convert UTF-8 string to wstring (macOS wchar_t is UTF-32)
static std::wstring utf8_to_wstring(const std::string &s)
{
	std::wstring result;
	result.reserve(s.size());
	const char *p = s.c_str();
	while (*p) {
		wchar_t wc = static_cast<unsigned char>(*p);
		if (wc < 0x80) {
			result += static_cast<wchar_t>(wc);
			p++;
		} else if ((wc & 0xE0) == 0xC0 && p[1]) {
			result += static_cast<wchar_t>(((wc & 0x1F) << 6) | (p[1] & 0x3F));
			p += 2;
		} else if ((wc & 0xF0) == 0xE0 && p[1] && p[2]) {
			result += static_cast<wchar_t>(((wc & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
			p += 3;
		} else {
			result += L'?';
			p++;
		}
	}
	return result;
}

bool Config::Load(const std::wstring &path, const std::wstring &section) noexcept
{
	values_.clear();

	// Convert wstring path to string for fopen
	std::string spath;
	for (wchar_t wc : path)
		spath += static_cast<char>(wc & 0xFF);

	std::ifstream ifs(spath);
	if (!ifs.is_open())
		return false;

	std::string target_section;
	for (wchar_t wc : section)
		target_section += static_cast<char>(wc & 0xFF);

	bool in_section = false;
	bool found = false;
	std::string line;

	while (std::getline(ifs, line)) {
		// Remove carriage return if present
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		// Skip empty lines and comments
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;

		// Section header
		if (line[0] == '[') {
			size_t end = line.find(']');
			if (end == std::string::npos)
				continue;

			std::string sct = line.substr(1, end - 1);
			in_section = (sct == target_section);
			if (in_section)
				found = true;
			continue;
		}

		if (!in_section)
			continue;

		// Key=Value pair
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		std::string key = line.substr(0, eq);
		std::string value = line.substr(eq + 1);

		// Trim whitespace from key
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
			key.erase(key.begin());

		// Trim whitespace from value
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
			value.pop_back();
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
			value.erase(value.begin());

		// Strip surrounding quotes
		if (value.size() >= 2 &&
		    ((value.front() == '"' && value.back() == '"') ||
		     (value.front() == '\'' && value.back() == '\''))) {
			value = value.substr(1, value.size() - 2);
		}

		values_.emplace(utf8_to_wstring(key), utf8_to_wstring(value));
	}

	return found;
}

bool Config::Exists(const std::wstring &key) const noexcept
{
	return !!values_.count(key);
}

const std::wstring& Config::Get(const std::wstring &key) const
{
	return values_.at(key);
}

const std::wstring Config::Get(const std::wstring &key, const std::wstring &default_value) const
{
	try {
		return Get(key);
	} catch (const std::out_of_range &) {
		return default_value;
	}
}

bool ConfigSet::Load(const std::wstring &path) noexcept
{
	configs_.clear();

	// Convert wstring path to string
	std::string spath;
	for (wchar_t wc : path)
		spath += static_cast<char>(wc & 0xFF);

	std::ifstream ifs(spath);
	if (!ifs.is_open())
		return false;

	// Collect all section names first
	std::vector<std::wstring> sections;
	std::string line;

	while (std::getline(ifs, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.empty() || line[0] != '[')
			continue;

		size_t end = line.find(']');
		if (end == std::string::npos)
			continue;

		std::string sct = line.substr(1, end - 1);
		std::wstring wsct = utf8_to_wstring(sct);

		if (!configs_.count(wsct))
			sections.push_back(wsct);
	}

	// Load each section
	for (auto &s : sections) {
		auto v = configs_.emplace(s, Config());
		if (!v.first->second.Load(path, s)) {
			configs_.clear();
			return false;
		}
	}

	return !configs_.empty();
}

bool ConfigSet::Exists(const std::wstring &sct) const noexcept
{
	return !!configs_.count(sct);
}

const px4::Config& ConfigSet::Get(const std::wstring &sct) const
{
	return configs_.at(sct);
}

} // namespace px4
