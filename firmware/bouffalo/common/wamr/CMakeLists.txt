sdk_generate_library()
set(WAMR_ROOT_DIR "${MPX_ROOT}/third_party/wamr")
set(WAMR_BUILD_PLATFORM "bouffalo")
if(MPX_HOST_PORT)
    set(WAMR_BUILD_TARGET "RISCV32_ILP32D")
    set(WAMR_BUILD_INTERP 1)
    set(WAMR_BUILD_FAST_INTERP 1)
    set(WAMR_BUILD_AOT 0)
    sdk_add_compile_definitions(-DWASM_ENABLE_INSTRUCTION_METERING=1)
else()
    set(WAMR_BUILD_TARGET "RISCV32_ILP32F")
    set(WAMR_BUILD_INTERP 0)
    set(WAMR_BUILD_FAST_INTERP 0)
    set(WAMR_BUILD_AOT 1)
endif()
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_THREAD_MGR 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
set(WAMR_BUILD_DUMP_CALL_STACK 1)
set(WAMR_BUILD_MEMORY_PROFILING 1)
set(WAMR_BUILD_CUSTOM_NAME_SECTION 1)
set(SHARED_PLATFORM_CONFIG "${CMAKE_CURRENT_LIST_DIR}/shared_platform.cmake")
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
if(MPX_HOST_PORT)
    include("${CMAKE_CURRENT_LIST_DIR}/native_invoke.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/metering.cmake")
    set_property(SOURCE "${_metered_interp}" APPEND PROPERTY COMPILE_OPTIONS "${MPX_ORION_INTERPRETER_OPT}")
    set_property(SOURCE "${_interp_directory}/wasm_loader.c" APPEND PROPERTY COMPILE_OPTIONS "${MPX_ORION_RUNTIME_OPT}")
endif()
sdk_library_add_sources(${WAMR_RUNTIME_LIB_SOURCE})
sdk_add_include_directories(
    "${CMAKE_CURRENT_LIST_DIR}"
    "${WAMR_ROOT_DIR}/core/iwasm/include"
    "${WAMR_ROOT_DIR}/core/shared/include"
    "${WAMR_ROOT_DIR}/core/shared/utils"
    "${WAMR_ROOT_DIR}/core/shared/platform/include"
)
sdk_add_compile_definitions(-DBUILD_TARGET_${WAMR_BUILD_TARGET}=1 -DWASM_ENABLE_AOT=${WAMR_BUILD_AOT}
    -DWASM_ENABLE_INTERP=${WAMR_BUILD_INTERP} -DWASM_ENABLE_FAST_INTERP=${WAMR_BUILD_FAST_INTERP}
    -DWASM_ENABLE_BULK_MEMORY=1 -DWASM_ENABLE_REF_TYPES=1 -DWASM_ENABLE_LIBC_BUILTIN=0
    -DWASM_ENABLE_LIBC_WASI=0 -DWASM_ENABLE_THREAD_MGR=0 -DWASM_ENABLE_SHARED_MEMORY=0
    -DWASM_ENABLE_SIMD=0 -DWASM_DISABLE_HW_BOUND_CHECK=1 -DWASM_LINEAR_MEMORY_RESERVE_MAX=1
    -DWASM_ENABLE_MODULE_INST_CONTEXT=0 -DWASM_ENABLE_MEMORY_PROFILING=1 -DWASM_ENABLE_DUMP_CALL_STACK=1 -DWASM_ENABLE_CUSTOM_NAME_SECTION=1)
# Match RV32F Guests on both chips. DG's SDK-wide RV32D flags otherwise
# suppress the soft-double relocation symbols needed by ILP32F AOT modules.
# Source options follow the SDK interface options in the compiler command.
# A target-private option would precede (and be overridden by) those flags.
if(WAMR_BUILD_AOT)
    set_property(SOURCE ${WAMR_RUNTIME_LIB_SOURCE} APPEND PROPERTY COMPILE_OPTIONS -march=rv32imafc_zicsr_zifencei)
endif()
sdk_add_private_compile_options(-Wno-error -Wno-format)
