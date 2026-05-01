// device_base.cpp - macOS libusb-based USB device base (replaces WinUSB version)

#include "device_base.hpp"

#include <cinttypes>
#include <cwchar>

#include "msg.h"

namespace px4 {

DeviceBase::DeviceBase(const std::wstring &path,
		       const px4::DeviceDefinition &device_def,
		       std::uintptr_t index,
		       px4::ReceiverManager &receiver_manager)
	: device_def_(device_def),
	  receiver_manager_(receiver_manager)
{
	strncpy_s(dev_.driver_name, "px4_libusb", sizeof("px4_libusb"));
	sprintf_s(dev_.device_name, "%" PRIuPTR, index);

	usb_dev_.handle = nullptr;
	usb_dev_.serial = nullptr;
	memset(&usb_dev_.descriptor, 0, sizeof(usb_dev_.descriptor));

	/* Parse path "BUS:ADDR" where both values are decimal */
	unsigned long bus_n = 0, addr_n = 0;
	const wchar_t *p = path.c_str();
	wchar_t *endp;
	bus_n = wcstoul(p, &endp, 10);
	if (!endp || *endp != L':')
		throw DeviceError("px4::DeviceBase: invalid path (expected BUS:ADDR)");
	addr_n = wcstoul(endp + 1, nullptr, 10);

	auto bus  = static_cast<uint8_t>(bus_n);
	auto addr = static_cast<uint8_t>(addr_n);

	/* Enumerate libusb devices and find the matching one */
	libusb_device **list = nullptr;
	ssize_t cnt = libusb_get_device_list(nullptr, &list);
	if (cnt < 0)
		throw DeviceError("px4::DeviceBase: libusb_get_device_list() failed");

	libusb_device *found = nullptr;
	struct libusb_device_descriptor desc = {};

	for (ssize_t i = 0; i < cnt; i++) {
		if (libusb_get_bus_number(list[i])     == bus &&
		    libusb_get_device_address(list[i]) == addr) {
			found = list[i];
			libusb_get_device_descriptor(found, &desc);
			break;
		}
	}

	if (!found) {
		libusb_free_device_list(list, 1);
		throw DeviceError("px4::DeviceBase: device not found");
	}

	/* Copy descriptor fields into our USB_DEVICE_DESCRIPTOR */
	usb_dev_.descriptor.bLength            = desc.bLength;
	usb_dev_.descriptor.bDescriptorType    = desc.bDescriptorType;
	usb_dev_.descriptor.bcdUSB             = desc.bcdUSB;
	usb_dev_.descriptor.bDeviceClass       = desc.bDeviceClass;
	usb_dev_.descriptor.bDeviceSubClass    = desc.bDeviceSubClass;
	usb_dev_.descriptor.bDeviceProtocol    = desc.bDeviceProtocol;
	usb_dev_.descriptor.bMaxPacketSize0    = desc.bMaxPacketSize0;
	usb_dev_.descriptor.idVendor           = desc.idVendor;
	usb_dev_.descriptor.idProduct          = desc.idProduct;
	usb_dev_.descriptor.bcdDevice          = desc.bcdDevice;
	usb_dev_.descriptor.iManufacturer      = desc.iManufacturer;
	usb_dev_.descriptor.iProduct           = desc.iProduct;
	usb_dev_.descriptor.iSerialNumber      = desc.iSerialNumber;
	usb_dev_.descriptor.bNumConfigurations = desc.bNumConfigurations;

	/* Open the device */
	int ret = libusb_open(found, &usb_dev_.handle);
	libusb_free_device_list(list, 1); /* unrefs all including found */

	if (ret < 0)
		throw DeviceError("px4::DeviceBase: libusb_open() failed");

	/* Detach kernel driver if needed and claim interface 0 */
	libusb_set_auto_detach_kernel_driver(usb_dev_.handle, 1);
	ret = libusb_claim_interface(usb_dev_.handle, 0);
	if (ret < 0) {
		libusb_close(usb_dev_.handle);
		usb_dev_.handle = nullptr;
		throw DeviceError("px4::DeviceBase: libusb_claim_interface() failed");
	}

	/* Read serial number string (ASCII) and store as wchar_t */
	if (desc.iSerialNumber) {
		unsigned char serial_buf[128] = {};
		int r = libusb_get_string_descriptor_ascii(
			usb_dev_.handle, desc.iSerialNumber,
			serial_buf, sizeof(serial_buf) - 1);
		if (r > 0) {
			usb_dev_.serial = new USB_STRING_DESCRIPTOR();
			memset(usb_dev_.serial->bString, 0,
			       sizeof(usb_dev_.serial->bString));
			for (int i = 0; i < r && i < 127; i++)
				usb_dev_.serial->bString[i] =
					static_cast<wchar_t>(serial_buf[i]);
			usb_dev_.serial->bLength          = static_cast<uint8_t>(2 + r * 2);
			usb_dev_.serial->bDescriptorType  = 3; /* USB_STRING_DESCRIPTOR_TYPE */
		}
	}
}

DeviceBase::~DeviceBase()
{
	if (usb_dev_.serial) {
		delete usb_dev_.serial;
		usb_dev_.serial = nullptr;
	}

	if (usb_dev_.handle) {
		libusb_release_interface(usb_dev_.handle, 0);
		libusb_close(usb_dev_.handle);
		usb_dev_.handle = nullptr;
	}
}

} // namespace px4
