// Runtime paths shared by the macOS DriverHost socket servers and lock file.
#pragma once

#include <cstdlib>
#include <string>

namespace px4 {

inline std::string runtime_path(const char *name)
{
	const char *configured_dir = std::getenv("PX4_RUNTIME_DIR");
	std::string path = configured_dir && configured_dir[0]
		? configured_dir
		: "/tmp";

	if (path.back() != '/')
		path += '/';
	path += name;
	return path;
}

} // namespace px4
