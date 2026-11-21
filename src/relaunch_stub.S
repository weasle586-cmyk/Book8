/* In-RAM kernel relaunch. See kernel/relaunch.h for the plan layout; the
 * struct book8_relaunch_plan field offsets below must match it. */
.set PLAN_CR3, 0
.set PLAN_ENTRY, 8
.set PLAN_MBI_SRC, 16
.set PLAN_MBI_DST, 24
.set PLAN_MBI_LEN, 32
.set PLAN_SEG_COUNT, 40
.set PLAN_STACK, 48
.set PLAN_GDTR, 94
.set PLAN_SEGMENTS, 104
.set PLAN_OFFSET, 0x3000
.set STUB_OFFSET, 0x3400

.section .text
.code64
/* void book8_relaunch_enter(uint8_t *plan, uint64_t plan_physical)
 * Interrupts must already be masked and bus-master DMA quiesced. The running
 * kernel's mapping must identity-map plan_physical (low 4 GiB identity). */
.global book8_relaunch_enter
.type book8_relaunch_enter, @function
book8_relaunch_enter:
    cli
    cld
    mov %rsi, %r12
    lea STUB_OFFSET(%rdi), %rdi
    lea book8_relaunch_stub_start(%rip), %rsi
    lea book8_relaunch_stub_end(%rip), %rcx
    sub %rsi, %rcx
    rep movsb
    lea PLAN_OFFSET(%r12), %rdi
    lea STUB_OFFSET(%r12), %rax
    jmp *%rax
.size book8_relaunch_enter, . - book8_relaunch_enter

/* Position independent; runs from the plan area with RDI = &plan (physical). */
.global book8_relaunch_stub_start
.global book8_relaunch_stub_end
book8_relaunch_stub_start:
    mov %rdi, %rbx
    mov PLAN_STACK(%rbx), %rsp
    lgdt PLAN_GDTR(%rbx)
    mov PLAN_CR3(%rbx), %rax
    mov %rax, %cr3
    lea 1f(%rip), %rax
    pushq $0x08
    push %rax
    lretq
1:
    mov $0x10, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %eax, %fs
    mov %eax, %gs
    mov PLAN_SEG_COUNT(%rbx), %r8
    lea PLAN_SEGMENTS(%rbx), %r9
2:
    test %r8, %r8
    jz 3f
    mov 0(%r9), %rdi
    mov 8(%r9), %rsi
    mov 16(%r9), %rcx
    rep movsb
    mov 24(%r9), %rcx
    sub 16(%r9), %rcx
    xor %eax, %eax
    rep stosb
    add $32, %r9
    dec %r8
    jmp 2b
3:
    mov PLAN_MBI_DST(%rbx), %rdi
    mov PLAN_MBI_SRC(%rbx), %rsi
    mov PLAN_MBI_LEN(%rbx), %rcx
    rep movsb
    mov PLAN_ENTRY(%rbx), %edi
    mov PLAN_MBI_DST(%rbx), %ebx
    lea 4f(%rip), %rax
    pushq $0x18
    push %rax
    lretq
.code32
4:
    /* Compatibility mode -> legacy protected mode, paging off, as GRUB leaves it.
     * rdmsr/wrmsr use EDX:EAX, so the entry point lives in EDI. */
    mov %cr0, %eax
    and $0x7fffffff, %eax
    mov %eax, %cr0
    mov $0xc0000080, %ecx
    rdmsr
    and $0xfffff6fe, %eax /* clear SCE, LME and NXE; LMA cleared by PG=0 */
    wrmsr
    mov $0x36d76289, %eax
    jmp *%edi
.code64
book8_relaunch_stub_end:
