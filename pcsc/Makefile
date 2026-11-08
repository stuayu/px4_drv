CXX ?= c++
PKG_CONFIG ?= pkg-config
BUILD ?= build
ROOT := ..
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)

MACOS_SDK ?= $(shell xcrun --sdk macosx --show-sdk-path)
MACOS_PCSC_HEADERS ?= $(MACOS_SDK)/System/Library/Frameworks/PCSC.framework/Versions/A/Headers
MACOS_BUNDLE := $(BUILD)/ifd-px4.bundle/Contents
MACOS_BINARY := $(MACOS_BUNDLE)/MacOS/libifd-px4.dylib
MACOS_INFO := $(MACOS_BUNDLE)/Info.plist
MACOS_IFD_DIR ?= /usr/libexec/SmartCardServices/drivers
MACOS_SOURCES := ifdhandler_macos.cpp
MACOS_HEADERS := $(ROOT)/host/common/card_command.hpp vendor/ifdhandler.h

CPPFLAGS += -I$(ROOT)/host/common -Ivendor -I$(MACOS_PCSC_HEADERS) -I.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread

all: macos-bundle

macos-bundle: $(MACOS_BINARY) $(MACOS_INFO)

$(MACOS_BINARY): $(MACOS_SOURCES) $(MACOS_HEADERS) exports.macos | $(BUILD)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -bundle -Wl,-undefined,dynamic_lookup \
		-Wl,-exported_symbols_list,exports.macos $(MACOS_SOURCES) -o $@

$(MACOS_INFO): Info-macos.plist | $(BUILD)
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD):
	mkdir -p $@

install: macos-bundle
	mkdir -p "$(DESTDIR)$(MACOS_IFD_DIR)/ifd-px4.bundle/Contents/MacOS"
	install -m 0755 $(MACOS_BINARY) "$(DESTDIR)$(MACOS_IFD_DIR)/ifd-px4.bundle/Contents/MacOS/libifd-px4.dylib"
	install -m 0644 $(MACOS_INFO) "$(DESTDIR)$(MACOS_IFD_DIR)/ifd-px4.bundle/Contents/Info.plist"

.PHONY: all install macos-bundle

else

PCSC_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libpcsclite)
USBDROPDIR ?= $(shell $(PKG_CONFIG) --variable=usbdropdir libpcsclite)
SHARED := $(ROOT)/host/core
CPPFLAGS += -I$(ROOT)/include -I$(SHARED) $(PCSC_CFLAGS)
CPPFLAGS += -I.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
SOURCES := ifdhandler.cpp linux_card_device.cpp $(SHARED)/smart_card.cpp
HEADERS := linux_card_device.hpp $(SHARED)/card_device.hpp $(SHARED)/smart_card.hpp $(ROOT)/include/px4_card_ioctl.h

all: $(BUILD)/libifd-px4.so

$(BUILD):
	mkdir -p $@

$(BUILD)/libifd-px4.so: $(SOURCES) $(HEADERS) exports.map | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared $(SOURCES) $(LDFLAGS) -Wl,--version-script=exports.map -o $@

$(BUILD)/smart_card_state_test: $(ROOT)/winusb/tests/smart_card_state_test.cpp $(SHARED)/smart_card.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(SHARED)/smart_card.cpp $(LDFLAGS) -o $@

$(BUILD)/ifdhandler_test: tests/ifdhandler_test.cpp $(SOURCES) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(SOURCES) $(LDFLAGS) -Wl,--wrap=open,--wrap=close,--wrap=ioctl -rdynamic -ldl -o $@

check: $(BUILD)/smart_card_state_test $(BUILD)/ifdhandler_test
	$(BUILD)/smart_card_state_test
	$(BUILD)/ifdhandler_test
	python3 tests/check_card_guids.py

$(BUILD)/libifd-px4-test.so: tests/ifdhandler_test.cpp $(SOURCES) $(HEADERS) exports.map | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -DPX4_IFD_FAKE_MODULE -fPIC -shared $< $(SOURCES) $(LDFLAGS) -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--version-script=exports.map -o $@

$(BUILD)/pcsc_integration_test: tests/pcsc_integration_test.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libpcsclite) -o $@

integration-build: $(BUILD)/libifd-px4-test.so $(BUILD)/pcsc_integration_test

install: all
	test -n "$(USBDROPDIR)"
	install -D -m 0755 $(BUILD)/libifd-px4.so $(DESTDIR)$(USBDROPDIR)/ifd-px4.bundle/Contents/Linux/libifd-px4.so
	install -D -m 0644 Info.plist $(DESTDIR)$(USBDROPDIR)/ifd-px4.bundle/Contents/Info.plist

.PHONY: all check install integration-build

endif
