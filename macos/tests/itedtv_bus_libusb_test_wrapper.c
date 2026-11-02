#include <stddef.h>

void *itedtv_test_calloc(size_t count, size_t size);

/* 実装をそのままコンパイルし、work配列の確保失敗だけを注入する。 */
#define calloc itedtv_test_calloc
#include "../src/DriverHost_PX4/itedtv_bus_libusb.c"
