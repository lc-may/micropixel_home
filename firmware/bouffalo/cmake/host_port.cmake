# SPDX-License-Identifier: Apache-2.0
include("${MPX_ROOT}/cmake/guest_runtime.cmake")
include("${MPX_ROOT}/cmake/guest_audio.cmake")
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_guest_options --profile "${MPX_ORION_GUEST_PROFILE}" --stack-size 32768)
set(_embed_options)
set(_embed_dependencies)
if(MPX_ORION_AOT)
    if(DEFINED ENV{WAMRC} AND NOT MPX_ORION_WAMRC)
        set(MPX_ORION_WAMRC "$ENV{WAMRC}" CACHE FILEPATH "Orion catalog AOT compiler")
    endif()
    find_program(MPX_ORION_WAMRC NAMES wamrc REQUIRED)
    execute_process(COMMAND "${MPX_ORION_WAMRC}" --version
        OUTPUT_VARIABLE _wamrc_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT _wamrc_version STREQUAL "wamrc 2.4.3")
        message(FATAL_ERROR "Orion AOT requires the pinned wamrc 2.4.3, got ${_wamrc_version}")
    endif()
    set(_embed_options --wamrc "${MPX_ORION_WAMRC}")
    set(_embed_dependencies "${MPX_ORION_WAMRC}")
endif()
set(_guest_options_file "${_generated}/guest-build-options.txt")
file(GENERATE OUTPUT "${_guest_options_file}" CONTENT "${_guest_options}\n${_embed_options}\n")
function(_mpx_build_orion_guest name source header)
    if(IS_DIRECTORY "${source}")
        file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${source}/*.cpp" "${source}/*.hpp" "${source}/app.json")
    else()
        set(_sources "${source}")
    endif()
    if(MPX_ORION_AOT)
        set(_output "${_generated}/${name}.aot.xz")
        set(_catalog_inputs ${_catalog_inputs} "${_output}" PARENT_SCOPE)
    else()
        set(_output "${_generated}/${header}")
        target_sources(app PRIVATE "${_output}")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND python3 "${_repo}/tools/pc.py" "${source}" ${_guest_options} --output-dir "${_generated}/${name}"
        COMMAND python3 "${MPX_ROOT}/tools/embed_wasm.py" "${_generated}/${name}/${name}.wasm" "${_output}" ${_embed_options}
        DEPENDS ${_sources} ${_guest_sdk} "${_guest_options_file}" "${_repo}/tools/pc.py" "${_repo}/tools/micropixel"
            "${_repo}/tools/build_app_bundle.py" "${MPX_ROOT}/tools/embed_wasm.py" ${_embed_dependencies}
        VERBATIM)
endfunction()
foreach(_name IN LISTS MPX_HOST_GUESTS)
    _mpx_build_orion_guest("${_name}" "${_repo}/guest/tests/conformance/${_name}.cpp" "${_name}.h")
endforeach()
foreach(_name IN LISTS MPX_HOST_APP_GUESTS)
    _mpx_build_orion_guest("${_name}" "${_repo}/guest/apps/${_name}" "mpx_guest_${_name}.h")
endforeach()
if(MPX_ORION_AOT)
    set(_pack "${MPX_ROOT}/tools/pack_orion_apps.py")
    set(_catalog_header "${_generated}/orion_aot_catalog.h")
    set(_catalog_image "${CMAKE_CURRENT_BINARY_DIR}/orion_apps.bin")
    add_custom_command(OUTPUT "${_catalog_header}" "${_catalog_image}"
        COMMAND python3 "${_pack}" "${MPX_HOST_PARTITIONS}" "${_catalog_header}" "${_catalog_image}" ${_catalog_inputs}
        DEPENDS "${_pack}" "${MPX_HOST_PARTITIONS}" ${_catalog_inputs}
        VERBATIM)
    add_custom_target(orion_apps DEPENDS "${_catalog_header}" "${_catalog_image}")
    add_dependencies(app orion_apps)
endif()

set(_host_port_sources
    "${MPX_ROOT}/host/controller/media_widgets.cpp"
    "${MPX_ROOT}/platform/graphics/image_decode.c"
    "${MPX_ROOT}/platform/wireless/wireless.c"
    "${MPX_ROOT}/runtime/abi/alarm_endpoint.cpp"
    "${MPX_ROOT}/runtime/abi/resource_endpoint.cpp"
    "${MPX_ROOT}/runtime/abi/weather_endpoint.cpp"
    "${MPX_ROOT}/runtime/abi/wireless_endpoint.cpp"
    "${MPX_ROOT}/runtime/graphics/guest_panel.cpp"
    "${MPX_ROOT}/runtime/guest_runner.cpp"
    "${MPX_SHARED_HOST}/device/text.cpp"
    "${MPX_SHARED_HOST}/host/controller/alarm_scheduler.cpp"
    "${MPX_SHARED_HOST}/host/controller/app_hub.cpp"
    "${MPX_SHARED_HOST}/host/controller/app_manager.cpp"
    "${MPX_SHARED_HOST}/runtime/abi/network_endpoint.cpp"
    "${MPX_SHARED_HOST}/runtime/resources/bitmap_store.cpp"
    "${MPX_SHARED_HOST}/platform/graphics/app_surface_compositor.cpp"
    "${MPX_SHARED_HOST}/platform/graphics/guest_scene.cpp"
    "${MPX_SHARED_HOST}/platform/graphics/pixel_compositor.cpp"
    "${MPX_SHARED_HOST}/platform/lvgl/fonts/bitmap_font_rasterizer.cpp")
sdk_add_include_directories("$ENV{BL_SDK_BASE}/components/graphics/lvgl_v9/libs/gltf/stb_image")
if(CONFIG_FHOST)
    sdk_add_include_directories("$ENV{BL_SDK_BASE}/examples/wifi/sta/wifi_https")
    if(NOT CONFIG_PVMP3DEC)
        add_subdirectory("$ENV{BL_SDK_BASE}/components/multimedia/pvmp3dec" "${CMAKE_CURRENT_BINARY_DIR}/mpx_pvmp3")
    endif()
    target_compile_definitions(libmbedtls PRIVATE MBEDTLS_HAVE_TIME_DATE)
    set(_sntp "$ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/apps/sntp/sntp.c")
    set_property(SOURCE "${_sntp}" "${MPX_ROOT}/platform/network/network_time.c" APPEND PROPERTY
        COMPILE_OPTIONS "-include;${MPX_ROOT}/platform/network/sntp_config.h")
    target_sources(app PRIVATE "${_sntp}")
    list(APPEND _host_port_sources
        "${MPX_ROOT}/host/network/network.cpp"
        "${MPX_ROOT}/platform/audio/media.cpp"
        "${MPX_ROOT}/platform/network/https_get.c"
        "${MPX_ROOT}/platform/network/network_time.c")
endif()
if(CONFIG_LITTLEFS)
    list(APPEND _host_port_sources
        "${MPX_ROOT}/host/controller/alarm_store.cpp"
        "${MPX_ROOT}/platform/storage/file_store.c")
endif()
target_sources(app PRIVATE ${_host_port_sources})
sdk_set_main_file("${MPX_HOST_MAIN}")
set_source_files_properties("${MPX_HOST_MAIN}" ${_host_port_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
set_property(SOURCE "${MPX_SHARED_HOST}/platform/graphics/app_surface_compositor.cpp"
    "${MPX_SHARED_HOST}/platform/graphics/guest_scene.cpp"
    "${MPX_SHARED_HOST}/platform/graphics/pixel_compositor.cpp" APPEND PROPERTY COMPILE_OPTIONS "${MPX_ORION_RUNTIME_OPT}")
