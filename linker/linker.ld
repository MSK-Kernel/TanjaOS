ENTRY(_start)

SECTIONS
{
    /* Kernel loads at 1MB (standard for GRUB + Multiboot) */
    . = 1M;

    /* ================= MULTIBOOT + CODE ================= */
    .text ALIGN(4K) :
    {
        KEEP(*(.multiboot))   /* MUST stay first */
        *(.text*)
    }

    /* ================= READ-ONLY DATA ================= */
    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    /* ================= INITIALIZED DATA ================= */
    .data ALIGN(4K) :
    {
        *(.data*)
    }

    /* ================= UNINITIALIZED DATA ================= */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }
}ENTRY(_start)

SECTIONS
{
    /* Kernel loads at 1MB (standard for GRUB + Multiboot) */
    . = 1M;

    /* ================= MULTIBOOT + CODE ================= */
    .text ALIGN(4K) :
    {
        KEEP(*(.multiboot))   /* MUST stay first */
        *(.text*)
    }

    /* ================= READ-ONLY DATA ================= */
    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    /* ================= INITIALIZED DATA ================= */
    .data ALIGN(4K) :
    {
        *(.data*)
    }

    /* ================= UNINITIALIZED DATA ================= */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }
}
