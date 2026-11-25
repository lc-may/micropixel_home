# SPDX-License-Identifier: Apache-2.0
include("${MPX_ROOT}/cmake/guest_runtime.cmake")
include("${MPX_ROOT}/cmake/guest_audio.cmake")
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_guest_options --profile "${MPX_ORION_GUEST_PROFILE}" --stack-size 32768)
set(_guest_options_file "${_generated}/guest-build-options.txt")
file(GENERATE OUTPUT "${_guest_options_file}" CONTENT "${_guest_options}\n")
foreach(_name IN LISTS MPX_HOST_GUESTS)
    set(_source "${_repo}/guest/tests/conformance/${_name}.cpp")
    set(_header "${_generated}/${_name}.h")
    add_custom_command(OUTPUT "${_header}"
        COMMAND python3 "${_repo}/tools/pc.py" "${_source}" ${_guest_options} --output-dir "${_generated}/${_name}"
        COMMAND python3 "${MPX_ROOT}/tools/embed_wasm.py" "${_generated}/${_name}/${_name}.wasm" "${_header}"
        DEPENDS "${_source}" ${_guest_sdk} "${_guest_options_file}" "${_repo}/tools/pc.py" "${_repo}/tools/micropixel"
            "${_repo}/tools/build_app_bundle.py" "${MPX_ROOT}/tools/embed_wasm.py"
        VERBATIM)
    target_sources(app PRIVATE "${_header}")
endforeach()
foreach(_name IN LISTS MPX_HOST_APP_GUESTS)
    set(_source "${_repo}/guest/apps/${_name}")
    set(_header "${_generated}/mpx_guest_${_name}.h")
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${_source}/*.cpp" "${_source}/*.hpp" "${_source}/app.json")
    add_custom_command(OUTPUT "${_header}"
        COMMAND python3 "${_repo}/tools/pc.py" "${_source}" ${_guest_options} --output-dir "${_generated}/${_name}"
        COMMAND python3 "${MPX_ROOT}/tools/embed_wasm.py" "${_generated}/${_name}/${_name}.wasm" "${_header}"
        DEPENDS ${_sources} ${_guest_sdk} "${_guest_options_file}" "${_repo}/tools/pc.py" "${_repo}/tools/micropixel"
            "${_repo}/tools/build_app_bundle.py" "${MPX_ROOT}/tools/embed_wasm.py"
        VERBATIM)
    target_sources(app PRIVATE "${_header}")
endforeach()

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
