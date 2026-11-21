#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/hwdiag.h"
#include "../src/syscall.h"
#include "../kernel/exec.h"
#include "../net/net.h"

static uint32_t config[64];
static unsigned config_reads;
static uint32_t registers[256];
static unsigned char *user_buffer;
static size_t user_bytes;
static bool writable;
static bool has_user_space;
static uint8_t frame[BOOK8_NET_FRAME_MAX];
static size_t frame_size;
static unsigned utility_requests;
long book8_gui_utility_request(const struct book8_gui_utility_request *request)
{
    if (request->kind == BOOK8_GUI_CODEEDIT) {
        assert(strcmp(request->path, "A:\\DATA\\TEST.C") == 0);
    } else {
        assert(request->kind == BOOK8_GUI_NVDIAG);
        assert(request->path[0] == '\0');
    }
    ++utility_requests;
    return -BOOK8_EAGAIN;
}

char smoke_pages[64];
__asm__(".globl __user_text_start\n.set __user_text_start, smoke_pages\n"
        ".globl __user_text_end\n.set __user_text_end, smoke_pages + 16\n"
        ".globl __user_data_start\n.set __user_data_start, smoke_pages + 16\n"
        ".globl __user_stack_end\n.set __user_stack_end, smoke_pages + 64\n");

bool exec_user_range_ok(uint64_t ptr, uint64_t len, bool write)
{
    uint64_t lo = (uintptr_t)user_buffer;
    return (!write || writable) && ptr >= lo && ptr - lo <= user_bytes
        && len <= user_bytes - (ptr - lo);
}

bool exec_has_user_space(void) { return has_user_space; }
uint32_t exec_current_pid(void) { return 1; }
long exec_sys_open(uint64_t a, uint64_t b) { (void)a; (void)b; return -BOOK8_ENOSYS; }
long exec_sys_read(uint64_t a, uint64_t b, uint64_t c)
{ (void)a; (void)b; (void)c; return -BOOK8_ENOSYS; }
long exec_sys_close(uint64_t a) { (void)a; return -BOOK8_ENOSYS; }
long exec_sys_execve(struct syscall_frame *f, uint64_t a, uint64_t b)
{ (void)f; (void)a; (void)b; return -BOOK8_ENOSYS; }
long book8_console_write(const char *a, unsigned long b) { (void)a; return (long)b; }
long book8_framebuffer_info(struct book8_fb_info *a) { (void)a; return -BOOK8_ENODEV; }
long book8_framebuffer_draw_pixel(uint64_t a, uint64_t b, uint32_t c)
{ (void)a; (void)b; (void)c; return -BOOK8_ENODEV; }
void syscall_return_to_kernel(long a) { (void)a; abort(); }

static uint32_t pci_read(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset)
{
    (void)ctx;
    assert(bus == 2u && slot == 3u && function == 0u);
    ++config_reads;
    return config[offset / 4u];
}

static long call(uint64_t op, uint64_t address, uint64_t size)
{
    struct syscall_frame frame = {
        .rax = SYS_hwdiag, .rdi = op, .rsi = address, .rdx = size
    };
    syscall_dispatch(&frame);
    return (long)frame.rax;
}
static long utility_call(uint64_t address, uint64_t size)
{
    struct syscall_frame f = {.rax = SYS_gui_utility, .rdi = address, .rsi = size};
    syscall_dispatch(&f);
    return (long)f.rax;
}
static void check_utility_syscall(void)
{
    struct book8_gui_utility_request request = {.kind = BOOK8_GUI_CODEEDIT};
    strcpy(request.path, "A:\\DATA\\TEST.C");
    user_bytes = sizeof(request) + 1u;
    user_buffer = malloc(user_bytes);
    assert(user_buffer);
    has_user_space = true;
    writable = false;
    memcpy(user_buffer + 1u, &request, sizeof(request));
    uint64_t address = (uintptr_t)(user_buffer + 1u);
    assert(utility_call(address, sizeof(request)) == -BOOK8_EAGAIN);
    assert(utility_requests == 1u);
    request.kind = BOOK8_GUI_NVDIAG;
    request.path[0] = '\0';
    memcpy(user_buffer + 1u, &request, sizeof(request));
    assert(utility_call(address, sizeof(request)) == -BOOK8_EAGAIN);
    assert(utility_requests == 2u);
    assert(utility_call(address, sizeof(request) - 1u) == -BOOK8_EINVAL);
    assert(utility_call(0u, sizeof(request)) == -BOOK8_EFAULT);
    assert(utility_call(UINT64_MAX - 1u, sizeof(request)) == -BOOK8_EFAULT);
    assert(utility_call(address + 1u, sizeof(request)) == -BOOK8_EFAULT);
    request.kind = 99u;
    memcpy(user_buffer + 1u, &request, sizeof(request));
    assert(utility_call(address, sizeof(request)) == -BOOK8_EINVAL);
    request.kind = BOOK8_GUI_CODEEDIT;
    memset(request.path, 'x', sizeof(request.path));
    memcpy(user_buffer + 1u, &request, sizeof(request));
    assert(utility_call(address, sizeof(request)) == -BOOK8_EINVAL);
    assert(utility_requests == 2u);
    free(user_buffer);
    user_buffer = NULL; user_bytes = 0u; has_user_space = false;
}

static enum book8_net_result capture_send(void *ctx, const void *data, size_t size)
{
    (void)ctx;
    assert(size <= sizeof(frame));
    memcpy(frame, data, size);
    frame_size = size;
    return BOOK8_NET_OK;
}

static enum book8_net_result empty_receive(void *ctx, void *data, size_t size,
    size_t *received)
{
    (void)ctx; (void)data; (void)size;
    *received = 0u;
    return BOOK8_NET_EMPTY;
}

static void check_wire(void)
{
    static struct book8_net net;
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    struct book8_net_transport transport = {
        .send = capture_send, .receive = empty_receive
    };
    const uint32_t peer = BOOK8_NET_IP(192, 168, 50, 17);
    assert(book8_net_init(&net, &transport, mac) == BOOK8_NET_OK);
    assert(book8_net_configure(&net, BOOK8_NET_IP(192, 168, 50, 208),
        0xffffff00u, 0u) == BOOK8_NET_OK);
    net.arp[0] = (struct book8_arp_entry){.ip = peer, .mac = {2, 0, 0, 0, 0, 2}};
    assert(net_telemetry_init(&net, peer) == BOOK8_NET_OK);
    const struct net_telemetry_metrics metrics = {.cpu_mhz = 3200};
    const struct net_telemetry_extended extended = {.build_id = 12345};
    assert(net_telemetry_send_v2(1000u, &metrics, &extended,
        "local diagnostics disabled") == BOOK8_NET_OK);
    assert(frame_size == 14u + 20u + 8u + 292u);
    assert(frame[34] == 0x1f && frame[35] == 0x90);
    assert(frame[36] == 0x1f && frame[37] == 0x90);
    assert(memcmp(frame + 42u, "BK8\002", 4u) == 0);
    assert(strcmp((const char *)frame + 42u + 164u, "local diagnostics disabled") == 0);
    assert(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
}

int main(int argc, char **argv)
{
    struct nv_gpu gpu = {
        .present = true, .mapped = true, .device_id = NV_DEVICE_GTX970,
        .bus = 2, .slot = 3, .bar0 = (volatile uint8_t *)registers,
        .bar1 = (volatile uint8_t *)registers,
        .bar0_phys = 0xfe000000, .bar0_size = sizeof(registers),
        .bar1_phys = 0xd0000000, .bar1_size = 0x10000000,
        .boot0 = 0x124000a1, .chipset = NV_CHIPSET_GM204,
        .channel_bound = true, .ring_acked = true,
        .scanout.valid = true,
        .pushbuf = (uint32_t *)registers,
        .fence = (volatile uint32_t *)&registers[200u],
        .ce_found = true, .ce_verified = true,
        .ce_engine = 1u, .ce_runlist = 2u,
        .ce_count = 1u, .ptop_devices = 3u,
        .stat_gpu_fills = 21u, .stat_gpu_copies = 13u,
        .stat_small_skips = 77u, .stat_unsupported = 4u,
        .stat_fence_timeouts = 2u,
        .plat = {.pci_read32 = pci_read}
    };
    struct book8_hwdiag_info info;
    struct book8_hwdiag_read32 reg = {NV_PMC_ENABLE, 0xdeadbeef};
    struct book8_hwdiag_report report = {0u, BOOK8_ENODEV};
    struct syscall_frame write_frame = {
        .rax = SYS_write, .rdi = 1u, .rsi = (uintptr_t)smoke_pages, .rdx = 1u
    };
    syscall_dispatch(&write_frame);
    assert((long)write_frame.rax == 1);
    write_frame.rax = SYS_write;
    write_frame.rsi = UINT64_MAX - 1u;
    write_frame.rdx = 1u;
    syscall_dispatch(&write_frame);
    assert((long)write_frame.rax == -BOOK8_EFAULT);
    write_frame.rsi = (uintptr_t)smoke_pages;
    write_frame.rdx = 65537u;
    write_frame.rax = SYS_write;
    syscall_dispatch(&write_frame);
    assert((long)write_frame.rax == -BOOK8_EINVAL);
    write_frame.rdx = 1u;
    has_user_space = true;
    write_frame.rax = SYS_write;
    syscall_dispatch(&write_frame);
    assert((long)write_frame.rax == -BOOK8_EFAULT);
    has_user_space = false;
    hwdiag_init(NULL);
    assert(hwdiag_info(&info) == 0 && info.flags == 0u && info.bar0_phys == 0u);
    assert(hwdiag_read32(&reg) == -BOOK8_ENODEV && reg.value == 0xdeadbeef);
    assert(hwdiag_report(&report) == -BOOK8_ENOSYS);

    hwdiag_init(&gpu);
    assert(hwdiag_info(&info) == 0 && info.flags == 3u);
    assert(info.pci_id == 0x13c210de && info.bdf == 0x218);
    assert(info.bar0_size == sizeof(registers) && info.bar1_phys == 0xd0000000);
    assert(config_reads == 0u);
    config[0] = 0x13c210de;
    config[1] = 2u;
    registers[NV_PMC_ENABLE / 4u] = 0x12345678;
    assert(hwdiag_read32(&reg) == 0 && reg.value == 0x12345678);
    config[1] = 2u | (1u << 20u);
    config[0x34u / 4u] = 0x40u;
    config[0x40u / 4u] = 1u;
    config[0x44u / 4u] = 0u;
    struct book8_nvdiag_snapshot snapshot;
    assert(hwdiag_snapshot(&snapshot) == 0);
    assert((snapshot.flags & (BOOK8_NVDIAG_PRESENT | BOOK8_NVDIAG_MAPPED
        | BOOK8_NVDIAG_PCI_CONFIG | BOOK8_NVDIAG_MMIO_READABLE))
        == (BOOK8_NVDIAG_PRESENT | BOOK8_NVDIAG_MAPPED
            | BOOK8_NVDIAG_PCI_CONFIG | BOOK8_NVDIAG_MMIO_READABLE));
    assert(snapshot.pci_id == 0x13c210deu
        && snapshot.bdf == 0x218u
        && snapshot.bar0_phys == 0xfe000000u);
    assert((snapshot.register_flags & BOOK8_NVDIAG_REG_PMC_ENABLE) != 0u
        && snapshot.pmc_enable == 0x12345678u);
    assert((snapshot.flags & (BOOK8_NVDIAG_CE_FOUND
        | BOOK8_NVDIAG_CE_VERIFIED | BOOK8_NVDIAG_ACCEL_ACTIVE))
        == (BOOK8_NVDIAG_CE_FOUND | BOOK8_NVDIAG_CE_VERIFIED
            | BOOK8_NVDIAG_ACCEL_ACTIVE));
    assert((snapshot.flags & BOOK8_NVDIAG_ACCEL_FAILED) == 0u);
    assert(snapshot.accel_min_pixels == NV_ACCEL_DEFAULT_MIN_PIXELS);
    assert(snapshot.ce_engine == 1u && snapshot.ce_runlist == 2u
        && snapshot.ce_count == 1u && snapshot.ptop_devices == 3u);
    assert(snapshot.accel_fills == 21u && snapshot.accel_copies == 13u
        && snapshot.accel_small_skips == 77u
        && snapshot.accel_unsupported == 4u
        && snapshot.accel_fence_timeouts == 2u);
    gpu.accel_failed = true;
    assert(hwdiag_snapshot(&snapshot) == 0);
    assert((snapshot.flags & BOOK8_NVDIAG_ACCEL_FAILED) != 0u
        && (snapshot.flags & BOOK8_NVDIAG_ACCEL_ACTIVE) == 0u);
    gpu.accel_failed = false;
    config[1] = 2u;
    config[0x34u / 4u] = 0u;
    reg.offset = NV_PMC_BOOT_0;
    registers[0] = 0x124000a1;
    assert(hwdiag_read32(&reg) == 0 && reg.value == registers[0]);
    unsigned reads = config_reads;
    reg.offset = NV_PRAMIN;
    assert(hwdiag_read32(&reg) == -BOOK8_EPERM && config_reads == reads);
    reg.offset = 1u;
    assert(hwdiag_read32(&reg) == -BOOK8_EPERM);
    reg.offset = NV_PMC_ENABLE;
    gpu.bar0_size = NV_PMC_ENABLE + 3u;
    assert(hwdiag_read32(&reg) == -BOOK8_ENODEV);
    gpu.bar0_size = sizeof(registers);
    config[0] = UINT32_MAX;
    assert(hwdiag_read32(&reg) == -BOOK8_ENODEV);
    config[0] = 0x13c210de;
    config[1] = 0u;
    assert(hwdiag_read32(&reg) == -BOOK8_EAGAIN);
    config[1] = 2u | (1u << 20u);
    config[0x34u / 4u] = 0x40;
    config[0x40u / 4u] = 1u;
    config[0x44u / 4u] = 3u;
    reg.value = 0xdeadbeef;
    gpu.bar0 = (volatile uint8_t *)(uintptr_t)0x1000u;
    assert(hwdiag_read32(&reg) == -BOOK8_EAGAIN && reg.value == 0xdeadbeef);
    gpu.bar0 = (volatile uint8_t *)registers;
    config[0x44u / 4u] = 0u;
    assert(hwdiag_read32(&reg) == 0);
    config[0x44u / 4u] = UINT32_MAX;
    assert(hwdiag_read32(&reg) == -BOOK8_ENODEV);
    config[0x40u / 4u] = 0x4002u;
    assert(hwdiag_read32(&reg) == -BOOK8_EIO);
    config[0x34u / 4u] = 0xffu;
    assert(hwdiag_read32(&reg) == -BOOK8_EIO);
    config[0x34u / 4u] = 0u;
    assert(hwdiag_read32(&reg) == -BOOK8_EIO);
    config[1] = 2u;
    registers[NV_PMC_ENABLE / 4u] = UINT32_MAX;
    assert(hwdiag_read32(&reg) == -BOOK8_EIO);
    report = (struct book8_hwdiag_report){0xffffffff, 0u};
    assert(hwdiag_report(&report) == -BOOK8_ENOSYS);
    report.read_errno = BOOK8_EAGAIN;
    assert(hwdiag_report(&report) == -BOOK8_ENOSYS);
    report.pmc_enable = 0u;
    report.read_errno = 0xffffffff;
    assert(hwdiag_report(&report) == -BOOK8_ENOSYS);

    user_buffer = malloc(sizeof(info) + 2u);
    assert(user_buffer != NULL);
    user_bytes = sizeof(info) + 1u;
    writable = true;
    uint64_t address = (uintptr_t)(user_buffer + 1);
    assert(call(BOOK8_HWDIAG_INFO, address, sizeof(info)) == 0);
    memcpy(&info, user_buffer + 1, sizeof(info));
    assert(info.pci_id == 0x13c210de);
    assert(call(99u, address, sizeof(info)) == -BOOK8_EINVAL);
    assert(call(BOOK8_HWDIAG_INFO, address, sizeof(info) - 1u) == -BOOK8_EINVAL);
    assert(call(BOOK8_HWDIAG_INFO, 0u, sizeof(info)) == -BOOK8_EFAULT);
    assert(call(BOOK8_HWDIAG_INFO, UINT64_MAX - 1u, sizeof(info)) == -BOOK8_EFAULT);
    assert(call(BOOK8_HWDIAG_INFO, address + 1u, sizeof(info)) == -BOOK8_EFAULT);
    writable = false;
    assert(call(BOOK8_HWDIAG_INFO, address, sizeof(info)) == -BOOK8_EFAULT);
    assert(call(BOOK8_HWDIAG_READ32, address, sizeof(reg)) == -BOOK8_EFAULT);
    report = (struct book8_hwdiag_report){0u, BOOK8_ENODEV};
    memcpy(user_buffer + 1, &report, sizeof(report));
    assert(call(BOOK8_HWDIAG_REPORT, address, sizeof(report)) == -BOOK8_ENOSYS);
    writable = true;
    reg = (struct book8_hwdiag_read32){NV_PMC_BOOT_0, 0u};
    memcpy(user_buffer + 1, &reg, sizeof(reg));
    assert(call(BOOK8_HWDIAG_READ32, address, sizeof(reg)) == 0);
    memcpy(&reg, user_buffer + 1, sizeof(reg));
    assert(reg.value == 0x124000a1);
    free(user_buffer);
    check_utility_syscall();
    check_wire();
    if (argc == 2 && strcmp(argv[1], "--wire") == 0) {
        assert(fwrite(frame + 42u, 1u, 292u, stdout) == 292u);
    } else {
        assert(argc == 1);
        puts("PASS: hwdiag allowlist, power guard, syscall buffers and v2 UDP 8080");
    }
    return 0;
}
