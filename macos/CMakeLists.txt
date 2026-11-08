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
set(HOST_SRC      "${REPO_ROOT}/host")

# ---------------------------------------------------------------------------
# Include paths (order matters – macOS overrides come first)
# ---------------------------------------------------------------------------
include_directories(
    "${MACOS_SRC}/DriverHost_PX4"       # macOS-specific headers (device_notifier, etc.)
    "${MACOS_SRC}/common"               # macOS-specific config/util/guid_compat
    "${MACOS_SRC}/include"              # Fake Windows headers (windows.h, guiddef.h, …)
    "${HOST_SRC}/core"                 # Shared host core headers
    "${HOST_SRC}/devices"              # Shared device classes
    "${HOST_SRC}/common"               # Shared host headers (command.hpp, type.hpp, ...)
    "${WINUSB_SRC}/DriverHost_PX4"      # WinUSB-only headers still shared (none expected)
    "${WINUSB_SRC}/common"              # Shared common headers (command.hpp, type.hpp, …)
    "${DRIVER_DIR}/common/include"
    "${DRIVER_DIR}/common/bridge"
    "${DRIVER_DIR}/common/demod"
    "${DRIVER_DIR}/common/tuner"
    "${DRIVER_DIR}/common/ts"
    "${DRIVER_DIR}/common/signal"
    "${DRIVER_DIR}/common/torne"
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
    "${HOST_SRC}/common/msg_log.c"
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
    "${HOST_SRC}/core/ctrl_server.cpp"
    "${HOST_SRC}/core/stream_server.cpp"
    "${MACOS_SRC}/DriverHost_PX4/driver_host.cpp"
    "${MACOS_SRC}/DriverHost_PX4/main.cpp"
)

# Shared device-logic C++ sources (compiled from the winusb tree)
set(SHARED_CXX_SRCS
    "${HOST_SRC}/devices/px4_device.cpp"
    "${HOST_SRC}/devices/pxmlt_device.cpp"
    "${HOST_SRC}/devices/t230c_device.cpp"
    "${HOST_SRC}/devices/t230c_protocol.cpp"
    "${HOST_SRC}/core/ts_stream_diagnostics.cpp"
    "${HOST_SRC}/devices/t230c_silicon.cpp"
    "${HOST_SRC}/devices/siano_device.cpp"
    "${HOST_SRC}/devices/siano_protocol.cpp"
    "${HOST_SRC}/devices/siano_wire.cpp"
    "${HOST_SRC}/devices/isdb2056_device.cpp"
    "${HOST_SRC}/devices/isdbt2071_device.cpp"
    "${HOST_SRC}/core/receiver_base.cpp"
    "${HOST_SRC}/core/ts_stats.cpp"
    "${HOST_SRC}/core/usb_history.cpp"
    "${HOST_SRC}/core/receiver_manager.cpp"
    "${HOST_SRC}/core/device_definition_set.cpp"
    "${HOST_SRC}/core/device_factory.cpp"
    "${HOST_SRC}/core/ringbuffer.cpp"
    "${HOST_SRC}/core/server_base.cpp"
    "${HOST_SRC}/core/smart_card.cpp"
    "${HOST_SRC}/core/card_server.cpp"
)

# ITE IT930x / tuner driver C sources (shared with Linux kernel module)
set(DRIVER_C_SRCS
    "${DRIVER_DIR}/common/bridge/it930x.c"
    "${DRIVER_DIR}/common/bridge/siano_wire_core.c"
    "${DRIVER_DIR}/common/bridge/t230c_usb.c"
    "${DRIVER_DIR}/common/signal/signal_stat.c"
    "${DRIVER_DIR}/common/demod/cxd2856er.c"
    "${DRIVER_DIR}/common/demod/cxd2858er.c"
    "${DRIVER_DIR}/common/demod/si2168.c"
    "${DRIVER_DIR}/common/demod/tc90522.c"
    "${DRIVER_DIR}/common/tuner/r850.c"
    "${DRIVER_DIR}/common/tuner/rt710.c"
    "${DRIVER_DIR}/common/tuner/si2141.c"
    "${DRIVER_DIR}/common/ts/t230c_ts_sync.c"
    "${DRIVER_DIR}/common/ts/tsmf.c"
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
install(FILES
    "${REPO_ROOT}/winusb/pkg/DriverHost_PX4/isdbt_rio.inp"
    "${REPO_ROOT}/winusb/pkg/DriverHost_PX4/LICENCE.siano"
    DESTINATION bin)

# ---------------------------------------------------------------------------
# Post-build: copy INI to the build directory for convenient local testing
# ---------------------------------------------------------------------------
add_custom_command(TARGET DriverHost_PX4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/DriverHost_PX4.ini"
        "$<TARGET_FILE_DIR:DriverHost_PX4>/DriverHost_PX4.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${REPO_ROOT}/winusb/pkg/DriverHost_PX4/isdbt_rio.inp"
        "$<TARGET_FILE_DIR:DriverHost_PX4>/isdbt_rio.inp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${REPO_ROOT}/winusb/pkg/DriverHost_PX4/LICENCE.siano"
        "$<TARGET_FILE_DIR:DriverHost_PX4>/LICENCE.siano"
    COMMENT "Copying DriverHost_PX4 runtime files to build directory"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(diagnostics_test
        "${REPO_ROOT}/tests/diagnostics_test.cpp"
        "${HOST_SRC}/core/ts_stats.cpp"
        "${HOST_SRC}/core/usb_history.cpp"
        "${HOST_SRC}/common/msg_log.c"
        "${DRIVER_DIR}/common/signal/signal_stat.c"
        "${MACOS_SRC}/common/msg.c")
    add_test(NAME diagnostics_test COMMAND diagnostics_test)

    add_executable(signal_stat_test
        "${REPO_ROOT}/tests/signal_stat_test.c"
        "${DRIVER_DIR}/common/signal/signal_stat.c")
    target_compile_definitions(signal_stat_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(signal_stat_test PRIVATE
        "${DRIVER_DIR}/common/signal"
        "${MACOS_SRC}/DriverHost_PX4"
        "${HOST_SRC}/common")
    target_link_libraries(signal_stat_test PRIVATE m)
    add_test(NAME signal_stat_test COMMAND signal_stat_test)

    # USB deviceの取得だけをテスト側で置き換え、実際の受信callbackを使う。
    set(STREAM_TEST_MACOS_CXX_SRCS ${MACOS_CXX_SRCS})
    list(REMOVE_ITEM STREAM_TEST_MACOS_CXX_SRCS
        "${MACOS_SRC}/DriverHost_PX4/device_base.cpp"
        "${MACOS_SRC}/DriverHost_PX4/main.cpp")
    add_executable(siano_wire_test
        "${WINUSB_SRC}/../tests/siano_wire_test.cpp"
        "${HOST_SRC}/devices/siano_wire.cpp"
        "${DRIVER_DIR}/common/bridge/siano_wire_core.c")
    add_test(NAME siano_wire_test COMMAND siano_wire_test)

    add_executable(siano_wire_c_test
        "${WINUSB_SRC}/../tests/siano_wire_c_test.c"
        "${DRIVER_DIR}/common/bridge/siano_wire_core.c")
    target_include_directories(siano_wire_c_test PRIVATE "${DRIVER_DIR}")
    add_test(NAME siano_wire_c_test COMMAND siano_wire_c_test)

    add_executable(siano_protocol_core_test
        "${WINUSB_SRC}/../tests/siano_protocol_core_test.c"
        "${DRIVER_DIR}/common/bridge/siano_protocol_core.c"
        "${DRIVER_DIR}/common/bridge/siano_wire_core.c")
    target_include_directories(siano_protocol_core_test PRIVATE "${DRIVER_DIR}")
    add_test(NAME siano_protocol_core_test COMMAND siano_protocol_core_test)

    add_executable(siano_protocol_test
        "${WINUSB_SRC}/../tests/siano_protocol_test.cpp"
        "${HOST_SRC}/devices/siano_protocol.cpp"
        "${HOST_SRC}/devices/siano_wire.cpp"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c"
        "${DRIVER_DIR}/common/bridge/siano_wire_core.c")
    add_test(NAME siano_protocol_test COMMAND siano_protocol_test)

    add_executable(torne_core_test
        "${REPO_ROOT}/tests/torne_core_test.c"
        "${DRIVER_DIR}/common/torne/torne_bridge.c"
        "${DRIVER_DIR}/common/torne/torne_dib.c")
    add_test(NAME torne_core_test COMMAND torne_core_test)

    # OS 間で機種の定義に差が無いことを確かめる (INI 2種類と Linux の USB ID 表)。
    set(PARITY_TEST_MACOS_CXX_SRCS ${MACOS_CXX_SRCS})
    list(REMOVE_ITEM PARITY_TEST_MACOS_CXX_SRCS "${MACOS_SRC}/DriverHost_PX4/main.cpp")
    add_executable(device_parity_test
        "${REPO_ROOT}/tests/device_parity_test.cpp"
        ${MACOS_C_SRCS} ${PARITY_TEST_MACOS_CXX_SRCS} ${SHARED_CXX_SRCS} ${DRIVER_C_SRCS})
    target_compile_definitions(device_parity_test PRIVATE PX4_REPO_ROOT="${REPO_ROOT}")
    target_link_directories(device_parity_test PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(device_parity_test PRIVATE ${LIBUSB_LIBRARIES}
        "-framework CoreFoundation" "-framework IOKit")
    add_test(NAME device_parity_test COMMAND device_parity_test)

    # PX4 系・PXMLT 系の共有ストリームが全転送を失った後の開始・停止・参照数を、
    # 模擬 bus と IT930x の制御応答で実際の SetCapture() から検証する。
    add_executable(shared_stream_failure_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_stream_failure_test.cpp"
        ${MACOS_C_SRCS} ${STREAM_TEST_MACOS_CXX_SRCS} ${SHARED_CXX_SRCS} ${DRIVER_C_SRCS})
    target_link_directories(shared_stream_failure_test PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(shared_stream_failure_test PRIVATE ${LIBUSB_LIBRARIES}
        "-framework CoreFoundation" "-framework IOKit")
    add_test(NAME shared_stream_failure_test COMMAND shared_stream_failure_test)

    # libusbの失敗・cancel完了を模擬し、実際のmacOS転送層を検証する。
    add_executable(itedtv_usb_failure_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/itedtv_usb_failure_test.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/itedtv_bus_libusb_test_wrapper.c"
        "${HOST_SRC}/core/usb_history.cpp"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c")
    add_test(NAME itedtv_usb_failure_test COMMAND itedtv_usb_failure_test)

    # 作った TS で、TSMF の TS 番号による選択・ヘッダーのない搬送波の素通し・分割 TLV の判定を確認する。
    add_executable(tsmf_filter_test
        "${REPO_ROOT}/winusb/tests/tsmf_filter_test.c"
        "${DRIVER_DIR}/common/ts/tsmf.c")
    add_test(NAME tsmf_filter_test COMMAND tsmf_filter_test)

    add_executable(j83b_driver_test
        "${REPO_ROOT}/winusb/tests/j83b_driver_test.c"
        "${DRIVER_DIR}/common/demod/cxd2856er.c"
        "${DRIVER_DIR}/common/demod/cxd2858er.c"
        "${DRIVER_DIR}/common/signal/signal_stat.c"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c")
    target_compile_definitions(j83b_driver_test PRIVATE _DEFAULT_SOURCE)
    add_test(NAME j83b_driver_test COMMAND j83b_driver_test)
    add_executable(ts_stream_diagnostics_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ts_stream_diagnostics_test.cpp"
        "${HOST_SRC}/core/ts_stream_diagnostics.cpp")
    add_test(NAME ts_stream_diagnostics_test COMMAND ts_stream_diagnostics_test)
    add_executable(t230c_driver_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/t230c_driver_test.cpp"
        "${HOST_SRC}/devices/t230c_protocol.cpp"
        "${HOST_SRC}/devices/t230c_silicon.cpp"
        "${DRIVER_DIR}/common/bridge/t230c_usb.c"
        "${DRIVER_DIR}/common/demod/si2168.c"
        "${DRIVER_DIR}/common/tuner/si2141.c"
        "${DRIVER_DIR}/common/ts/t230c_ts_sync.c"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c")
    add_test(NAME t230c_driver_test COMMAND t230c_driver_test)
    add_executable(t230c_common_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/t230c_common_test.c"
        "${DRIVER_DIR}/common/bridge/t230c_usb.c"
        "${DRIVER_DIR}/common/demod/si2168.c"
        "${DRIVER_DIR}/common/tuner/si2141.c"
        "${DRIVER_DIR}/common/ts/t230c_ts_sync.c"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c")
    add_test(NAME t230c_common_test COMMAND t230c_common_test)
    add_executable(t230c_pipe_timeout_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/t230c_pipe_timeout_test.cpp")
    add_test(NAME t230c_pipe_timeout_test COMMAND t230c_pipe_timeout_test)
    add_executable(px4rec_failure_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/px4rec_failure_test.cpp"
        "${B25_SRC}/arib_std_b25.c" "${B25_SRC}/b_cas_card.c"
        "${B25_SRC}/multi2.c" "${B25_SRC}/ts_section_parser.c")
    target_include_directories(px4rec_failure_test PRIVATE "${B25_SRC}")
    target_link_libraries(px4rec_failure_test PRIVATE "-framework PCSC")
    add_test(NAME px4rec_failure_test COMMAND px4rec_failure_test)
    add_executable(chset_loader_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/chset_loader_test.cpp"
        "${MACOS_SRC}/common/util.cpp")
    target_include_directories(chset_loader_test PRIVATE "${WINUSB_SRC}/BonDriver_PX4")
    target_compile_definitions(chset_loader_test PRIVATE PX4_REPO_ROOT="${REPO_ROOT}")
    add_test(NAME chset_loader_test COMMAND chset_loader_test)
    # 共通の StreamBuffer の停止要求・異常通知・ロック範囲と、データ ID の検索を単体で検証する。
    add_executable(stream_buffer_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_buffer_test.cpp"
        "${HOST_SRC}/core/receiver_base.cpp"
        "${DRIVER_DIR}/common/ts/tsmf.c"
        "${HOST_SRC}/core/ts_stats.cpp"
        "${HOST_SRC}/core/receiver_manager.cpp"
        "${HOST_SRC}/core/ringbuffer.cpp"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/DriverHost_PX4/misc_posix.c"
        "${MACOS_SRC}/common/msg.c")
    add_test(NAME stream_buffer_test COMMAND stream_buffer_test)

    # StreamBuffer の異常通知が data pipe の client にEOFを返すことを実接続で検証する。
    add_executable(stream_server_failure_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_server_failure_test.cpp"
        "${MACOS_SRC}/common/pipe.cpp"
        "${HOST_SRC}/common/msg_log.c"
        "${MACOS_SRC}/common/msg.c"
        "${MACOS_SRC}/DriverHost_PX4/misc_posix.c"
        "${MACOS_SRC}/DriverHost_PX4/pipe_server.cpp"
        "${HOST_SRC}/core/stream_server.cpp"
        "${HOST_SRC}/core/server_base.cpp"
        "${HOST_SRC}/core/receiver_base.cpp"
        "${DRIVER_DIR}/common/ts/tsmf.c"
        "${HOST_SRC}/core/ts_stats.cpp"
        "${HOST_SRC}/core/receiver_manager.cpp"
        "${HOST_SRC}/core/ringbuffer.cpp")
    target_compile_definitions(stream_server_failure_test PRIVATE _DEFAULT_SOURCE)
    add_test(NAME stream_server_failure_test COMMAND stream_server_failure_test)
    set_tests_properties(stream_server_failure_test PROPERTIES TIMEOUT 10)

    set_tests_properties(itedtv_usb_failure_test j83b_driver_test chset_loader_test stream_buffer_test PROPERTIES TIMEOUT 10)
    # 異常と開始要求の同時実行を繰り返すため、Sanitizer ビルドでも収まる上限にする。
    set_tests_properties(shared_stream_failure_test PROPERTIES TIMEOUT 60)
    # 指定時間の正常終了を複数回試すため、他の短時間テストとは別にする。
    set_tests_properties(px4rec_failure_test PROPERTIES TIMEOUT 30)
endif()
