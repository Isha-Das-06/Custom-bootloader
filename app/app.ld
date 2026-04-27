ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40100000, LENGTH = 0x700000   /* APP_LOAD_ADDR .. APP_RAM_END */
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > RAM

    .ARM.exidx : { *(.ARM.exidx*) } > RAM

    .data : {
        *(.data*)
        . = ALIGN(4);
    } > RAM

    /* everything above is the signed payload; .bss is zeroed at startup */
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.comment) *(.note*) }
}
