ENTRY(_start)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1M
    RAM   (rwx) : ORIGIN = 0x40000000, LENGTH = 0xFF000   /* up to BOOT_INFO_ADDR */
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    .data : {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM AT > FLASH
    _data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    .stacks (NOLOAD) : {
        . = ALIGN(8);
        . += 0x1000;  _irq_stack_top = .;
        . += 0x1000;  _und_stack_top = .;
        . += 0x1000;  _abt_stack_top = .;
        . += 0x10000; _svc_stack_top = .;
    } > RAM

    /DISCARD/ : { *(.comment) *(.note*) }
}
