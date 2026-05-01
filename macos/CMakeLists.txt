cmake_minimum_required(VERSION 3.15)
project(DriverHost_PX4_macOS CXX C)

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Find libusb-1.0
# Use pkg-config first, fall back to Homebrew/MacPorts paths.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
endif()

if(NOT LIBUSB_FOUND)
    # Homebrew (both Intel and Apple Silicon)
    foreach(BREW_PREFIX /opt/homebrew /usr/local)
        if(EXISTS "${BREW_PREFIX}/include/libusb-1.0/libusb.h")
            set(LIBUSB_INCLUDE_DIRS "${BREW_PREFIX}/include/libusb-1.0")
            set(LIBUSB_LIBRARIES    "${BREW_PREFIX}/lib/libusb-1.0.dylib")
            set(LIBUSB_FOUND TRUE)
            break()
        endif()
    endforeach()
endif()

if(NOT LIBUSB_FOUND)
    message(FATAL_ERROR
        "libusb-1.0 not found.\n"
        "Install it with:  brew install libusb")
endif()

# ---------------------------------------------------------------------------
# Root of the repo (parent of this directory)
# ---------------------------------------------------------------------------
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(MACOS_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(WINUSB_SRC    "${REPO_ROOT}/winusb/src")
set(DRIVER_DIR    "${REPO_ROOT}/driver")

# ---------------------------------------------------------------------------
# Include paths (order matters – macOS overrides come first)
# ---------------------------------------------------------------------------
include_directories(
    "${MACOS_SRC}/DriverHost_PX4"       # macOS-specific headers (device_notifier, etc.)
    "${MACOS_SRC}/common"               # macOS-specific config/util/guid_compat
    "${MACOS_SRC}/include"              # Fake Windows headers (windows.h, guiddef.h, …)
    "${WINUSB_SRC}/DriverHost_PX4"      # Shared device-logic headers
    "${WINUSB_SRC}/common"              # Shared common headers (command.hpp, type.hpp, …)
    "${DRIVER_DIR}"                     # it930x.h, tc90522.h, i2c_comm.h, …
    "${LIBUSB_INCLUDE_DIRS}"
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
add_compile_definitions(
    $<$<CONFIG:Debug>:_DEBUG>
)

# Suppress noisy warnings from code that was written for MSVC
add_compile_options(
    -Wno-multichar
    -Wno-deprecated-declarations
    -Wno-nontrivial-memcall
    -Wno-switch
)

# Force-include misc_posix.h for all C++ translation units so that shared
# winusb files (device_definition_set.cpp, etc.) get the MSVC safe-string
# stubs (swprintf_s, wcscat_s, …) even when they don't include misc_win.h.
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-include${MACOS_SRC}/DriverHost_PX4/misc_posix.h>
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

# macOS-specific C sources
set(MACOS_C_SRCS
    "${MACOS_SRC}/common/msg.c"
    "${MACOS_SRC}/DriverHost_PX4/misc_posix.c"
    "${MACOS_SRC}/DriverHost_PX4/itedtv_bus_libusb.c"
)

# macOS-specific C++ sources
set(MACOS_CXX_SRCS
    "${MACOS_SRC}/common/config.cpp"
    "${MACOS_SRC}/common/util.cpp"
    "${MACOS_SRC}/common/pipe.cpp"
    "${MACOS_SRC}/DriverHost_PX4/device_base.cpp"
    "${MACOS_SRC}/DriverHost_PX4/device_notifier.cpp"
    "${MACOS_SRC}/DriverHost_PX4/device_manager.cpp"
    "${MACOS_SRC}/DriverHost_PX4/pipe_server.cpp"
    "${MACOS_SRC}/DriverHost_PX4/ctrl_server.cpp"
    "${MACOS_SRC}/DriverHost_PX4/stream_server.cpp"
    "${MACOS_SRC}/DriverHost_PX4/driver_host.cpp"
    "${MACOS_SRC}/DriverHost_PX4/main.cpp"
)

# Shared device-logic C++ sources (compiled from the winusb tree)
set(SHARED_CXX_SRCS
    "${WINUSB_SRC}/DriverHost_PX4/px4_device.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/pxmlt_device.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/isdb2056_device.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/isdbt2071_device.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/receiver_base.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/receiver_manager.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/device_definition_set.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/ringbuffer.cpp"
    "${WINUSB_SRC}/DriverHost_PX4/server_base.cpp"
)

# ITE IT930x / tuner driver C sources (shared with Linux kernel module)
set(DRIVER_C_SRCS
    "${DRIVER_DIR}/it930x.c"
    "${DRIVER_DIR}/cxd2856er.c"
    "${DRIVER_DIR}/cxd2858er.c"
    "${DRIVER_DIR}/tc90522.c"
    "${DRIVER_DIR}/r850.c"
    "${DRIVER_DIR}/rt710.c"
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(DriverHost_PX4
    ${MACOS_C_SRCS}
    ${MACOS_CXX_SRCS}
    ${SHARED_CXX_SRCS}
    ${DRIVER_C_SRCS}
)

target_link_directories(DriverHost_PX4 PRIVATE ${LIBUSB_LIBRARY_DIRS})
target_link_libraries(DriverHost_PX4
    PRIVATE
    ${LIBUSB_LIBRARIES}
)

# macOS framework needed for IOKit (used internally by libusb)
target_link_libraries(DriverHost_PX4
    PRIVATE
    "-framework CoreFoundation"
    "-framework IOKit"
)

# ---------------------------------------------------------------------------
# px4rec – TS recording client (mirakurun tuner command)
# ---------------------------------------------------------------------------
set(B25_SRC "${MACOS_SRC}/tools/b25")

add_executable(px4rec
    "${MACOS_SRC}/tools/px4rec.cpp"
    "${B25_SRC}/arib_std_b25.c"
    "${B25_SRC}/b_cas_card.c"
    "${B25_SRC}/multi2.c"
    "${B25_SRC}/ts_section_parser.c"
)

target_include_directories(px4rec PRIVATE
    "${MACOS_SRC}/DriverHost_PX4"
    "${MACOS_SRC}/common"
    "${MACOS_SRC}/include"
    "${WINUSB_SRC}/DriverHost_PX4"
    "${WINUSB_SRC}/common"
    "${DRIVER_DIR}"
    "${B25_SRC}"
)

target_link_libraries(px4rec PRIVATE
    "-framework PCSC"
)

# ---------------------------------------------------------------------------
# Install: copy the binary and the INI next to each other
# ---------------------------------------------------------------------------
install(TARGETS DriverHost_PX4 px4rec DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/DriverHost_PX4.ini" DESTINATION bin)

# ---------------------------------------------------------------------------
# Post-build: copy INI to the build directory for convenient local testing
# ---------------------------------------------------------------------------
add_custom_command(TARGET DriverHost_PX4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/DriverHost_PX4.ini"
        "$<TARGET_FILE_DIR:DriverHost_PX4>/DriverHost_PX4.ini"
    COMMENT "Copying DriverHost_PX4.ini to build directory"
)
