# SPDX-License-Identifier: Apache-2.0
include("${MPX_ROOT}/cmake/guest_runtime.cmake")
include("${MPX_ROOT}/cmake/guest_audio.cmake")
set(_guest "${_repo}/guest/tests/conformance/audio_pcm_stream.cpp")
set(_header "${CMAKE_CURRENT_BINARY_DIR}/generated/audio_guest_aot.h")
find_program(MPX_AUDIO_WAMRC NAMES wamrc REQUIRED)
add_custom_command(OUTPUT "${_header}"
    COMMAND python3 "${MPX_ROOT}/tools/build_audio_guest.py"
        --wamrc "${MPX_AUDIO_WAMRC}" --source "${_guest}" --output "${_header}"
    DEPENDS "${_guest}" ${_guest_sdk} "${_repo}/tools/micropixel" "${MPX_AUDIO_WAMRC}"
        "${MPX_ROOT}/tools/build_audio_guest.py"
    VERBATIM)

target_sources(app PRIVATE "${_header}")
sdk_set_main_file("${MPX_ROOT}/examples/audio_test.cpp")
set_source_files_properties("${MPX_ROOT}/examples/audio_test.cpp"
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
