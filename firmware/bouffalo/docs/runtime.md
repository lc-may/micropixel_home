# Bouffalo FreeRTOS/WAMR foundation

The board builds compose the same platform port with the SDK's FreeRTOS
kernel. BL616CL reserves its 4 MiB PSRAM region. The BL618DG profiles use the
AP core with NP disabled and 16 MiB PSRAM. Orion uses B1 silicon and selects
`CPU_MODEL=b1`. The vendor startup, linker and
Picolibc TLS support remain responsible for board initialization.
On Orion, the SDK's SYSINIT initializes the BSP and starts FreeRTOS before
calling the smoke executable's `main()`.

## WAMR execution contract

- The manifest pins the WAMR archive for AOT format 6.
- The AOT builds compile WAMR as RV32F ILP32F on both chips, including DG, whose
  SDK otherwise selects RV32D. Source-level compiler options follow the SDK
  flags; a platform assertion requires `__riscv_flen == 32`. This retains the
  soft-double relocation helpers (`__adddf3`, `__divdf3`, etc.) used by RV32F
  Guests. The remaining SDK code may use the chip's D instructions.
- The [Orion Host build](orion.zh-CN.md#pc-host-移植) uses the SDK's RV32D
  ILP32D ABI for both WAMR and its AOT catalog. `ORION_AOT=0` embeds portable
  Wasm and enables the fast interpreter with an instruction budget.
- JIT, WASI, libc builtins, pthreads, thread manager, shared memory and SIMD
  are disabled in both builds. This port does not
  implement WAMR thread creation, condition waits or native WASI descriptors.
- Bulk memory, reference types, custom names, memory profiling and stack dumps
  follow the Host configuration. `WASM_LINEAR_MEMORY_RESERVE_MAX=1` enables
  the reservation capability; the runtime setting defaults to **false**. An
  embedder that retains native Guest pointers must call
  `wasm_runtime_set_linear_memory_reserve_max(true)` **before instantiation**.
  That process-global setting and instantiation must be serialized with all
  other instance creation; save and restore the previous setting. Only instances
  created with maximum reservation promise a stable base during `memory.grow`.
  Other mappings may relocate on growth. The smoke executable demonstrates this
  ownership in its single WAMR task; a future Host composition must enforce it.
- WAMR allocations and 64-byte-aligned, zero-filled anonymous mappings use
  PSRAM. Reallocation preserves the old block on allocation failure; in-place
  linear-memory growth zeroes newly exposed bytes. Fixed mappings are rejected.
- There is no per-mapping MMU protection. `os_mprotect` is a compatibility hook;
  hardware bounds checks are disabled and WAMR software bounds checks remain
  enabled. This does not provide hardware process isolation.
- Executable AOT writes clean the data cache before `fence.i`. Boot time is the
  SDK microsecond timer. The CPU-time hook reports elapsed time, not measured
  per-task CPU consumption.
- Recursive mutexes use native FreeRTOS semaphores, including WAMR's static
  mutex used before platform initialization. Stack boundaries come from the
  current task's native stack base. All hooks run in task context.

## Native Host APIs

The Bouffalo port uses the SDK machine timer, memory manager, logger and
FreeRTOS task APIs. Both Orion Host and the audio validation build use Host
sources in `firmware/common/`. Bouffalo supplies its SDK adapters under
`platform/include/`. Orion audio reports native xcodec status codes.

`work/task_stack.h` converts byte-sized stack requests to native
stack words before calling `xTaskCreate`. It rounds up without addition
overflow and rejects zero or out-of-range depths. Stack high-water values
are reported in bytes.

## Validation executable

Build the AOT compiler from the prepared WAMR revision with LLVM 18 development
libraries, `clang-18` and `wasm-ld-18` installed (for example, Debian's
`llvm-18-dev`, `clang-18` and `lld-18` packages):

```sh
cmake -S firmware/bouffalo/third_party/wamr/wamr-compiler \
  -B firmware/bouffalo/third_party/.cache/wamrc \
  -DLLVM_DIR="$(llvm-config-18 --cmakedir)" \
  -DWAMR_BUILD_WITH_CUSTOM_LLVM=1 -DWAMR_BUILD_SIMD=0 \
  -DWAMR_BUILD_LIBC_BUILTIN=0 -DWAMR_BUILD_LIBC_WASI=0 \
  -DWAMR_BUILD_LIB_PTHREAD=0 -DWAMR_BUILD_LIB_WASI_THREADS=0
cmake --build firmware/bouffalo/third_party/.cache/wamrc --parallel 8
export PATH="$PWD/firmware/bouffalo/third_party/.cache/wamrc:$PATH"
```

Then use [the platform build commands](../README.md). CMake generates an AOT v6
module from `examples/runtime_guest.c` in each ignored build directory. The
compiler uses `riscv32`, `ilp32f`, `generic-rv32`,
`+m,+a,+f,+c,+zicsr,+zifencei` and software bounds checks. Generation rejects an
unexpected AOT version or absent `__adddf3`/`__divdf3` symbol references. No
Wasm, AOT, generated C arrays or toolchain binaries belong in the source tree.

On a prepared board, `examples/runtime_smoke.c` checks aligned PSRAM allocation
and reallocation contents, recursive/lazy mutexes, stack-base lookup,
anonymous zero-filled mappings, invalid-module rejection and WAMR lifecycle.
It then loads and instantiates the valid AOT module and actually executes
`1.25 + 2.5 == 3.75` and `7.5 / 2.5 == 3.0` through exported f64 functions.
The explicitly enabled maximum-reservation instance grows from one to three
64 KiB pages, checking a stable base, preserved bytes, zero new pages, and
rejection beyond its declared maximum without damaging the allocation.
After teardown and reinitialization it prints
`MicroPixel FreeRTOS/WAMR runtime smoke PASS` only if all checks succeeded.
A compile/link result alone is not a hardware pass.

`CONFIG_SKIP_COMBINE` leaves raw firmware. Before flashing a prepared board,
process a **copy** of its raw image with the SDK's `bflb_fw_post_proc` tool
(`--chipname=<chip> --imgfile=<copy> --appkeys=shared`). Preserve the board's
bootloader, partition table and app storage; the smoke target does not define
an installation layout. Identify the chip and back up the sectors being
replaced before using the SDK flash utility. The console uses the board SDK's
configured UART baud rate. Restore the previous firmware after validation.

The default runtime target verifies the FreeRTOS/WAMR platform over UART.
The optional [Orion display build](orion.zh-CN.md#屏幕) adds a test pattern for
panel bring-up. The Orion Host integrates the embedded app catalog, Guest Scene
graphics, touch and text input, audio, and selected app data in LittleFS.
