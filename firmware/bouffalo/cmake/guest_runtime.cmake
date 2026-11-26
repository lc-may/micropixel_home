# SPDX-License-Identifier: Apache-2.0
get_filename_component(_repo "${MPX_ROOT}/../.." ABSOLUTE)
sdk_add_include_directories("${MPX_SHARED_HOST}" "${_repo}/guest"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
sdk_add_compile_definitions(-DMICROPIXEL_BOUFFALO=1)
set(_guest_runtime_sources
    "${MPX_ROOT}/runtime/wamr/guest_runtime.cpp"
    "${MPX_ROOT}/runtime/abi/timer_endpoint.cpp"
    "${MPX_SHARED_HOST}/runtime/abi/service_registry.cpp"
    "${MPX_SHARED_HOST}/runtime/components.cpp"
    "${MPX_SHARED_HOST}/runtime/event_queue.cpp")
if(CONFIG_XZ)
    list(APPEND _guest_runtime_sources "${MPX_ROOT}/platform/storage/embedded_xz.cpp")
endif()
target_sources(app PRIVATE ${_guest_runtime_sources})
set_source_files_properties(${_guest_runtime_sources}
    PROPERTIES COMPILE_OPTIONS "-Werror=frame-larger-than=1536;-fstack-usage")
file(GLOB_RECURSE _guest_sdk CONFIGURE_DEPENDS "${_repo}/guest/sdk/*.hpp" "${_repo}/guest/runtime/*.cpp"
    "${_repo}/guest/runtime/*.hpp" "${_repo}/guest/abi/*.h")
