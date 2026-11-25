# SPDX-License-Identifier: Apache-2.0
get_filename_component(_repo "${MPX_ROOT}/../.." ABSOLUTE)
set(_host "${_repo}/firmware/espressif/main")
sdk_add_include_directories("${_host}" "${_repo}/guest" "${MPX_ROOT}/common"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
sdk_add_compile_definitions(-DMICROPIXEL_BOUFFALO=1)
set(_guest_runtime_sources
    "${MPX_ROOT}/common/guest_runtime.cpp"
    "${MPX_ROOT}/common/timer_endpoint.cpp"
    "${_host}/runtime/abi/service_registry.cpp"
    "${_host}/runtime/event_queue.cpp")
if(CONFIG_XZ)
    list(APPEND _guest_runtime_sources "${MPX_ROOT}/common/embedded_xz.cpp")
endif()
target_sources(app PRIVATE ${_guest_runtime_sources})
set_source_files_properties(${_guest_runtime_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
file(GLOB_RECURSE _guest_sdk CONFIGURE_DEPENDS "${_repo}/guest/sdk/*.hpp" "${_repo}/guest/runtime/*.cpp"
    "${_repo}/guest/runtime/*.hpp" "${_repo}/guest/abi/*.h")
