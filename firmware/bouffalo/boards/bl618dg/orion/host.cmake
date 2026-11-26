# SPDX-License-Identifier: Apache-2.0
set(_board "${MPX_ROOT}/boards/bl618dg/orion")
set(MPX_HOST_MAIN "${_board}/firmware_app.cpp")
set(MPX_HOST_GUESTS sdk_hello main_failure timer_counter audio_pcm_stream)
set(MPX_HOST_APP_GUESTS settings clock-alarm weather netease-music news)
option(MPX_ORION_AOT "Compile Orion catalog Guests to native RISC-V AOT" ON)
set(MPX_ORION_RUNTIME_OPT "-O2" CACHE STRING "Orion hot-path optimization")
set(MPX_ORION_INTERPRETER_OPT "${MPX_ORION_RUNTIME_OPT}" CACHE STRING "Orion interpreter optimization")
set(MPX_ORION_GUEST_PROFILE "release" CACHE STRING "Orion embedded Guest compiler profile")
if(NOT MPX_ORION_RUNTIME_OPT MATCHES "^-O(s|2|3)$")
    message(FATAL_ERROR "ORION_RUNTIME_OPT must be -Os, -O2 or -O3")
endif()
if(NOT MPX_ORION_INTERPRETER_OPT MATCHES "^-O(s|2|3)$")
    message(FATAL_ERROR "ORION_INTERPRETER_OPT must be -Os, -O2 or -O3")
endif()
if(NOT MPX_ORION_GUEST_PROFILE MATCHES "^(release|performance)$")
    message(FATAL_ERROR "ORION_GUEST_PROFILE must be release or performance")
endif()
sdk_add_compile_definitions(-DMICROPIXEL_ORION_HOST=1)
set(_font_generator "${MPX_ROOT}/tools/build_orion_font.py")
set(_font_data "${CMAKE_CURRENT_BINARY_DIR}/generated/orion_font_data.h")
set(MPX_ORION_FONT_SOURCE "" CACHE FILEPATH "Local Source Han Sans SC Regular 2.004R OpenType font")
set(_font_source_args)
if(MPX_ORION_FONT_SOURCE)
    set(_font_source_args --source "${MPX_ORION_FONT_SOURCE}")
endif()
add_custom_command(OUTPUT "${_font_data}"
    COMMAND python3 "${_font_generator}" "${_font_data}" ${_font_source_args}
    DEPENDS "${_font_generator}" ${MPX_ORION_FONT_SOURCE}
    VERBATIM)
if(NOT CONFIG_BSP_LCD OR NOT CONFIG_LVGL9_PORT_DISP OR NOT CONFIG_XCODEC_IO)
    message(FATAL_ERROR "Orion Host requires its LCD and audio defconfig; use a fresh BUILD_DIR")
endif()
if(MPX_LCD_CONFIG)
    get_filename_component(_lcd_header "${MPX_LCD_CONFIG}" NAME)
    if(NOT _lcd_header STREQUAL "lcd_conf_user.h" OR NOT EXISTS "${MPX_LCD_CONFIG}")
        message(FATAL_ERROR "LCD_CONFIG must point to a working panel's lcd_conf_user.h")
    endif()
    get_filename_component(_lcd_dir "${MPX_LCD_CONFIG}" DIRECTORY)
    sdk_add_include_directories("${_lcd_dir}")
endif()
sdk_add_include_directories("${_board}" "${MPX_ROOT}/common"
    "$ENV{BL_SDK_BASE}/examples/wifi/sta/wifi_https"
    "$ENV{BL_SDK_BASE}/components/graphics/lvgl_v9/libs/gltf/stb_image")
if(CONFIG_BLUETOOTH AND NOT CONFIG_FHOST)
    sdk_add_compile_definitions(-DCONFIG_BTBLE_USE_STANDALONE_PATH=1)
endif()
# The flash OTP read temporarily disables XIP while RF params are loaded.
# Keep the SDK security register functions in TCM across LTO.
set_property(SOURCE "$ENV{BL_SDK_BASE}/drivers/lhal/src/flash/bflb_flash_secreg.c"
    TARGET_DIRECTORY liblhal APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
# The pinned SDK exposes this panel in lcd.h but omits it from the source list.
target_sources(app PRIVATE "$ENV{BL_SDK_BASE}/bsp/common/lcd/mipi_dsi/st7102_ydp395bt008_dsi.c")
set(_board_sources "${_board}/orion_home.cpp" "${_board}/orion_touch.cpp" "${_board}/orion_font.cpp"
    "${MPX_ROOT}/common/wireless.c" "${MPX_ROOT}/common/wireless_endpoint.cpp"
    "${MPX_ROOT}/common/guest_panel.cpp" "${MPX_ROOT}/common/image_decode.c"
    "${MPX_ROOT}/../espressif/main/platform/graphics/guest_scene.cpp"
    "${MPX_ROOT}/../espressif/main/device/text.cpp")
target_sources(app PRIVATE "${_font_data}" "${_board}/fonts/font_orion_cjk.c"
    "${_board}/fonts/font_orion_clock.c" "${_board}/fonts/font_orion_temperature.c"
    "${_board}/fonts/font_orion_home.c" "${_board}/fonts/font_orion_home_value.c")
if(CONFIG_FHOST AND NOT CONFIG_BLUETOOTH)
    list(APPEND _board_sources "${_board}/orion_wifi.c")
endif()
if(CONFIG_FHOST)
    set(MPX_ORION_WIFI_RX_BUFS 64 CACHE STRING "Orion Wi-Fi FHOST RX buffer count in WRAM")
    set_property(CACHE MPX_ORION_WIFI_RX_BUFS PROPERTY STRINGS 64 72)
    if(NOT MPX_ORION_WIFI_RX_BUFS MATCHES "^[1-9][0-9]*$" OR
       MPX_ORION_WIFI_RX_BUFS LESS 4 OR MPX_ORION_WIFI_RX_BUFS GREATER 65535)
        message(FATAL_ERROR "MPX_ORION_WIFI_RX_BUFS must be a valid 16-bit FHOST buffer count (at least 4)")
    endif()
    sdk_add_compile_definitions("-DFHOST_RX_BUF_CNT=${MPX_ORION_WIFI_RX_BUFS}")
    option(MPX_ORION_TCP_SACK "Enable TCP selective acknowledgments for Orion Wi-Fi" ON)
    if(MPX_ORION_TCP_SACK)
        sdk_add_compile_definitions(-DLWIP_TCP_SACK_OUT=1)
    endif()
    option(MPX_ORION_RX_BENCHMARK "Build the Orion LAN RX diagnostic shell commands" OFF)
    if(MPX_ORION_RX_BENCHMARK)
        sdk_add_compile_definitions(-DMICROPIXEL_ORION_RX_BENCHMARK=1)
        list(APPEND _board_sources "${_board}/wifi_rx_benchmark.c")
        sdk_add_link_options(-Wl,--wrap=macif_rx_buf_check -Wl,--wrap=tcp_send_empty_ack)
        # Keep tcp_input's empty ACK calls visible to the benchmark linker wrapper.
        set_property(SOURCE "$ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/core/tcp_in.c"
            TARGET_DIRECTORY liblwip APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
    endif()
    if(NOT CONFIG_PVMP3DEC)
        add_subdirectory("$ENV{BL_SDK_BASE}/components/multimedia/pvmp3dec" "${CMAKE_CURRENT_BINARY_DIR}/mpx_pvmp3")
    endif()
    target_compile_definitions(libmbedtls PRIVATE MBEDTLS_HAVE_TIME_DATE)
    set(_sntp "$ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/apps/sntp/sntp.c")
    set_property(SOURCE "${_sntp}" "${MPX_ROOT}/common/network_time.c" APPEND PROPERTY
        COMPILE_OPTIONS "-include;${MPX_ROOT}/common/sntp_config.h")
    list(APPEND _board_sources "${MPX_ROOT}/common/network.cpp" "${MPX_ROOT}/common/network_time.c"
        "${MPX_ROOT}/common/https_get.c" "${MPX_ROOT}/common/media.cpp")
    target_sources(app PRIVATE "${_sntp}")
endif()
if(CONFIG_LITTLEFS)
    list(APPEND _board_sources "${MPX_ROOT}/common/alarm_store.cpp" "${MPX_ROOT}/common/file_store.c")
endif()
list(APPEND _board_sources "${MPX_ROOT}/common/weather_endpoint.cpp")
target_sources(app PRIVATE ${_board_sources})
set_property(SOURCE ${_board_sources} APPEND PROPERTY COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
set_property(SOURCE "${MPX_ROOT}/../espressif/main/platform/graphics/guest_scene.cpp" APPEND
    PROPERTY COMPILE_OPTIONS "${MPX_ORION_RUNTIME_OPT}")
