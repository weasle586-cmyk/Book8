/* Linker script for BOOK8 user programs: everything lives in the 1 GiB user
 * window at 0x8000000000, with code and data on separate pages. */
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);   /* R+X */
    data PT_LOAD FLAGS(6);   /* R+W */
}
SECTIONS
{
    . = 0x8000001000;
    .text : { *(.text*) *(.user.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) *(.user.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
