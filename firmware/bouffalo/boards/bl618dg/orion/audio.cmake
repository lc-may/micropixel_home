# SPDX-License-Identifier: Apache-2.0
set(_orion "${MPX_ROOT}/boards/bl618dg/orion")
sdk_add_include_directories("${_orion}")
target_sources(app PRIVATE "${_orion}/audio/codec_pins.c" "${_orion}/audio/orion_audio_output.cpp")
set_source_files_properties("${_orion}/audio/orion_audio_output.cpp"
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")

# Replace only the GPIO initializer called by xcodec. Keep the pinned SDK's
# codec lifecycle, shared I2C lock, I2S data interface and DMA definitions.
get_target_property(_sdk_sources app SOURCES)
set(_board_audio "$ENV{BL_SDK_BASE}/bsp/board/bl618dg_orion/board_audio.c")
if(NOT _board_audio IN_LIST _sdk_sources)
    message(FATAL_ERROR "Expected the Orion xcodec board source")
endif()
set_source_files_properties("${_board_audio}" PROPERTIES
    COMPILE_DEFINITIONS board_i2s_codec_gpio_init=mpx_orion_i2s_codec_gpio_init)
