// util.cpp - macOS-specific utility implementations

#include "util.hpp"

#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace px4 {

namespace util {

bool HexFromStr(const wchar_t c, std::uint8_t &v)
{
	if (c >= L'0' && c <= L'9')
		v = static_cast<uint8_t>(c - L'0');
	else if (c >= L'A' && c <= L'F')
		v = static_cast<uint8_t>(c - L'A' + 0x0a);
	else if (c >= L'a' && c <= L'f')
		v = static_cast<uint8_t>(c - L'a' + 0x0a);
	else
		return false;

	return true;
}

bool HexStrToUInt8(const wchar_t *str, std::uint8_t &v)
{
	std::uint8_t t;

	if (!HexFromStr(str[0], t))
		return false;

	v = t << 4;

	if (!HexFromStr(str[1], t))
		return false;

	v |= t;

	return true;
}

bool ParseGuidStr(const std::wstring &str, GUID &guid)
{
	GUID tmp = {};
	const wchar_t *p = str.c_str();
	wchar_t *end;

	if (std::wcslen(p) != 38 ||
	    p[0]  != L'{' ||
	    p[9]  != L'-' ||
	    p[14] != L'-' ||
	    p[19] != L'-' ||
	    p[24] != L'-' ||
	    p[37] != L'}')
		return false;

	tmp.Data1 = static_cast<uint32_t>(std::wcstoul(&p[1], &end, 16));
	if (end != &p[9])
		return false;

	tmp.Data2 = static_cast<uint16_t>(std::wcstoul(&p[10], &end, 16));
	if (end != &p[14])
		return false;

	tmp.Data3 = static_cast<uint16_t>(std::wcstoul(&p[15], &end, 16));
	if (end != &p[19])
		return false;

	for (int i = 0, j = 0; i < 17; i += 2, j++) {
		if (i == 4)
			i++;

		if (!HexStrToUInt8(&p[20 + i], tmp.Data4[j]))
			return false;
	}

	guid = tmp;

	return true;
}

bool ParseSystemStr(const std::wstring &str, px4::SystemType &systems)
{
	const wchar_t *head, *tail, *p, *split;

	systems = px4::SystemType::UNSPECIFIED;

	head = p = str.c_str();
	tail = head + std::wcslen(head);

	while (p <= tail) {
		p += std::wcsspn(p, L", \t");

		split = p + std::wcscspn(p, L", \t");
		if (split == p)
			split = tail;

		if (!std::wcsncmp(L"ISDB-T", p, 6))
			systems |= px4::SystemType::ISDB_T;
		else if (!std::wcsncmp(L"ISDB-S", p, 6))
			systems |= px4::SystemType::ISDB_S;
		else {
			systems = px4::SystemType::UNSPECIFIED;
			return false;
		}

		p = split + 1;
	}

	return true;
}

namespace path {

static std::wstring dir_path;
static std::wstring file_base;

bool Init(void */* mod */)
{
	char buf[PATH_MAX];
	uint32_t size = sizeof(buf);

	if (_NSGetExecutablePath(buf, &size) != 0)
		return false;

	// Resolve symlinks
	char resolved[PATH_MAX];
	if (!realpath(buf, resolved))
		std::strncpy(resolved, buf, PATH_MAX - 1);

	// Find last slash (directory separator)
	char *filename = std::strrchr(resolved, '/');
	if (!filename)
		filename = resolved;

	// Find extension
	char *fileext = std::strrchr(filename, '.');
	if (fileext)
		*fileext = '\0';

	// Convert to wstring (ASCII paths are safe)
	file_base.clear();
	for (const char *c = resolved; *c; c++)
		file_base += static_cast<wchar_t>(*c);

	*filename = '\0';
	dir_path.clear();
	for (const char *c = resolved; *c; c++)
		dir_path += static_cast<wchar_t>(*c);

	return true;
}

const std::wstring& GetDir() noexcept
{
	return dir_path;
}

const std::wstring& GetFileBase() noexcept
{
	return file_base;
}

} // namespace path

bool ShiftJisToUtf16(const char *sjis, std::wstring &utf16)
{
	/* On macOS the DriverHost only uses this for Windows-specific code paths
	 * that are never reached.  Return a best-effort ASCII conversion. */
	utf16.clear();
	if (!sjis) return false;
	while (*sjis)
		utf16 += static_cast<wchar_t>((unsigned char)*sjis++);
	return true;
}

} // namespace util

} // namespace px4
