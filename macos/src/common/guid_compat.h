// guid_compat.h - Portable GUID definition for non-Windows platforms

#pragma once

#include <stdint.h>
#include <string.h>

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} GUID;
#endif

static inline int IsEqualGUID(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

#ifdef __cplusplus
static inline bool operator==(const GUID &a, const GUID &b) noexcept
{
	return memcmp(&a, &b, sizeof(GUID)) == 0;
}
static inline bool operator!=(const GUID &a, const GUID &b) noexcept
{
	return memcmp(&a, &b, sizeof(GUID)) != 0;
}
#endif
