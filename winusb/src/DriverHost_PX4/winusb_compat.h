// winusb_compat.h

#pragma once

#ifdef __APPLE__
#include "libusb_compat.h"
#else

#include <windows.h>
#include <winusb.h>

struct usb_device {
	HANDLE dev;
	WINUSB_INTERFACE_HANDLE winusb;
	USB_DEVICE_DESCRIPTOR descriptor;
};

#endif /* __APPLE__ */
