# SPDX-License-Identifier: Apache-2.0
# The pinned WAMR sets this flag after loading. Memory data segments must be
# copied during loading before Orion can discard the decompression buffer.
set(_aot_directory "${WAMR_ROOT_DIR}/core/iwasm/aot")
set(_aot_source "${_aot_directory}/aot_loader.c")
set(_aot_orion "${CMAKE_CURRENT_BINARY_DIR}/aot_loader_orion.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_aot_source}")
file(READ "${_aot_source}" _aot_loader)
set(_load_call "    if (!load(buf, size, module, args->wasm_binary_freeable, args->no_resolve,")
string(FIND "${_aot_loader}" "${_load_call}" _load_position)
if(_load_position EQUAL -1 OR NOT "${_aot_source}" IN_LIST WAMR_RUNTIME_LIB_SOURCE)
    message(FATAL_ERROR "The pinned WAMR AOT loader no longer matches the Orion image ownership fix")
endif()
string(REPLACE "${_load_call}"
    "    module->is_binary_freeable = args->wasm_binary_freeable;\n${_load_call}" _aot_loader "${_aot_loader}")
file(WRITE "${_aot_orion}" "${_aot_loader}")
list(REMOVE_ITEM WAMR_RUNTIME_LIB_SOURCE "${_aot_source}")
list(APPEND WAMR_RUNTIME_LIB_SOURCE "${_aot_orion}")
sdk_add_private_include_directories("${_aot_directory}")
