cmake_minimum_required(VERSION 3.20)
project(micropixel_pc LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(REPOSITORY_ROOT "../.." ABSOLUTE)
set(HOST "${REPOSITORY_ROOT}/firmware/espressif/main")
set(PC_DEPS "${REPOSITORY_ROOT}/build/pc-deps" CACHE PATH "Downloaded PC dependencies")
set(PC_DEPENDENCY_ROOT "${PC_DEPS}/compiler/root" CACHE PATH "Optional locally unpacked Ubuntu development packages")
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}" "${HOST}" "${REPOSITORY_ROOT}/guest")
if(EXISTS "${PC_DEPENDENCY_ROOT}/usr/include")
    include_directories(SYSTEM "${PC_DEPENDENCY_ROOT}/usr/include" "${PC_DEPENDENCY_ROOT}/usr/include/x86_64-linux-gnu")
    link_directories("${PC_DEPENDENCY_ROOT}/usr/lib/x86_64-linux-gnu" "${PC_DEPENDENCY_ROOT}/usr/lib")
    set(CMAKE_BUILD_RPATH "${PC_DEPENDENCY_ROOT}/usr/lib/x86_64-linux-gnu;${PC_DEPENDENCY_ROOT}/usr/lib")
endif()

set(WAMR_ROOT_DIR "${REPOSITORY_ROOT}/firmware/espressif/components/wasm-micro-runtime")
set(WAMR_BUILD_PLATFORM "linux")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
add_library(pc_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_link_libraries(pc_wamr PUBLIC Threads::Threads m dl)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PC_DEPS}/lvgl" lvgl)
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

set(OPUS "${PC_DEPS}/micro-opus")
set(OGG "${OPUS}/lib/micro-ogg-demuxer")
file(GLOB OGG_SOURCES "${OGG}/src/*.cpp")
add_library(pc_opus STATIC "${OPUS}/src/ogg_opus_decoder.cpp" "${OPUS}/src/opus_header.cpp" ${OGG_SOURCES})
target_include_directories(pc_opus PUBLIC "${OPUS}/include" "${OGG}/include" "${PC_DEPENDENCY_ROOT}/usr/include/opus")
target_link_libraries(pc_opus PUBLIC opus)

set(HTTP "${PC_DEPS}/coreHTTP")
set(MQTT "${PC_DEPS}/coreMQTT")
set(LLHTTP "${HTTP}/source/dependency/3rdparty/llhttp")
add_library(pc_protocols STATIC "${HTTP}/source/core_http_client.c"
    "${LLHTTP}/src/api.c" "${LLHTTP}/src/http.c" "${LLHTTP}/src/llhttp.c"
    "${MQTT}/source/core_mqtt.c" "${MQTT}/source/core_mqtt_serializer.c" "${MQTT}/source/core_mqtt_state.c")
target_include_directories(pc_protocols PUBLIC "${HTTP}/source/include" "${HTTP}/source/interface"
    "${MQTT}/source/include" "${LLHTTP}/include")
target_compile_definitions(pc_protocols PUBLIC HTTP_DO_NOT_USE_CUSTOM_CONFIG MQTT_DO_NOT_USE_CUSTOM_CONFIG)
# The socket transport is nonblocking. Internet response latency routinely
# exceeds coreHTTP's 10 ms defaults, even after a successful TLS handshake.
target_compile_definitions(pc_protocols PRIVATE
    HTTP_RECV_RETRY_TIMEOUT_MS=10000U HTTP_SEND_RETRY_TIMEOUT_MS=10000U)

set(SHARED_SOURCES
    host/controller/app_manager.cpp
    host/ui/system_locale.cpp host/ui/lvgl/square_common/hall_card_ui.cpp
    device/device_registry.cpp device/device_services.cpp device/text.cpp
    runtime/abi/guest_abi.cpp runtime/abi/native_symbols.c runtime/abi/service_endpoints.cpp runtime/abi/service_registry.cpp
    runtime/bundle/bundle_reader.c runtime/bundle/bundle_section_reader.cpp runtime/bundle/memory_bundle_source.c
    runtime/guest_context.cpp runtime/event_queue.cpp runtime/key_event_bridge.cpp runtime/touch_event_bridge.cpp
    runtime/components.cpp
    runtime/audio/audio_playback_service.cpp runtime/audio/pcm_stream_service.cpp runtime/graphics/raster_kernels.cpp
    runtime/resources/bitmap_decoder.cpp runtime/resources/bitmap_store.cpp runtime/resources/resource_service.cpp
    runtime/services/storage_service.cpp runtime/services/app_storage.cpp runtime/services/direct_surface_service.cpp
    runtime/services/gpio_service.cpp runtime/services/haptics_service.cpp runtime/services/raster_service.cpp
    runtime/services/sensor_service.cpp runtime/services/timer_service.cpp work/background_executor.cpp
    platform/adapters/graphics_adapter.cpp platform/audio/audio_mixer.cpp
    platform/graphics/app_surface_compositor.cpp platform/graphics/guest_scene.cpp platform/graphics/pixel_compositor.cpp
    platform/lvgl/display/direct_surface_presenter.cpp platform/lvgl/display/dirty_region_coalescer.cpp
    platform/lvgl/display/scanout_arbiter.cpp platform/lvgl/display/scanout_stage_pool.cpp
    platform/lvgl/fonts/bitmap_font_rasterizer.cpp platform/lvgl/fonts/font_cbin_loader.cpp platform/lvgl/fonts/font_registry.cpp
    platform/lvgl/guest_graphics_engine.cpp platform/lvgl/guest_graphics_operations.cpp platform/lvgl/lvgl_software_pixel_compositor.cpp
    platform/lvgl/fonts/builtin/font_builtin_latin_14.c platform/lvgl/fonts/builtin/font_builtin_latin_18.c
    platform/lvgl/fonts/builtin/font_builtin_latin_24.c platform/lvgl/fonts/builtin/font_builtin_latin_32.c)
list(TRANSFORM SHARED_SOURCES PREPEND "${HOST}/")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB HOST_CATALOGS CONFIGURE_DEPENDS "${HOST}/host/ui/i18n/*.json")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
    COMMAND "${Python3_EXECUTABLE}" "${REPOSITORY_ROOT}/tools/generate_localization.py"
        --catalog-dir "${HOST}/host/ui/i18n" --default-locale en --cpp-namespace host_strings
        --output-header "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
        --report "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.json"
    DEPENDS ${HOST_CATALOGS} "${REPOSITORY_ROOT}/tools/generate_localization.py")
add_library(pc_host STATIC ${SHARED_SOURCES} os.cpp storage.cpp jpeg.cpp audio.cpp portable_app.cpp session.cpp rv32_session.cpp network.cpp media.cpp market.cpp store.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp")
target_include_directories(pc_host PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(pc_host PUBLIC MICROPIXEL_PC_COMPONENTS=1)
target_link_libraries(pc_host PUBLIC pc_wamr lvgl pc_opus pc_protocols PkgConfig::PNG PkgConfig::CJSON sqlite3 jpeg mbedtls mbedx509 mbedcrypto opusfile ogg mpg123)
target_compile_options(pc_host PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
target_sources(pc_host PRIVATE app_sessions.cpp remote.cpp)
add_executable(micropixel-pc main.cpp home_app.cpp)
target_compile_options(micropixel-pc PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(micropixel-pc PRIVATE pc_host)

add_executable(pc-app-framework-test "${REPOSITORY_ROOT}/tools/tests/test_pc_app_framework.cpp")
target_compile_options(pc-app-framework-test PRIVATE -UNDEBUG -fno-exceptions -fno-rtti)
target_link_libraries(pc-app-framework-test PRIVATE pc_host)
