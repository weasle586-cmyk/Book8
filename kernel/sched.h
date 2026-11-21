#ifndef BOOK8_SCHED_H
#define BOOK8_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#define SCHED_MAX_THREADS 128u
#define SCHED_NAME_MAX 32u
#define SCHED_STACK_SIZE 16384u
#define SCHED_RUNQ_CAPACITY 128u

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
};

struct tcb {
    uint32_t tid;
    enum thread_state state;
    char name[SCHED_NAME_MAX];
    void (*entry)(void *);
    void *arg;
    uint64_t rsp;
    uint64_t cr3;
    uint64_t cpu_ticks;
    uint64_t wake_tick;
    uint64_t affinity_mask;
    uint32_t home_cpu;
    uint8_t kernel_stack[SCHED_STACK_SIZE] __attribute__((aligned(16)));
};

struct sched_thread_info {
    uint32_t tid;
    enum thread_state state;
    char name[SCHED_NAME_MAX];
    uint64_t cpu_ticks;
};

void sched_init(uint32_t cpu_capacity);
bool sched_cpu_online(uint32_t cpu_id);
uint32_t sched_cpu_capacity(void);
uint32_t sched_online_cpu_count(void);
uint32_t sched_spawn(const char *name, void (*entry)(void *), void *arg);
uint32_t sched_spawn_on(const char *name, void (*entry)(void *), void *arg,
    uint64_t affinity_mask);
#if defined(BOOK8_HOST_TEST)
#define sched_yield book8_sched_yield
#endif
void sched_yield(void);
void sched_sleep_ms(uint32_t ms);
void sched_exit(void) __attribute__((noreturn));
uint32_t sched_snapshot(struct sched_thread_info *out, uint32_t capacity);
const char *sched_state_name(enum thread_state state);

struct timer_frame;
struct timer_frame *sched_timer_tick(struct timer_frame *frame);
struct timer_frame *sched_timer_tick_cpu(struct timer_frame *frame,
    uint32_t cpu_id, bool time_tick, bool forced);

static inline uint64_t sched_irq_save(void)
{
#if defined(BOOK8_HOST_TEST) \
    || (defined(__STDC_HOSTED__) && __STDC_HOSTED__)
    return 0u;
#else
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}

static inline void sched_irq_restore(uint64_t flags)
{
#if !defined(BOOK8_HOST_TEST) \
    && (!defined(__STDC_HOSTED__) || !__STDC_HOSTED__)
    if ((flags & 0x200u) != 0u)
        __asm__ volatile ("sti" : : : "memory");
#else
    (void)flags;
#endif
}

#endif
