/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../src/ios-jit/husk-ios-jit.h"

static int deny_release;
static kern_return_t checked_deallocate(vm_map_t task, vm_address_t at, vm_size_t n)
{
    if (deny_release) { deny_release--; return KERN_PROTECTION_FAILURE; }
    return vm_deallocate(task, at, n);
}
#define vm_deallocate checked_deallocate
#include "../../src/ios-jit/husk-ios-jit-pool.inc"
#undef vm_deallocate

#define MiB ((size_t)1 << 20)
static size_t prefix;
static HuskDualMapping original, candidate;
static void alias_prefix(HuskDualMapping *m)
{
    vm_address_t a = 0;
    vm_prot_t cur, max;
    assert(vm_remap(mach_task_self(), &a, prefix, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), (vm_address_t)m->rx_addr, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(vm_protect(mach_task_self(), a, prefix, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS);
    m->rw_addr = (void *)a; m->size = prefix;
    candidate = *m;
}

static bool mapped(void *p)
{
    vm_address_t a = (vm_address_t)p; vm_size_t n = 0;
    natural_t depth = 0; vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    return vm_region_recurse_64(mach_task_self(), &a, &n, &depth,
        (vm_region_recurse_info_t)&info, &count) == KERN_SUCCESS && a <= (vm_address_t)p;
}

static void setup(void)
{
    assert(!husk_prewarmed.rw_addr);
    vm_address_t rx = 0, rw = 0; vm_prot_t cur, max;
    assert(vm_allocate(mach_task_self(), &rx, 512 * MiB, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(vm_remap(mach_task_self(), &rw, 512 * MiB, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), rx, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(vm_protect(mach_task_self(), rx, 512 * MiB, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS);
    original = husk_prewarmed = (HuskDualMapping){(void *)rw, (void *)rx, 512 * MiB};
    husk_prewarm_done = true;
    husk_mapping_published = false; husk_runtime_high = husk_qemu_top = 0;
    prefix = 288 * MiB; candidate = (HuskDualMapping){0};
    /* Live code in the RX prefix must survive deleting either other view. */
    const uint32_t code[] = {0x52800540, 0xd65f03c0}; /* ARM64: return 42 */
    memcpy(original.rw_addr, code, sizeof(code));
    original.rw_addr[prefix - 1] = 0x5a;
    original.rw_addr[original.size - 1] = 0xa5;
    sys_icache_invalidate(original.rx_addr, sizeof(code));
}

static void cleanup(void)
{
    if (candidate.rw_addr) vm_deallocate(mach_task_self(), (vm_address_t)candidate.rw_addr, candidate.size);
    vm_deallocate(mach_task_self(), (vm_address_t)original.rw_addr, original.size);
    vm_deallocate(mach_task_self(), (vm_address_t)original.rx_addr, original.size);
    husk_prewarmed = (HuskDualMapping){0}; husk_prewarm_done = false;
}

static void assert_code(HuskDualMapping m)
{
    assert(m.rx_addr[prefix - 1] == 0x5a);
    m.rw_addr[prefix - 2] = 0x17;
    assert(m.rx_addr[prefix - 2] == 0x17);
#if defined(__aarch64__)
    assert(((int (*)(void))m.rx_addr)() == 42);
#endif
}

static void bad_alias(HuskDualMapping *m) { m->rw_addr = m->rx_addr; }

/* Competing publication waits until the callback/handoff completes. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered;
static void gated_alias(HuskDualMapping *m)
{
    pthread_mutex_lock(&gate); entered = true; pthread_cond_signal(&condition); pthread_mutex_unlock(&gate);
    alias_prefix(m);
}
static void *native_thread(void *unused)
{
    (void)unused; HuskDualMapping out;
    assert(husk_ios_jit_open_native(&out, gated_alias)); return NULL;
}

int main(void)
{
    printf("Real Mach JIT handoff: host page size %d\n", getpagesize());
    HuskDualMapping out;
    assert(!husk_ios_jit_open_native(&out, alias_prefix));
    assert(!husk_ios_jit_open_native(NULL, alias_prefix));
    setup();
    assert(husk_ios_jit_open_native(&out, alias_prefix));
    assert(out.rw_addr == husk_prewarmed.rw_addr && husk_prewarmed.size == 288 * MiB);
    assert(!mapped(original.rw_addr) && !mapped(original.rx_addr + prefix));
    assert_code(out);
    /* Prove the freed high allocation range can hold ordinary RW data. Fixed
     * mapping has no OVERWRITE: a stray live mapping would fail this test. */
    vm_address_t reclaimed = (vm_address_t)original.rw_addr;
    assert(vm_allocate(mach_task_self(), &reclaimed, 512 * MiB, VM_FLAGS_FIXED) == KERN_SUCCESS);
    ((char *)reclaimed)[0] = 11; ((char *)reclaimed)[512 * MiB - 1] = 19;
    assert_code(out);
    assert(husk_ios_jit_reserve_bottom(100 * MiB));
    HuskDualMapping qemu = husk_jit_take_top(128 * MiB);
    assert(qemu.rw_addr == out.rw_addr + 160 * MiB);
    qemu.rw_addr[0] = 73; assert(qemu.rx_addr[0] == 73);
    assert(!husk_ios_jit_reserve_bottom(161 * MiB));
    assert(!husk_jit_take_top(256 * MiB).rw_addr);
    assert(!husk_jit_take_top(SIZE_MAX).rw_addr && !husk_jit_take_top(0).rw_addr);
    cleanup();
    puts("PASS: 512 MiB old RW plus 224 MiB unused RX reclaimed; 288 MiB executable prefix, ordinary heap reuse and bounded QEMU sharing survive");

    for (int mode = 0; mode < 4; mode++) {
        setup();
        if (mode == 0) assert(husk_ios_jit_get_mapping() == &husk_prewarmed);
        if (mode == 1) assert(husk_ios_jit_reserve_bottom(16384));
        if (mode == 2) assert(husk_jit_take_top(128 * MiB).rw_addr);
        if (mode == 3) deny_release = 1;
        assert(husk_ios_jit_open_native(&out, alias_prefix));
        assert(husk_prewarmed.rw_addr == original.rw_addr && husk_prewarmed.size == 512 * MiB);
        assert(mapped(original.rw_addr) && mapped(original.rx_addr + prefix));
        assert(original.rx_addr[original.size - 1] == 0xa5);
        assert_code(out); cleanup();
    }
    setup(); assert(husk_ios_jit_open_native(&out, bad_alias));
    assert(out.rw_addr == original.rw_addr && mapped(original.rw_addr)); cleanup();
    puts("PASS: exported descriptors, native claims, active QEMU, failed deallocation and invalid alias never lose existing mappings");

    setup(); prefix = original.size;
    assert(husk_ios_jit_open_native(&out, alias_prefix));
    assert(husk_prewarmed.size == 512 * MiB && !mapped(original.rw_addr)); cleanup();
    setup(); pthread_t thread;
    assert(!pthread_create(&thread, NULL, native_thread, NULL));
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    HuskDualMapping *shared = husk_ios_jit_get_mapping();
    assert(!pthread_join(thread, NULL));
    assert(shared->rw_addr == candidate.rw_addr && shared->size == prefix);
    assert_code(*shared); cleanup();
    puts("PASS: full-size relocation and concurrent descriptor publication retain consistent addresses/capacity");
}
