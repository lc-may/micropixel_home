# SPDX-License-Identifier: Apache-2.0
# The pinned fast interpreter caches the count for the whole entry call.
# Observe explicit Host updates after native calls without charging each
# Wasm instruction for a memory access to the execution environment.
set(_interp_directory "${WAMR_ROOT_DIR}/core/iwasm/interpreter")
set(_fast_interp "${_interp_directory}/wasm_interp_fast.c")
set(_metered_interp "${CMAKE_CURRENT_BINARY_DIR}/wasm_interp_fast_metered.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fast_interp}")
file(READ "${_fast_interp}" _interpreter)

set(_count_init [=[
    int instructions_left = -1;
    if (exec_env) {
        instructions_left = exec_env->instructions_to_execute;
    }
]=])
set(_metered_init [=[
    /* The loader also enters this function without an execution environment
       to obtain the opcode table. */
    int configured_instruction_limit = -1;
    if (exec_env) {
        configured_instruction_limit = exec_env->instructions_to_execute;
    }
    int instructions_left = configured_instruction_limit;
]=])
set(_native_return [=[
                wasm_interp_call_func_native(module, exec_env, cur_func,
                                             prev_frame);
            }

#if WASM_ENABLE_TAIL_CALL != 0 || WASM_ENABLE_GC != 0
]=])
set(_metered_return [=[
                wasm_interp_call_func_native(module, exec_env, cur_func,
                                             prev_frame);
            }
#if WASM_ENABLE_INSTRUCTION_METERING != 0
            if (configured_instruction_limit != exec_env->instructions_to_execute) {
                configured_instruction_limit = exec_env->instructions_to_execute;
                instructions_left = configured_instruction_limit;
            }
#endif

#if WASM_ENABLE_TAIL_CALL != 0 || WASM_ENABLE_GC != 0
]=])
string(FIND "${_interpreter}" "${_count_init}" _init_position)
string(FIND "${_interpreter}" "${_native_return}" _return_position)
if(_init_position EQUAL -1 OR _return_position EQUAL -1 OR
    NOT "${_fast_interp}" IN_LIST WAMR_RUNTIME_LIB_SOURCE)
    message(FATAL_ERROR "The pinned WAMR fast interpreter no longer matches the Bouffalo event-wait budget hook")
endif()
string(REPLACE "${_count_init}" "${_metered_init}" _interpreter "${_interpreter}")
string(REPLACE "${_native_return}" "${_metered_return}" _interpreter "${_interpreter}")
file(WRITE "${_metered_interp}" "${_interpreter}")
list(REMOVE_ITEM WAMR_RUNTIME_LIB_SOURCE "${_fast_interp}")
list(APPEND WAMR_RUNTIME_LIB_SOURCE "${_metered_interp}")
sdk_add_private_include_directories("${_interp_directory}")
