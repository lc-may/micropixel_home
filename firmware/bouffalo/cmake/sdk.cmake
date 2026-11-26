# SPDX-License-Identifier: Apache-2.0
get_filename_component(MPX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(MPX_SHARED_HOST "${MPX_ROOT}/../common" ABSOLUTE)
if(MPX_SDK_BOARD)
    set(_sdk_board_args --board "${MPX_SDK_BOARD}")
endif()
execute_process(COMMAND python3 "${MPX_ROOT}/tools/check_sdk.py" --exact ${_sdk_board_args}
    COMMAND_ERROR_IS_FATAL ANY)
# Load the exact configuration whose provenance was checked above. HINTS would
# allow CMAKE_PREFIX_PATH, package registries or a cached *_DIR to override it.
include("$ENV{BL_SDK_BASE}/cmake/bouffalo_sdk-config.cmake")
if(MPX_SDK_BOARD AND NOT BOARD STREQUAL MPX_SDK_BOARD)
    message(FATAL_ERROR "SDK selected BOARD=${BOARD}, expected ${MPX_SDK_BOARD}. Use a fresh BUILD_DIR.")
endif()

set(MPX_NATIVE_VIDEO_TEST OFF)
if(BOARD STREQUAL "bl618dg_orion" AND CONFIG_DISPLAY_DEFAULT_TYPE_VIDEO AND CONFIG_LVGL9_PORT_DISP)
    set(MPX_NATIVE_VIDEO_TEST ON)
endif()

if(BOARD STREQUAL "bl618dg_orion" AND CPU_MODEL STREQUAL "b1")
    # The pinned SDK links the C++ standard library only for the b0 model.
    sdk_add_link_libraries(stdc++)
    if(MPX_HOST_PORT OR MPX_NATIVE_VIDEO_TEST)
        # The SDK's unsuffixed BL618DG multimedia and PKA archives are ILP32F.
        # Use its ILP32D archives for the Orion AP while keeping CPU_MODEL=b1.
        get_property(_libraries GLOBAL PROPERTY SDK_LIBS)
        list(TRANSFORM _libraries REPLACE "(/lib(multimedia|pka)_bl618dg)[.]a$" "\\1_b0.a")
        set_property(GLOBAL PROPERTY SDK_LIBS ${_libraries})
    endif()
endif()

if(MPX_NATIVE_VIDEO_TEST)
    return()
endif()

sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(MPX_HOST_PORT)
    # The portable Wasm Host uses the SDK ABI, including its prebuilt libraries.
    return()
endif()

# Public MicroPixel RV32 AOT bundles use the single-float ABI on both chips.
sdk_add_compile_options(-mabi=ilp32f)
sdk_add_link_options(-mabi=ilp32f)

# The ILP32F runtime and audio profiles do not need these vendor archives.
get_property(_libraries GLOBAL PROPERTY SDK_LIBS)
list(FILTER _libraries EXCLUDE REGEX "/(libmultimedia_|libpka_|libpec_v2_)[^/]*[.]a$")
set_property(GLOBAL PROPERTY SDK_LIBS ${_libraries})
