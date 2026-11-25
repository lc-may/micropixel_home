# SPDX-License-Identifier: Apache-2.0
# With no stack arguments, WAMR jumps straight to the native function.
# Keep its fixed frame 16-byte aligned on RV32 as required by the ABI.
set(_native_invoke "${WAMR_ROOT_DIR}/core/iwasm/common/arch/invokeNative_riscv.S")
set(_aligned_invoke "${CMAKE_CURRENT_BINARY_DIR}/invokeNative_riscv_aligned.S")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_native_invoke}")
file(READ "${_native_invoke}" _assembly)
set(_reserve "addi             sp, sp, - 2 * RV_REG_SIZE")
set(_pop "addi             sp, sp, 2 * RV_REG_SIZE")
string(FIND "${_assembly}" "${_reserve}" _reserve_position)
string(FIND "${_assembly}" "${_pop}" _pop_position)
if(_reserve_position EQUAL -1 OR _pop_position EQUAL -1 OR
    NOT "${_native_invoke}" IN_LIST WAMR_RUNTIME_LIB_SOURCE)
    message(FATAL_ERROR "The pinned WAMR RISC-V native call no longer matches the stack alignment fix")
endif()
string(REPLACE "${_reserve}" "addi             sp, sp, -16" _assembly "${_assembly}")
string(REPLACE "${_pop}" "addi             sp, sp, 16" _assembly "${_assembly}")
file(WRITE "${_aligned_invoke}" "${_assembly}")
list(REMOVE_ITEM WAMR_RUNTIME_LIB_SOURCE "${_native_invoke}")
list(APPEND WAMR_RUNTIME_LIB_SOURCE "${_aligned_invoke}")
