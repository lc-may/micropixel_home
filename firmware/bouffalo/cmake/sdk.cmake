# SPDX-License-Identifier: Apache-2.0
get_filename_component(MPX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
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

# Public MicroPixel RV32 AOT bundles use the single-float ABI on both chips.
sdk_add_compile_options(-mabi=ilp32f)
sdk_add_link_options(-mabi=ilp32f)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# The SDK adds these prebuilt ILP32D libraries unconditionally on DG B0.
# This foundation uses neither multimedia nor PKA. Drop them rather than
# disabling ABI diagnostics or rewriting vendor objects.
get_property(_libraries GLOBAL PROPERTY SDK_LIBS)
list(FILTER _libraries EXCLUDE REGEX "/(libmultimedia_|libpka_|libpec_v2_)[^/]*[.]a$")
set_property(GLOBAL PROPERTY SDK_LIBS ${_libraries})
