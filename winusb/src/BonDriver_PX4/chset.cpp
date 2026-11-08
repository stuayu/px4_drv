// chset.cpp

#include "chset.hpp"

#include <mutex>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include "isdbc_channel.h"
#include "util.hpp"

namespace px4 {

bool ChannelSet::Load(const std::wstring &path, px4::SystemType system) noexcept
{
	std::lock_guard<std::shared_mutex> lock(mtx_);
	std::ifstream ifs(path);

	if (!ifs.is_open())
		return false;

	std::unordered_map<std::uint32_t, std::uint32_t> space_id_table;

	while (true) {
		char line[256];
		char *line_head, *tmp_head, *tmp_tail = nullptr;

		if (!ifs.getline(line, 256)) {
			/* 長すぎる行や I/O エラーを EOF と区別し、部分的な表を成功扱いにしない。 */
			if (ifs.bad() || !ifs.eof())
				return false;
			break;
		}

		/* gcount には区切り文字も含まれるため、文字列の長さで CR を検査する。 */
		const std::size_t line_length = std::strlen(line);
		if (line_length && line[line_length - 1] == '\r')
			line[line_length - 1] = '\0';

		// Empty separator lines must not be interpreted as channel 0.  In
		// particular, a blank line after a CATV space used to replace C13 (the
		// first channel) with an empty entry.
		line_head = line;
		while (*line_head == ' ' || *line_head == '\t')
			line_head++;

		if (*line_head == '\0' || *line_head == ';') {
			// comment
			continue;
		}

		if (*line_head == '$') {
			// tuning space
			std::wstring name;
			std::uint32_t space_id;

			tmp_head = line_head + 1;

			// name
			Split(&tmp_head, &tmp_tail, '\t');
			px4::util::ShiftJisToUtf16(tmp_head, name);

			// space
			Split(&tmp_head, &tmp_tail, '\t');
			space_id = px4::util::atoui32(tmp_head);

			space_id_table.emplace(space_id, next_space_id_);

			if (!AddSpace(next_space_id_++, &name, system))
				return false;
		} else {
			// channel
			std::uint32_t space_id, ch_id;
			ChannelInfo ch;

			tmp_head = line_head;

			// name
			Split(&tmp_head, &tmp_tail, '\t');
			px4::util::ShiftJisToUtf16(tmp_head, ch.name);

			// space
			Split(&tmp_head, &tmp_tail, '\t');
			space_id = px4::util::atoui32(tmp_head);

			try {
				space_id = space_id_table.at(space_id);
			} catch (const std::out_of_range&) {
				if (strict_)
					return false;
				else
					continue;
			}

			// ch
			Split(&tmp_head, &tmp_tail, '\t');
			ch_id = px4::util::atoui32(tmp_head);

			if (system == px4::SystemType::ISDB_C || system == px4::SystemType::J83B) {
				if (!ParseCable(&tmp_head, &tmp_tail, system, ch)) {
					if (strict_)
						return false;
					else
						continue;
				}
			} else {
				// ptx_ch
				Split(&tmp_head, &tmp_tail, '\t');
				ch.ptx_ch = px4::util::atoui32(tmp_head);

				// tsid
				Split(&tmp_head, &tmp_tail, '\t');
				ch.tsid = static_cast<std::uint16_t>(px4::util::atoui32(tmp_head));
			}

			if (!AddChannel(space_id, ch_id, ch))
				return false;
		}
	}

	return true;
}

bool ChannelSet::LoadWithFallback(const std::wstring *path, const std::wstring &default_path, bool allow_fallback,
	px4::SystemType system, bool *used_fallback) noexcept
{
	if (used_fallback)
		*used_fallback = false;

	if (!path && !allow_fallback) {
		Clear();
		return false;
	}

	if (path) {
		if (Load(*path, system))
			return true;

		Clear();
		if (!allow_fallback)
			return false;
		if (used_fallback)
			*used_fallback = true;
	}

	if (Load(default_path, system))
		return true;

	Clear();
	return false;
}

bool ChannelSet::Merge(ChannelSet &chset) noexcept
{
	// TODO: Space ID をずらす
	try {
		for (auto it = chset.spaces_.cbegin(); it != chset.spaces_.cend(); ++it)
			spaces_.emplace_back(*it);

		return true;
	} catch (...) {
		return false;
	}
}

bool ChannelSet::ExistsSpace(std::uint32_t space_id) const noexcept
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	return (spaces_.size() > space_id);
}

const std::wstring& ChannelSet::GetSpaceName(std::uint32_t space_id) const
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	return spaces_.at(space_id).name;
}

px4::SystemType ChannelSet::GetSpaceSystem(std::uint32_t space_id) const
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	return spaces_.at(space_id).system;
}

bool ChannelSet::ExistsChannel(std::uint32_t space_id, std::uint32_t ch_id) const noexcept
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	try {
		return !!spaces_.at(space_id).channels.count(ch_id);
	} catch (const std::out_of_range&) {
		return false;
	}
}

const px4::ChannelSet::ChannelInfo& ChannelSet::GetChannel(std::uint32_t space_id, std::uint32_t ch_id) const
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	return spaces_.at(space_id).channels.at(ch_id);
}

bool ChannelSet::AddSpace(std::uint32_t space_id, const std::wstring *name, px4::SystemType system) noexcept
{
	if (spaces_.size() > space_id) {
		auto& space = spaces_.at(space_id);

		if (name && space.name.empty())
			space.name = *name;

		if (space.system == px4::SystemType::UNSPECIFIED)
			space.system = system;
	} else if (name) {
		spaces_.emplace_back(SpaceInfo(*name, system));
	} else if (!strict_) {
		spaces_.emplace_back(SpaceInfo(system));
	} else {
		return false;
	}

	return true;
}

bool ChannelSet::AddChannel(std::uint32_t space_id, std::uint32_t ch_id, const ChannelInfo &ch) noexcept
{
	if (!AddSpace(space_id, nullptr))
		return false;

	auto& space = spaces_.at(space_id);

	if (space.channels.count(ch_id))
		space.channels.at(ch_id) = ch;
	else
		space.channels.emplace(ch_id, ch);

	return true;
}

void ChannelSet::Split(char **head, char **tail, char c) noexcept
{
	char *h = *head, *t = *tail;

	if (t)
		h = t + 1;

	t = std::strchr(h, c);
	if (t)
		*t = '\0';

	*head = h;
	*tail = t;

	return;
}

bool ChannelSet::ParseCable(char **head, char **tail, px4::SystemType system, ChannelInfo &ch) noexcept
{
	auto parse = [](const char *str, long min, long max, long &value) noexcept {
		char *end;

		value = std::strtol(str, &end, 10);
		return end != str && *end == '\0' && value >= min && value <= max;
	};
	long value;

	// J.83 Annex B は地域によって 6.5MHz 刻みからずれた周波数で送られるため、番号から計算せずに中心周波数 (kHz) を書く
	// 範囲は ISDB-C と同じ VHF 1ch から UHF 62ch までの帯域で、TSMF を使わないため列はここで終わる
	if (system == px4::SystemType::J83B) {
		if (!*tail)
			return false;
		Split(head, tail, '\t');
		if (!parse(*head, 90000, 770000, value) || *tail)
			return false;
		ch.freq = static_cast<std::uint32_t>(value);

		return true;
	}

	// 現行の物理チャンネル番号 (0〜112) と旧形式の番号 (113〜124)
	if (!*tail)
		return false;
	Split(head, tail, '\t');
	if (!parse(*head, 0, 124, value))
		return false;
	const long physical_channel = value;

	// 5列の現行形式は物理chとTSMF番号、6/7列の旧形式は物理ch・TSID・中心周波数・補正。
	if (!*tail)
		return false;
	Split(head, tail, '\t');
	if (!*tail) {
		if (physical_channel > 112 || !parse(*head, 0, 14, value))
			return false;
		ch.ptx_ch = static_cast<std::uint32_t>(physical_channel);
		ch.tsmf_stream = static_cast<std::uint8_t>(value);
		return true;
	}

	long tsid;
	if (!parse(*head, 0, 65535, tsid))
		return false;
	ch.tsid = static_cast<std::uint16_t>(tsid);

	Split(head, tail, '\t');
	if (!parse(*head, 0, 1000000, value))
		return false;
	if (*tail) {
		Split(head, tail, '\t');
		if (!parse(*head, -3000, 3000, value) || *tail)
			return false;
	}

	long current_channel;
	if (physical_channel >= 113 && physical_channel <= 115) {
		current_channel = physical_channel - 113;
	} else if (physical_channel >= 116 && physical_channel <= 124) {
		current_channel = physical_channel - 103;
	} else {
		current_channel = physical_channel;
	}
	if (isdbc_channel_to_khz(static_cast<int>(current_channel)) < 0)
		return false;

	/* 旧形式の周波数値と補正は現行の共有物理ch表に置き換えるため、構文と範囲だけ検証する。 */
	ch.ptx_ch = static_cast<std::uint32_t>(current_channel);
	ch.tsmf_stream = 0;

	return true;
}

} // namespace px4
