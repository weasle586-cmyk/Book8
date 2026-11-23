#include "sched.h"

#include "exec.h"
#include "smp.h"
#include <stddef.h>

#define PIT_INPUT_HZ 1193182ull
#define PIT_DIVISOR 65536ull
#define SCHED_QUANTUM_TICKS 2u

struct sched_queue_slot {
    volatile uint32_t sequence;
    struct tcb *thread;
};

struct sched_run_queue {
    volatile uint32_t enqueue_position;
    volatile uint32_t dequeue_position;
    volatile uint32_t count;
    struct sched_queue_slot slots[SCHED_RUNQ_CAPACITY];
};

struct sched_cpu {
    struct sched_run_queue run_queue;
    struct tcb *current;
    uint32_t quantum_ticks;
    bool online;
};

extern uint64_t book8_read_cr3(void);
extern void book8_write_cr3(uint64_t value);
extern uint32_t book8_smp_current_cpu_index(void);
extern void book8_sched_request_reschedule(uint32_t cpu_id);
/* Strong version lives in src/syscall.c; hosted scheduler tests use this. */
__attribute__((weak)) void syscall_select_kernel_stack(uint32_t cpu_id,
    uint64_t rsp)
{
    (void)cpu_id;
    (void)rsp;
}

static struct tcb threads[SCHED_MAX_THREADS];
static struct sched_cpu cpus[BOOK8_SMP_MAX_CPUS];
static uint32_t next_tid = 1u;
static volatile uint32_t scheduler_lock;
static volatile uint64_t scheduler_ticks;
static volatile uint64_t online_mask = 1u;
static volatile uint32_t online_count = 1u;
static uint32_t cpu_capacity = 1u;
static bool scheduler_ready;
static uint64_t kernel_cr3;

_Static_assert((SCHED_RUNQ_CAPACITY & (SCHED_RUNQ_CAPACITY - 1u)) == 0u,
    "run-queue capacity must be a power of two");
_Static_assert(SCHED_MAX_THREADS > BOOK8_SMP_MAX_CPUS,
    "thread pool must leave space beyond per-CPU idle threads");

static uint64_t scheduler_lock_enter(void)
{
    uint64_t flags = sched_irq_save();
    while (__atomic_exchange_n(&scheduler_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void scheduler_lock_leave(uint64_t flags)
{
    __atomic_store_n(&scheduler_lock, 0u, __ATOMIC_RELEASE);
    sched_irq_restore(flags);
}

static void copy_name(char *destination, const char *source)
{
    uint32_t i = 0u;
    if (source == NULL)
        source = "kernel-thread";
    while (i + 1u < SCHED_NAME_MAX && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static void thread_trampoline(void (*entry)(void *), void *arg)
{
    entry(arg);
    sched_exit();
}

static void run_queue_init(struct sched_run_queue *queue)
{
    queue->enqueue_position = 0u;
    queue->dequeue_position = 0u;
    queue->count = 0u;
    for (uint32_t i = 0u; i < SCHED_RUNQ_CAPACITY; ++i) {
        queue->slots[i].sequence = i;
        queue->slots[i].thread = NULL;
    }
}

static bool run_queue_enqueue(struct sched_run_queue *queue,
    struct tcb *thread)
{
    uint32_t position = __atomic_load_n(&queue->enqueue_position,
        __ATOMIC_RELAXED);
    struct sched_queue_slot *slot;
    for (;;) {
        slot = &queue->slots[position & (SCHED_RUNQ_CAPACITY - 1u)];
        uint32_t sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
        int32_t difference = (int32_t)(sequence - position);
        if (difference == 0) {
            if (__atomic_compare_exchange_n(&queue->enqueue_position,
                    &position, position + 1u, false, __ATOMIC_RELAXED,
                    __ATOMIC_RELAXED))
                break;
        } else if (difference < 0) {
            return false;
        } else {
            position = __atomic_load_n(&queue->enqueue_position,
                __ATOMIC_RELAXED);
        }
    }
    slot->thread = thread;
    __atomic_store_n(&slot->sequence, position + 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&queue->count, 1u, __ATOMIC_RELAXED);
    return true;
}

static struct tcb *run_queue_dequeue(struct sched_run_queue *queue)
{
    uint32_t position = __atomic_load_n(&queue->dequeue_position,
        __ATOMIC_RELAXED);
    struct sched_queue_slot *slot;
    for (;;) {
        slot = &queue->slots[position & (SCHED_RUNQ_CAPACITY - 1u)];
        uint32_t sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
        int32_t difference = (int32_t)(sequence - (position + 1u));
        if (difference == 0) {
            if (__atomic_compare_exchange_n(&queue->dequeue_position,
                    &position, position + 1u, false, __ATOMIC_RELAXED,
                    __ATOMIC_RELAXED))
                break;
        } else if (difference < 0) {
            return NULL;
        } else {
            position = __atomic_load_n(&queue->dequeue_position,
                __ATOMIC_RELAXED);
        }
    }
    struct tcb *thread = slot->thread;
    __atomic_store_n(&slot->sequence, position + SCHED_RUNQ_CAPACITY,
        __ATOMIC_RELEASE);
    __atomic_sub_fetch(&queue->count, 1u, __ATOMIC_RELAXED);
    return thread;
}

static bool affinity_allows(uint64_t affinity, uint32_t cpu_id)
{
    return cpu_id < 64u && (affinity & (1ull << cpu_id)) != 0u;
}

static uint32_t choose_cpu(uint64_t affinity)
{
    uint64_t online = __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
    uint32_t selected = UINT32_MAX;
    uint32_t least = UINT32_MAX;
    for (uint32_t i = 0u; i < cpu_capacity; ++i) {
        if ((online & (1ull << i)) == 0u || !affinity_allows(affinity, i))
            continue;
        uint32_t count = __atomic_load_n(&cpus[i].run_queue.count,
            __ATOMIC_RELAXED);
        if (selected == UINT32_MAX || count < least) {
            selected = i;
            least = count;
        }
    }
    return selected;
}

static bool enqueue_thread(struct tcb *thread)
{
    uint32_t target = choose_cpu(thread->affinity_mask);
    if (target == UINT32_MAX)
        return false;
    thread->home_cpu = target;
    if (!run_queue_enqueue(&cpus[target].run_queue, thread))
        return false;
    if (target != book8_smp_current_cpu_index())
        book8_sched_request_reschedule(target);
    return true;
}

static void reap_dead_threads(void)
{
    for (uint32_t i = cpu_capacity; i < SCHED_MAX_THREADS; ++i) {
        struct tcb *thread = &threads[i];
        bool is_current = false;
        for (uint32_t cpu = 0u; cpu < cpu_capacity; ++cpu) {
            if (cpus[cpu].current == thread) {
                is_current = true;
                break;
            }
        }
        if (thread != NULL && !is_current && thread->state == THREAD_DEAD) {
            thread->tid = 0u;
            thread->name[0] = '\0';
            thread->entry = NULL;
            thread->arg = NULL;
            thread->rsp = 0u;
            thread->cr3 = 0u;
            thread->cpu_ticks = 0u;
            thread->wake_tick = 0u;
            thread->affinity_mask = 0u;
            thread->home_cpu = 0u;
            thread->state = THREAD_UNUSED;
        }
    }
}

static struct tcb *take_from_queue(uint32_t owner, uint32_t worker)
{
    struct sched_run_queue *queue = &cpus[owner].run_queue;
    for (uint32_t attempt = 0u; attempt < SCHED_RUNQ_CAPACITY; ++attempt) {
        struct tcb *thread = run_queue_dequeue(queue);
        if (thread == NULL)
            return NULL;
        if (affinity_allows(thread->affinity_mask, worker))
            return thread;
        if (!run_queue_enqueue(queue, thread))
            return NULL;
    }
    return NULL;
}

static struct tcb *take_ready_thread(uint32_t cpu_id)
{
    struct tcb *thread = take_from_queue(cpu_id, cpu_id);
    if (thread != NULL)
        return thread;
    for (uint32_t offset = 1u; offset < cpu_capacity; ++offset) {
        uint32_t victim = (cpu_id + offset) % cpu_capacity;
        if (!cpus[victim].online)
            continue;
        thread = take_from_queue(victim, cpu_id);
        if (thread != NULL)
            return thread;
    }
    return NULL;
}

static void wake_sleepers(void)
{
    uint64_t now = __atomic_load_n(&scheduler_ticks, __ATOMIC_RELAXED);
    for (uint32_t i = cpu_capacity; i < SCHED_MAX_THREADS; ++i) {
        struct tcb *thread = &threads[i];
        if (thread->state == THREAD_BLOCKED && thread->wake_tick <= now) {
            thread->wake_tick = 0u;
            thread->state = THREAD_READY;
            (void)enqueue_thread(thread);
        }
    }
}

void sched_init(uint32_t requested_cpu_capacity)
{
    uint64_t flags = scheduler_lock_enter();
    cpu_capacity = requested_cpu_capacity;
    if (cpu_capacity == 0u)
        cpu_capacity = 1u;
    if (cpu_capacity > BOOK8_SMP_MAX_CPUS)
        cpu_capacity = BOOK8_SMP_MAX_CPUS;
    for (uint32_t i = 0u; i < SCHED_MAX_THREADS; ++i)
        threads[i] = (struct tcb){0};
    for (uint32_t i = 0u; i < BOOK8_SMP_MAX_CPUS; ++i) {
        run_queue_init(&cpus[i].run_queue);
        cpus[i].current = NULL;
        cpus[i].quantum_ticks = 0u;
        cpus[i].online = false;
    }
    next_tid = 1u;
    kernel_cr3 = book8_read_cr3();
    for (uint32_t i = 0u; i < cpu_capacity; ++i) {
        struct tcb *idle = &threads[i];
        idle->tid = next_tid++;
        idle->state = i == 0u ? THREAD_RUNNING : THREAD_UNUSED;
        idle->cr3 = kernel_cr3;
        idle->affinity_mask = 1ull << i;
        idle->home_cpu = i;
        copy_name(idle->name, i == 0u ? "kernel" : "cpu-idle");
        cpus[i].current = i == 0u ? idle : NULL;
        cpus[i].online = i == 0u;
    }
    __atomic_store_n(&scheduler_ticks, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&online_mask, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&online_count, 1u, __ATOMIC_RELEASE);
    scheduler_ready = true;
    scheduler_lock_leave(flags);
}

bool sched_cpu_online(uint32_t cpu_id)
{
    if (!scheduler_ready || cpu_id == 0u || cpu_id >= cpu_capacity)
        return cpu_id == 0u && scheduler_ready;
    uint64_t flags = scheduler_lock_enter();
    if (!cpus[cpu_id].online) {
        struct tcb *idle = &threads[cpu_id];
        idle->state = THREAD_RUNNING;
        idle->cr3 = kernel_cr3;
        cpus[cpu_id].current = idle;
        cpus[cpu_id].online = true;
        __atomic_fetch_or(&online_mask, 1ull << cpu_id, __ATOMIC_RELEASE);
        __atomic_add_fetch(&online_count, 1u, __ATOMIC_RELEASE);
    }
    scheduler_lock_leave(flags);
    return true;
}

uint32_t sched_cpu_capacity(void)
{
    return cpu_capacity;
}

uint32_t sched_online_cpu_count(void)
{
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

uint32_t sched_spawn_on(const char *name, void (*entry)(void *), void *arg,
    uint64_t affinity_mask)
{
    if (!scheduler_ready || entry == NULL)
        return 0u;
    uint64_t flags = scheduler_lock_enter();
    reap_dead_threads();
    uint64_t online = __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
    uint64_t eligible = affinity_mask == 0u ? online : affinity_mask & online;
    uint32_t target = choose_cpu(eligible);
    if (target == UINT32_MAX) {
        scheduler_lock_leave(flags);
        return 0u;
    }

    struct tcb *thread = NULL;
    for (uint32_t i = cpu_capacity; i < SCHED_MAX_THREADS; ++i) {
        if (threads[i].state == THREAD_UNUSED) {
            thread = &threads[i];
            break;
        }
    }
    if (thread == NULL) {
        scheduler_lock_leave(flags);
        return 0u;
    }

    thread->tid = next_tid++;
    if (next_tid == 0u)
        next_tid = 1u;
    copy_name(thread->name, name);
    thread->entry = entry;
    thread->arg = arg;
    thread->cr3 = kernel_cr3;
    thread->cpu_ticks = 0u;
    thread->wake_tick = 0u;
    thread->affinity_mask = eligible;
    thread->home_cpu = target;
    uint64_t top = (uint64_t)&thread->kernel_stack[SCHED_STACK_SIZE];
    struct timer_frame *frame =
        (struct timer_frame *)(uintptr_t)(top - 8u - sizeof(*frame));
    *frame = (struct timer_frame){0};
    frame->rip = (uint64_t)thread_trampoline;
    frame->rdi = (uint64_t)entry;
    frame->rsi = (uint64_t)arg;
    frame->cs = 0x08u;
    frame->rflags = 0x202u;
    frame->rsp = top - 8u;
    frame->ss = 0x10u;
    thread->rsp = (uint64_t)(uintptr_t)frame;
    thread->state = THREAD_READY;
    if (!run_queue_enqueue(&cpus[target].run_queue, thread)) {
        thread->state = THREAD_UNUSED;
        scheduler_lock_leave(flags);
        return 0u;
    }
    uint32_t tid = thread->tid;
    scheduler_lock_leave(flags);
    if (target != book8_smp_current_cpu_index())
        book8_sched_request_reschedule(target);
    return tid;
}

uint32_t sched_spawn(const char *name, void (*entry)(void *), void *arg)
{
    return sched_spawn_on(name, entry, arg, UINT64_MAX);
}

void sched_yield(void)
{
    if (scheduler_ready)
        book8_sched_request_reschedule(book8_smp_current_cpu_index());
}

void sched_sleep_ms(uint32_t ms)
{
    if (!scheduler_ready)
        return;
    if (ms == 0u) {
        sched_yield();
        return;
    }
    uint64_t delay = ((uint64_t)ms * PIT_INPUT_HZ
        + PIT_DIVISOR * 1000u - 1u) / (PIT_DIVISOR * 1000u);
    if (delay == 0u)
        delay = 1u;

    uint32_t cpu_id = book8_smp_current_cpu_index();
    if (cpu_id >= cpu_capacity)
        return;
    uint64_t flags = scheduler_lock_enter();
    struct tcb *thread = cpus[cpu_id].current;
    uint64_t deadline = __atomic_load_n(&scheduler_ticks, __ATOMIC_RELAXED)
        + delay;
    if (thread == &threads[cpu_id]) {
        scheduler_lock_leave(flags);
        while (__atomic_load_n(&scheduler_ticks, __ATOMIC_ACQUIRE) < deadline)
            sched_yield();
        return;
    }
    thread->wake_tick = deadline;
    thread->state = THREAD_BLOCKED;
    scheduler_lock_leave(flags);
    book8_sched_request_reschedule(cpu_id);
}

void sched_exit(void)
{
    uint32_t cpu_id = book8_smp_current_cpu_index();
    uint64_t flags = scheduler_lock_enter();
    if (cpu_id >= cpu_capacity || cpus[cpu_id].current == NULL
        || cpus[cpu_id].current == &threads[cpu_id]) {
        scheduler_lock_leave(flags);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    cpus[cpu_id].current->state = THREAD_DEAD;
    cpus[cpu_id].current->wake_tick = 0u;
    scheduler_lock_leave(flags);
    book8_sched_request_reschedule(cpu_id);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

uint32_t sched_snapshot(struct sched_thread_info *out, uint32_t capacity)
{
    uint64_t flags = scheduler_lock_enter();
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < cpu_capacity; ++i) {
        if (!cpus[i].online)
            continue;
        const struct tcb *thread = &threads[i];
        if (out != NULL && count < capacity) {
            out[count].tid = thread->tid;
            out[count].state = thread->state;
            out[count].cpu_ticks = thread->cpu_ticks;
            copy_name(out[count].name, thread->name);
        }
        ++count;
    }
    for (uint32_t i = cpu_capacity; i < SCHED_MAX_THREADS; ++i) {
        const struct tcb *thread = &threads[i];
        if (thread->state == THREAD_UNUSED)
            continue;
        if (out != NULL && count < capacity) {
            out[count].tid = thread->tid;
            out[count].state = thread->state;
            out[count].cpu_ticks = thread->cpu_ticks;
            copy_name(out[count].name, thread->name);
        }
        ++count;
    }
    scheduler_lock_leave(flags);
    return count;
}

const char *sched_state_name(enum thread_state state)
{
    switch (state) {
    case THREAD_READY: return "READY";
    case THREAD_RUNNING: return "RUNNING";
    case THREAD_BLOCKED: return "BLOCKED";
    case THREAD_DEAD: return "DEAD";
    case THREAD_UNUSED: return "UNUSED";
    }
    return "UNKNOWN";
}

struct timer_frame *sched_timer_tick_cpu(struct timer_frame *frame,
    uint32_t cpu_id, bool time_tick, bool forced)
{
    if (!scheduler_ready || frame == NULL || cpu_id >= cpu_capacity
        || !cpus[cpu_id].online)
        return frame;
    uint64_t flags = scheduler_lock_enter();
    struct sched_cpu *cpu = &cpus[cpu_id];
    struct tcb *previous = cpu->current;
    if (previous == NULL) {
        scheduler_lock_leave(flags);
        return frame;
    }
    if (time_tick) {
        __atomic_add_fetch(&scheduler_ticks, 1u, __ATOMIC_RELEASE);
        wake_sleepers();
    }
    previous->rsp = (uint64_t)(uintptr_t)frame;
    previous->cr3 = book8_read_cr3();
    if (!forced && !time_tick && ++cpu->quantum_ticks < SCHED_QUANTUM_TICKS) {
        scheduler_lock_leave(flags);
        return frame;
    }
    if (!forced && time_tick
        && ++cpu->quantum_ticks < SCHED_QUANTUM_TICKS) {
        scheduler_lock_leave(flags);
        return frame;
    }
    cpu->quantum_ticks = 0u;
    previous->cpu_ticks += time_tick ? 1u : 0u;
    if (previous->state == THREAD_RUNNING && previous != &threads[cpu_id]) {
        previous->state = THREAD_READY;
        if (!enqueue_thread(previous))
            previous->state = THREAD_RUNNING;
    }

    struct tcb *next = take_ready_thread(cpu_id);
    if (next != NULL && next->state != THREAD_READY)
        next = NULL;
    if (next == previous && previous != &threads[cpu_id]) {
        if (run_queue_enqueue(&cpus[cpu_id].run_queue, previous))
            next = &threads[cpu_id];
    }
    if (next == NULL)
        next = &threads[cpu_id];
    uint64_t previous_cr3 = previous->cr3;
    if (previous == &threads[cpu_id] && next != previous)
        previous->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    next->home_cpu = cpu_id;
    cpu->current = next;
    if (next != previous && next->cr3 != previous_cr3)
        book8_write_cr3(next->cr3);
    /* TSS RSP0 follows the incoming task: CPL3->CPL0 entries always land on
     * the current task's kernel stack, on whichever CPU is running it. */
    syscall_select_kernel_stack(cpu_id,
        (uint64_t)&next->kernel_stack[SCHED_STACK_SIZE]);
    struct timer_frame *next_frame =
        next == previous ? frame : (struct timer_frame *)(uintptr_t)next->rsp;
    scheduler_lock_leave(flags);
    return next_frame;
}

struct timer_frame *sched_timer_tick(struct timer_frame *frame)
{
    return sched_timer_tick_cpu(frame, book8_smp_current_cpu_index(), true,
        false);
}
