#include <fstream>
#include <string>
#include <unistd.h>

/* MSVC の wstring ファイル名を macOS の一時ファイルに接続し、解析本体は共有する。 */
namespace std {
class test_ifstream : public ifstream {
public:
	explicit test_ifstream(const wstring &path) : ifstream(string(path.begin(), path.end())) {}
};
}
#define ifstream test_ifstream
#include "../../winusb/src/BonDriver_PX4/chset.cpp"
#undef ifstream

static bool run(const std::string &content, bool expected, bool check_channel = false)
{
	char path[] = "/tmp/px4-chset-test-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return false;
	bool ok = write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
	close(fd);
	std::string name(path);
	std::wstring wide(name.begin(), name.end());
	px4::ChannelSet channels;
	bool loaded = channels.Load(wide, px4::SystemType::ISDB_C);
	if (loaded && check_channel) {
		ok = ok && channels.ExistsChannel(0, 0);
		if (ok) {
			const auto &channel = channels.GetChannel(0, 0);
			ok = channel.name == L"Name" && channel.ptx_ch == 3 && channel.tsid == 0;
		}
	}
	unlink(path);
	if (!ok || loaded != expected) {
		fprintf(stderr, "ChSet load: expected %d, got %d\n", expected, loaded);
		return false;
	}
	return true;
}

static std::wstring write_temp(const std::string &content, std::string &name)
{
	char path[] = "/tmp/px4-chset-test-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return std::wstring();
	bool ok = write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
	close(fd);
	name = path;
	return ok ? std::wstring(name.begin(), name.end()) : std::wstring();
}

static bool loaded_channel(px4::ChannelSet &channels, const wchar_t *expected)
{
	return channels.ExistsChannel(0, 0) && channels.GetChannel(0, 0).name == expected;
}

/*
 * 指定 ChSet を読めないときの既定 ChSet への切り替えは、既存の INI では維持し、
 * 機種専用 ChSet を指定した INI では止められる (別機種の周波数補正を使わない)。
 */
static bool run_fallback()
{
	std::string explicit_name, default_name, broken_name;
	const std::wstring explicit_path = write_temp("$Space\t0\nExplicit\t0\t0\t13\t0\n", explicit_name);
	const std::wstring default_path = write_temp("$Space\t0\nDefault\t0\t0\t13\t0\n", default_name);
	const std::wstring broken_path = write_temp("$Space\t0\nPartial\t0\t0\t13\t0\n;" + std::string(300, 'x') + "\n", broken_name);
	const std::wstring missing_path = L"/tmp/px4-chset-test-missing";
	bool ok = !explicit_path.empty() && !default_path.empty() && !broken_path.empty();

	px4::ChannelSet configured;
	ok = ok && configured.LoadWithFallback(&explicit_path, default_path, false, px4::SystemType::ISDB_C) &&
		loaded_channel(configured, L"Explicit");

	px4::ChannelSet compatible;
	ok = ok && compatible.LoadWithFallback(&missing_path, default_path, true, px4::SystemType::ISDB_C) &&
		loaded_channel(compatible, L"Default");

	px4::ChannelSet unset;
	ok = ok && unset.Load(explicit_path, px4::SystemType::ISDB_C) &&
		!unset.LoadWithFallback(nullptr, default_path, false, px4::SystemType::ISDB_C) &&
		!unset.ExistsSpace(0);

	px4::ChannelSet unset_compatible;
	ok = ok && unset_compatible.LoadWithFallback(nullptr, default_path, true, px4::SystemType::ISDB_C) &&
		loaded_channel(unset_compatible, L"Default");

	px4::ChannelSet missing;
	ok = ok && !missing.LoadWithFallback(&missing_path, default_path, false, px4::SystemType::ISDB_C) &&
		!missing.ExistsSpace(0);

	/* 途中まで読んだ表も残さない。 */
	px4::ChannelSet broken;
	ok = ok && !broken.LoadWithFallback(&broken_path, default_path, false, px4::SystemType::ISDB_C) &&
		!broken.ExistsSpace(0);

	px4::ChannelSet broken_compatible;
	ok = ok && broken_compatible.LoadWithFallback(&broken_path, default_path, true, px4::SystemType::ISDB_C) &&
		loaded_channel(broken_compatible, L"Default") && !broken_compatible.ExistsChannel(0, 1);

	unlink(explicit_name.c_str());
	unlink(default_name.c_str());
	unlink(broken_name.c_str());
	if (!ok)
		fprintf(stderr, "ChSet fallback selection failed\n");
	return ok;
}

/* 配布する T230C 用 ChSet が、ISDB-C の列の規則で全行読めること。 */
static bool run_distributed_t230c()
{
	const std::string name = PX4_REPO_ROOT "/winusb/pkg/BonDriver_PX4/BonDriver_T230C-C.ChSet.txt";
	std::ifstream src(name);
	std::string line;
	unsigned int rows = 0;
	while (std::getline(src, line))
		if (!line.empty() && line[0] != ';' && line[0] != '$')
			rows++;
	px4::ChannelSet channels;
	channels.SetStrict(true);
	const bool ok = rows && channels.Load(std::wstring(name.begin(), name.end()), px4::SystemType::ISDB_C) &&
		channels.ExistsChannel(0, rows - 1) && !channels.ExistsChannel(0, rows);
	if (!ok)
		fprintf(stderr, "distributed T230C ChSet failed to load (%u rows)\n", rows);
	return ok;
}

/* torne 用の地デジ ChSet が ISDB-T の列として全行読み込めること。 */
static bool run_distributed_torne()
{
	const std::string name = PX4_REPO_ROOT "/winusb/pkg/BonDriver_PX4/BonDriver_Torne-T.ChSet.txt";
	std::ifstream src(name);
	std::string line;
	unsigned int rows = 0;
	while (std::getline(src, line))
		if (!line.empty() && line[0] != ';' && line[0] != '$')
			rows++;
	px4::ChannelSet channels;
	channels.SetStrict(true);
	const bool ok = rows && channels.Load(std::wstring(name.begin(), name.end()), px4::SystemType::ISDB_T) &&
		channels.ExistsChannel(0, rows - 1) && !channels.ExistsChannel(0, rows);
	if (!ok)
		fprintf(stderr, "distributed torne ChSet failed to load (%u rows)\n", rows);
	return ok;
}

int main()
{
	return run_distributed_t230c() && run_distributed_torne() &&
		run("$Space\t0\r\nName\t0\t0\t3\t0\r\n", true, true) &&
		run("$Space\t0\nName\t0\t0\t3\t0", true, true) &&
		run("$Space\t0\n;" + std::string(300, 'x') + "\n", false) &&
		run("$Space\t0\n;" + std::string(254, 'x') + "\n", true) &&
		run_fallback() ? 0 : 1;
}
