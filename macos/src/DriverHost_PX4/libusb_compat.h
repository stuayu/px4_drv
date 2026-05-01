// libusb_compat.h - libusb-based usb_device definition replacing winusb_compat.h

#pragma once

#include <stdint.h>
#include <libusb.h>

// ---------------------------------------------------------------------------
// USB descriptor types (mirrors Windows USB_DEVICE_DESCRIPTOR / USB_STRING_DESCRIPTOR)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

typedef struct {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

typedef struct {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	wchar_t  bString[128];   // UTF-32 on macOS (wchar_t = 4 bytes)
} USB_STRING_DESCRIPTOR;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// usb_device - wraps a libusb device handle and cached descriptors
// ---------------------------------------------------------------------------
struct usb_device {
	libusb_device_handle *handle;
	USB_DEVICE_DESCRIPTOR descriptor;
	USB_STRING_DESCRIPTOR *serial;
};
