OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M }
SECTIONS {
    . = ORIGIN(RAM);
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) *(.sdata .sdata.*) } > RAM
    . = ALIGN(16);
    __global_pointer$ = . + 0x800;
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) } > RAM
    . = ALIGN(16);
    __bss_end = .;
    __heap_start = .;
    __heap_end = 0x83e00000;
    __stack_bottom = 0x83f00000;
    __stack_top = 0x83fff000;
    ASSERT(__heap_start < __heap_end, "RV32 runner exceeds heap region")
}
