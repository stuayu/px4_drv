// config.hpp

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace px4 {

class Config final {
public:
	Config() noexcept {}
	~Config() {}

	bool Load(const std::wstring &path, const std::wstring &section) noexcept;
	bool Exists(const std::wstring &key) const noexcept;
	const std::wstring& Get(const std::wstring &key) const;
	const std::wstring Get(const std::wstring &key, const std::wstring &default_value) const;
	std::size_t GetDynamicMemoryBytes() const noexcept
	{
		std::size_t bytes = values_.bucket_count() * sizeof(void*);
		for (const auto &entry : values_)
			bytes += sizeof(entry) + (entry.first.capacity() + entry.second.capacity()) * sizeof(wchar_t) + 2 * sizeof(void*);
		return bytes;
	}

private:
	std::unordered_map<std::wstring, std::wstring> values_;
};

class ConfigSet final {
public:
	ConfigSet() noexcept {}
	~ConfigSet() {}

	bool Load(const std::wstring &path) noexcept;
	bool Exists(const std::wstring &sct) const noexcept;
	const px4::Config& Get(const std::wstring &sct) const;
	std::size_t GetDynamicMemoryBytes() const noexcept
	{
		std::size_t bytes = configs_.bucket_count() * sizeof(void*);
		for (const auto &entry : configs_)
			bytes += sizeof(entry) + entry.first.capacity() * sizeof(wchar_t) + entry.second.GetDynamicMemoryBytes() + 2 * sizeof(void*);
		return bytes;
	}

private:
	std::unordered_map<std::wstring, Config> configs_;
};

} // namespace px4
