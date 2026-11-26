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
    set(_partitions "${MPX_ROOT}/boards/bl618dg/orion/partition_cfg_8M.toml")
    set(_catalog_header "${_generated}/orion_aot_catalog.h")
    set(_catalog_image "${CMAKE_CURRENT_BINARY_DIR}/orion_apps.bin")
    add_custom_command(OUTPUT "${_catalog_header}" "${_catalog_image}"
        COMMAND python3 "${_pack}" "${_partitions}" "${_catalog_header}" "${_catalog_image}" ${_catalog_inputs}
        DEPENDS "${_pack}" "${_partitions}" ${_catalog_inputs}
        VERBATIM)
    add_custom_target(orion_apps DEPENDS "${_catalog_header}" "${_catalog_image}")
    add_dependencies(app orion_apps)
endif()

set(_host_port_sources
    "${MPX_ROOT}/common/alarm_endpoint.cpp"
    "${MPX_ROOT}/common/guest_runner.cpp"
    "${MPX_ROOT}/common/media_widgets.cpp"
    "${MPX_ROOT}/common/resource_endpoint.cpp"
    "${_host}/host/controller/alarm_scheduler.cpp"
    "${_host}/host/controller/app_hub.cpp"
    "${_host}/host/controller/app_manager.cpp"
    "${_host}/runtime/abi/network_endpoint.cpp"
    "${_host}/runtime/components.cpp"
    "${_host}/runtime/resources/bitmap_store.cpp"
    "${_host}/platform/graphics/app_surface_compositor.cpp"
    "${_host}/platform/graphics/pixel_compositor.cpp"
    "${_host}/platform/lvgl/fonts/bitmap_font_rasterizer.cpp")
target_sources(app PRIVATE ${_host_port_sources})
sdk_set_main_file("${MPX_HOST_MAIN}")
set_source_files_properties("${MPX_HOST_MAIN}" ${_host_port_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
set_property(SOURCE "${_host}/platform/graphics/app_surface_compositor.cpp"
    "${_host}/platform/graphics/pixel_compositor.cpp" APPEND PROPERTY COMPILE_OPTIONS "${MPX_ORION_RUNTIME_OPT}")
