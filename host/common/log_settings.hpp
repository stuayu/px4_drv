// log_settings.hpp - DriverHost_PX4 のログ設定を INI と環境変数から適用する
//
// 優先順位は環境変数、INI の [DriverHost_PX4]、既定値の順。
// 不正な値は警告を出し、直前の設定を変えずに残す。

#pragma once

#include <string>

#include "config.hpp"
#include "msg.h"
#include "msg_log.h"
#include "util.hpp"

namespace px4 {

namespace log_settings_detail {

/* INI と環境変数の値は ASCII の範囲で書く。範囲外の文字は不正値として扱う。 */
inline std::string ToNarrow(const std::wstring &value)
{
	std::string result;
	result.reserve(value.size());
	for (wchar_t c : value)
		result.push_back((c >= 0 && c < 0x80) ? static_cast<char>(c) : '?');
	return result;
}

} // namespace log_settings_detail

/*
 * ini は [DriverHost_PX4] を読み込み済みの設定 (読めなかった場合は nullptr)。
 * get_env は環境変数の値を value へ写し、存在しなければ false を返す。
 * 戻り値は LogUsbHistory=true が指定されたかどうか。
 * 呼び出し側は、この値と LogLevel=trace のどちらかで制御転送の履歴を有効にする。
 */
inline bool ApplyLogSettings(const Config *ini, bool (*get_env)(const char *name, std::string &value))
{
	static const struct {
		const char *name;
		const wchar_t *ini_key;
		const char *env_name;
	} keys[] = {
		{ "LogLevel", L"LogLevel", "PX4_LOG_LEVEL" },
		{ "LogCategories", L"LogCategories", "PX4_LOG_CATEGORIES" },
		{ "LogFormat", L"LogFormat", "PX4_LOG_FORMAT" },
		{ "LogStatsInterval", L"LogStatsInterval", "PX4_LOG_STATS_INTERVAL" },
	};

	for (const auto &key : keys) {
		if (ini && ini->Exists(key.ini_key)) {
			const std::string value = log_settings_detail::ToNarrow(ini->Get(key.ini_key));
			if (!msg_log_apply_setting(key.name, value.c_str()))
				msg_warn("DriverHost_PX4.ini の %s=%s は無効です。既定値を使います。\n", key.name, value.c_str());
		}

		std::string env_value;
		if (key.env_name && get_env(key.env_name, env_value)) {
			if (!msg_log_apply_setting(key.name, env_value.c_str()))
				msg_warn("環境変数 %s=%s は無効です。設定を変えません。\n", key.env_name, env_value.c_str());
		}
	}

	return ini && ini->Exists(L"LogUsbHistory") && px4::util::wtob(ini->Get(L"LogUsbHistory"));
}

} // namespace px4
