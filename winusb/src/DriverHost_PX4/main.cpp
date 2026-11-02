// main.cpp

#define msg_prefix	"DriverHost_PX4"

#include <string>
#include <iostream>
#include <stdexcept>

#include <windows.h>

#include "driver_host.hpp"
#include "config.hpp"
#include "log_settings.hpp"
#include "usb_history.h"
#include "util.hpp"
#include "msg.h"

#ifdef USE_HIGH_RESOLUTION_TIMER
#pragma comment(lib, "winmm.lib")
#endif

/* 環境変数を読む。GetEnvironmentVariable は未設定と空の区別ができ、CRT の getenv より警告が少ない。 */
static bool GetLogEnv(const char *name, std::string &value)
{
	wchar_t wname[64];
	wchar_t buf[256];
	size_t i;

	for (i = 0; i < sizeof(wname) / sizeof(wname[0]) - 1 && name[i]; i++)
		wname[i] = static_cast<wchar_t>(name[i]);
	wname[i] = L'\0';

	const DWORD len = GetEnvironmentVariableW(wname, buf, sizeof(buf) / sizeof(buf[0]));
	if (len == 0 || len >= sizeof(buf) / sizeof(buf[0]))
		return false;

	value.clear();
	for (DWORD j = 0; j < len; j++)
		value.push_back((buf[j] < 0x80) ? static_cast<char>(buf[j]) : '?');
	return true;
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR pCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
	FILE *fp;

	AllocConsole();

	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);

	msg_set_mode(MSG_MODE_CONSOLE);
#else
	msg_set_mode(MSG_MODE_DEBUGGER);
#endif

	/* ログの設定は DriverHost_PX4.exe と同じ場所の INI から読むため、先にパスを確定する。 */
	px4::util::path::Init(hInstance);

	SetCurrentDirectoryW(px4::util::path::GetDir().c_str());

	/* ログの設定。環境変数 PX4_LOG_* は INI の値より優先する。 */
	{
		px4::Config ini;
		const bool loaded = ini.Load(px4::util::path::GetFileBase() + L".ini", L"DriverHost_PX4");
		const bool usb_history = px4::ApplyLogSettings(loaded ? &ini : nullptr, GetLogEnv);

		/* LogLevel=trace の診断では制御転送の履歴も必要になるため、同時に有効にする。 */
		usb_history_set_enabled(usb_history || msg_log_get_level() >= PX4_LOG_TRACE);
	}

	msg_info("Start\n");

#ifdef USE_HIGH_RESOLUTION_TIMER
	UINT timer_resolution = 1;
	TIMECAPS tc;

	if (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR)
		timer_resolution = tc.wPeriodMin;

	if (timeBeginPeriod(timer_resolution) != TIMERR_NOERROR)
		timer_resolution = 0;

	msg_info("Timer Resolution: %u\n", timer_resolution);
#endif

	try {
		px4::DriverHost host;

		host.Run();
	} catch (const std::runtime_error & e) {
		msg_err("%s\n", e.what());
		MessageBoxA(nullptr, e.what(), "DriverHost_PX4 (wWinMain)", MB_OK | MB_ICONERROR);
	}

#ifdef USE_HIGH_RESOLUTION_TIMER
	if (timer_resolution)
		timeEndPeriod(timer_resolution);
#endif

	msg_info("Exiting...\n");

#ifdef _DEBUG
	Sleep(2000);
	FreeConsole();
#endif

	return 0;
}
