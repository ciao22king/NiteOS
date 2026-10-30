/* Higher-half kernel: loaded at 1 MiB physical, mapped at 0xFFFFFFFF80000000.
   The .boot* sections stay at their low (identity-mapped) addresses because the
   32-bit entry stub runs before the higher-half mapping is usable. */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;

    .boot ALIGN(4K) : AT(ADDR(.boot))
    {
        KEEP(*(.multiboot2))
        *(.boot .boot.*)
    }

    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    kernel_end = .;
    kernel_end_phys = . - KERNEL_VMA;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.note.gnu.property) }
}
