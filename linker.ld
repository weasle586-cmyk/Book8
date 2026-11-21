OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    .multiboot ALIGN(8) : { KEEP(*(.multiboot)) }
    .text ALIGN(4K) : { *(.text*) }
    .ap_trampoline ALIGN(16) : {
        KEEP(*(.ap_trampoline))
    }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    /* Ring 3 pages: each section owns whole 4K pages so paging can flip U/S
     * on them without exposing neighbouring kernel data. */
    .user_text ALIGN(4K) : {
        __user_text_start = .;
        KEEP(*(.user.text))
        . = ALIGN(4K);
        __user_text_end = .;
    }
    .user_data ALIGN(4K) : {
        __user_data_start = .;
        KEEP(*(.user.data))
        . = ALIGN(4K);
        __user_data_end = .;
    }
    .user_stack ALIGN(4K) : {
        __user_stack_start = .;
        KEEP(*(.user.stack))
        . = ALIGN(4K);
        __user_stack_end = .;
    }
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
        . = ALIGN(4K);
        __kernel_end = .;
    }
}
