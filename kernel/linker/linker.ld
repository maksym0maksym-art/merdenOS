
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .text ALIGN(4K) :
    {
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4K);
    __kernel_end = .;
}
