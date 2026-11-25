# SPDX-License-Identifier: Apache-2.0
if(NOT CONFIG_XCODEC_IO)
    message(FATAL_ERROR "Orion Guest audio requires CONFIG_XCODEC_IO; use a fresh BUILD_DIR")
endif()

set(_audio_sources
    "${MPX_ROOT}/common/audio_endpoint.cpp"
    "${_host}/runtime/abi/url_media_endpoint.cpp"
    "${_host}/device/device_services.cpp"
    "${_host}/platform/audio/audio_engine.cpp"
    "${_host}/platform/audio/audio_mixer.cpp"
    "${_host}/runtime/audio/pcm_stream_service.cpp")
target_sources(app PRIVATE ${_audio_sources})
set_source_files_properties(${_audio_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
