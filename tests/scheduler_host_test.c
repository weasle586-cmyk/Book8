#include "../kernel/exec.h"
#include "../kernel/sched.h"
#include "../kernel/vmm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

void book8_scheduler_test_worker(void) {}
static void kernel_thread_test_entry(void *arg) { (void)arg; }
uint64_t boot_pml4[512] __attribute__((aligned(4096)));
static uint64_t fake_cr3 = 0x1000u;
static unsigned cr3_writes;
static uint32_t test_cpu_id;
uint64_t book8_read_cr3(void) { return fake_cr3; }
void book8_write_cr3(uint64_t value) { fake_cr3 = value; ++cr3_writes; }
uint32_t book8_smp_current_cpu_index(void) { return test_cpu_id; }
void book8_sched_request_reschedule(uint32_t cpu_id) { (void)cpu_id; }
void syscall_select_task(struct exec_task *task) { assert(task != 0); }
void fs_close(file_handle_t *file) { (void)file; }
void vmm_free_pages(void *base, uint32_t count) { (void)base; (void)count; }

int main(void)
{
    struct timer_frame shell = {0}, worker1_saved = {0}, worker2_saved = {0};
    struct timer_frame *worker1, *worker2;
    uint64_t worker1_cr3;

    boot_pml4[0] = 0x123007u;
    boot_pml4[300] = 0x456003u;
    assert(exec_timer_tick(&shell) == &shell);
    assert(cr3_writes == 0u);
    assert(exec_scheduler_test_start());
    assert(!exec_scheduler_test_start());
    assert(exec_timer_tick(&shell) == &shell);
    worker1 = exec_timer_tick(&shell);
    assert(cr3_writes == 1u);
    worker1_cr3 = fake_cr3;
    assert(((uint64_t *)worker1_cr3)[0] == 0x123003u);
    assert(((uint64_t *)worker1_cr3)[300] == boot_pml4[300]);
    assert(worker1 != &shell && worker1->rip == (uint64_t)book8_scheduler_test_worker);
    assert(worker1->cs == 0x08u && worker1->ss == 0x10u);
    assert(worker1->rflags == 0x202u && (worker1->rsp & 15u) == 8u);
    assert(worker1->rdi != 0u && worker1->rsi != 0u);
    *(volatile uint64_t *)worker1->rdi = 1u;
    assert(!exec_scheduler_test_finish());
    assert(exec_timer_tick(&worker1_saved) == &worker1_saved);
    worker2 = exec_timer_tick(&worker1_saved);
    assert(cr3_writes == 2u);
    assert(worker2 != worker1 && worker2->rip == worker1->rip);
    assert(worker2->rdi != worker1->rdi && worker2->rsi != worker1->rsi);
    *(volatile uint64_t *)worker2->rdi = 1u;
    assert(exec_timer_tick(&worker2_saved) == &worker2_saved);
    assert(exec_timer_tick(&worker2_saved) == &shell);
    assert(fake_cr3 == 0x1000u && cr3_writes == 3u);
    assert(exec_timer_tick(&shell) == &shell);
    fake_cr3 = worker1_cr3;
    assert(exec_timer_tick(&shell) == &worker1_saved);
    assert(cr3_writes == 3u);
    assert(exec_timer_tick(&worker1_saved) == &worker1_saved);
    assert(exec_timer_tick(&worker1_saved) == &worker2_saved);
    assert(exec_timer_tick(&worker2_saved) == &worker2_saved);
    assert(exec_timer_tick(&worker2_saved) == &shell);
    assert(exec_scheduler_test_ticks() == 12u);
    assert(exec_scheduler_test_finish());
    assert(exec_timer_tick(&shell) == &shell);
    assert(exec_scheduler_test_start());
    assert(!exec_scheduler_test_finish());
    fake_cr3 = 0x1000u;
    unsigned cr3_writes_before_threads = cr3_writes;
    sched_init(1u);
    assert(sched_spawn("invalid", NULL, NULL) == 0u);
    uint32_t tid = sched_spawn("host-worker", kernel_thread_test_entry,
        (void *)(uintptr_t)0x55u);
    assert(tid != 0u);
    struct sched_thread_info thread_info[SCHED_MAX_THREADS];
    assert(sched_snapshot(thread_info, SCHED_MAX_THREADS) == 2u);
    assert(thread_info[0].state == THREAD_RUNNING);
    assert(thread_info[1].tid == tid && thread_info[1].state == THREAD_READY);
    assert(thread_info[1].cpu_ticks == 0u);
    struct timer_frame kernel_frame = {0};
    assert(sched_timer_tick(&kernel_frame) == &kernel_frame);
    struct timer_frame *thread_frame = sched_timer_tick(&kernel_frame);
    assert(thread_frame != &kernel_frame && thread_frame->rip != 0u);
    assert(thread_frame->cs == 0x08u && thread_frame->rflags == 0x202u);
    assert(cr3_writes == cr3_writes_before_threads);
    fake_cr3 = 0x2000u;
    assert(sched_timer_tick(thread_frame) == thread_frame);
    assert(sched_timer_tick(thread_frame) == &kernel_frame);
    assert(fake_cr3 == 0x1000u
        && cr3_writes == cr3_writes_before_threads + 1u);
    assert(sched_snapshot(thread_info, SCHED_MAX_THREADS) == 2u);
    assert(thread_info[0].state == THREAD_RUNNING);
    assert(thread_info[1].state == THREAD_READY);
    sched_init(4u);
    assert(sched_cpu_online(1u));
    assert(sched_cpu_online(2u));
    assert(sched_cpu_online(3u));
    assert(sched_online_cpu_count() == 4u);
    tid = sched_spawn_on("pinned-worker", kernel_thread_test_entry,
        (void *)(uintptr_t)0x77u, 1ull << 1u);
    assert(tid != 0u);
    struct timer_frame ap_frame = {0};
    assert(sched_timer_tick_cpu(&kernel_frame, 0u, true, false)
        == &kernel_frame);
    assert(sched_timer_tick_cpu(&kernel_frame, 0u, true, false)
        == &kernel_frame);
    assert(sched_timer_tick_cpu(&ap_frame, 1u, false, false) == &ap_frame);
    struct timer_frame *pinned_frame =
        sched_timer_tick_cpu(&ap_frame, 1u, false, false);
    assert(pinned_frame != &ap_frame && pinned_frame->rdi
        == (uint64_t)kernel_thread_test_entry
        && pinned_frame->rsi == 0x77u);
    puts("PASS: scheduler quantum, frame rotation, state guards and cleanup");
    return 0;
}
