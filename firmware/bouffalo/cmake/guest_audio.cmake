# SPDX-License-Identifier: Apache-2.0
if(NOT CONFIG_XCODEC_IO)
    message(FATAL_ERROR "Orion Guest audio requires CONFIG_XCODEC_IO; use a fresh BUILD_DIR")
endif()

set(_audio_sources
    "${MPX_ROOT}/runtime/abi/audio_endpoint.cpp"
    "${MPX_SHARED_HOST}/runtime/abi/url_media_endpoint.cpp"
    "${MPX_SHARED_HOST}/device/device_services.cpp"
    "${MPX_SHARED_HOST}/platform/audio/audio_engine.cpp"
    "${MPX_SHARED_HOST}/platform/audio/audio_mixer.cpp"
    "${MPX_SHARED_HOST}/runtime/audio/pcm_stream_service.cpp")
target_sources(app PRIVATE ${_audio_sources})
set_source_files_properties(${_audio_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
