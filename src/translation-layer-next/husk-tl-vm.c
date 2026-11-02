/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-vm.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

void tl_log_line(const char *, ...);

static int vm_info(task_vm_info_data_t *info)
{
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)info, &count) == KERN_SUCCESS
        && count >= TASK_VM_INFO_REV2_COUNT;
}

/* Darwin's untagged anonymous mmap searches only the heap range on systems
 * with VM map ranges. Linux's non-fixed mmap has no such restriction. Walk
 * the actual task map, including inaccessible reservations and submaps, and
 * ask Mach to reserve a whole gap. VM_FLAGS_FIXED WITHOUT OVERWRITE is an
 * atomic no-replace operation: even a concurrent allocator cannot be clobbered.
 * Kernel address-range/protection checks still apply. No guessed addresses,
 * fictitious mappings, precommitted RAM, or process-limit changes are used. */
void *tl_vm_reserve_gap(size_t size)
{
    const vm_size_t mask = (vm_size_t)getpagesize() - 1;
    if (!size || size > SIZE_MAX - mask) { errno = ENOMEM; return MAP_FAILED; }
    vm_size_t bytes = (size + mask) & ~mask;
    task_vm_info_data_t info = {0};
    if (!vm_info(&info)) { errno = ENOMEM; return MAP_FAILED; }
    if (info.min_address > UINTPTR_MAX - mask) { errno = ENOMEM; return MAP_FAILED; }
    vm_address_t cursor = (info.min_address + mask) & ~mask;
    vm_address_t limit = info.max_address;
    unsigned attempts = 0;
    vm_size_t largest_gap = 0;
    kern_return_t last = KERN_NO_SPACE;
    while (cursor < limit && bytes <= limit - cursor) {
        vm_address_t next = cursor;
        vm_size_t span = 0;
        vm_region_basic_info_data_64_t region;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = vm_region_64(mach_task_self(), &next, &span,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&region, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS && kr != KERN_INVALID_ADDRESS) break;
        vm_address_t end = kr == KERN_SUCCESS && next < limit ? next : limit;
        if (end > cursor && end - cursor > largest_gap) largest_gap = end - cursor;
        if (end > cursor && bytes <= end - cursor) {
            vm_address_t at = cursor;
            last = vm_map(mach_task_self(), &at, bytes, 0, VM_FLAGS_FIXED,
                MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
            attempts++;
            if (last == KERN_SUCCESS) {
                tl_log_line("mm: reserved real VM gap addr=%p len=%#zx attempts=%u", (void *)at, size, attempts);
                return (void *)at;
            }
            /* A racing allocation may have split this gap. Re-query it once:
             * if a new entry is there, the next iteration advances past it.
             * Otherwise the kernel refused this gap; proceed to the next one. */
            vm_address_t check = cursor;
            vm_size_t check_span = 0;
            count = VM_REGION_BASIC_INFO_COUNT_64; object = MACH_PORT_NULL;
            kr = vm_region_64(mach_task_self(), &check, &check_span, VM_REGION_BASIC_INFO_64,
                (vm_region_info_t)&region, &count, &object);
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            if (kr == KERN_SUCCESS && check < end && check_span && check <= UINTPTR_MAX - check_span) {
                cursor = check + check_span;
                continue;
            }
        }
        if (!span || next >= limit || next > UINTPTR_MAX - span || next + span <= cursor) break;
        cursor = next + span;
    }
    tl_log_line("mm: no reservable VM gap len=%#zx largest_gap=%#zx attempts=%u mach_error=%d bounds=%#llx..%#llx",
        size, (size_t)largest_gap, attempts, last, (unsigned long long)info.min_address, (unsigned long long)limit);
#ifndef TL_VM_NO_DELEGATED_RANGES
    /* iOS also reserves inaccessible padding between its allocation ranges.
     * It is NOT a free gap: only mach_vm_range_create may delegate it to us. */
    return tl_vm_reserve_delegated(size);
#endif
    errno = ENOMEM;
    return MAP_FAILED;
}

void tl_vm_log_state(const char *reason)
{
    int saved = errno;
    task_vm_info_data_t info = {0};
    if (vm_info(&info)) {
        size_t available = 0;
#if TARGET_OS_IPHONE
        available = os_proc_available_memory();
#endif
        tl_log_line("mm: state[%s] footprint=%llu MiB resident=%llu MiB virtual=%llu MiB available=%zu MiB regions=%d",
            reason, (unsigned long long)(info.phys_footprint >> 20),
            (unsigned long long)(info.resident_size >> 20), (unsigned long long)(info.virtual_size >> 20),
            available >> 20, info.region_count);
    }
    errno = saved;
}

static int read_self(uintptr_t from, void *to, size_t size)
{
    if (!size) return 1;
    if (from > UINTPTR_MAX - size || (uintptr_t)to > UINTPTR_MAX - size) return 0;
    vm_size_t copied = 0;
    return vm_read_overwrite(mach_task_self(), from, size, (vm_address_t)to, &copied) == KERN_SUCCESS
        && copied == size;
}

/* Used by Unity's bundled libunwindstack, including while reporting OOM.
 * ENOSYS makes that version fall back to unsafe memcpy of the address being
 * inspected. Mach reads preserve EFAULT for inaccessible pages instead.
 * Cross-process reads are deliberately denied; no task port is requested. */
long tl_vm_process_readv(pid_t pid, const struct iovec *local, unsigned long nl,
                        const struct iovec *remote, unsigned long nr, unsigned long flags)
{
    enum { MAX_IOV = 1024 };
    if (flags || nl > MAX_IOV || nr > MAX_IOV) return -22; /* EINVAL */
    struct iovec l[MAX_IOV], r[MAX_IOV];
    if (!read_self((uintptr_t)local, l, nl * sizeof(*l))) return -14;
    size_t total_l = 0, total_r = 0;
    for (unsigned long i = 0; i < nl; i++) {
        if (l[i].iov_len > LONG_MAX - total_l) return -22;
        total_l += l[i].iov_len;
    }
    if (!total_l) return 0;
    if (!read_self((uintptr_t)remote, r, nr * sizeof(*r))) return -14;
    for (unsigned long i = 0; i < nr; i++) {
        if (r[i].iov_len > LONG_MAX - total_r) return -22;
        total_r += r[i].iov_len;
    }
    if (!total_l || !total_r) return 0;
    if (pid != getpid()) {
        if (pid <= 0 || (kill(pid,0) < 0 && errno == ESRCH)) return -3;
        return -1; /* EPERM: do not acquire another process's task port */
    }
    unsigned long li = 0, ri = 0;
    size_t lo = 0, ro = 0, copied = 0;
    const size_t mask = (size_t)getpagesize() - 1;
    while (li < nl && ri < nr) {
        if (lo == l[li].iov_len) { li++; lo = 0; continue; }
        if (ro == r[ri].iov_len) { ri++; ro = 0; continue; }
        uintptr_t src = (uintptr_t)r[ri].iov_base, dst = (uintptr_t)l[li].iov_base;
        if (src > UINTPTR_MAX - ro || dst > UINTPTR_MAX - lo) break;
        src += ro; dst += lo;
        size_t n = l[li].iov_len - lo;
        if (n > r[ri].iov_len - ro) n = r[ri].iov_len - ro;
        /* Linux can return a short transfer when a later page is inaccessible. */
        if (n > mask + 1 - (src & mask)) n = mask + 1 - (src & mask);
        if (n > mask + 1 - (dst & mask)) n = mask + 1 - (dst & mask);
        if (!read_self(src, (void *)dst, n)) break;
        copied += n; lo += n; ro += n;
    }
    return copied ? (long)copied : -14; /* EFAULT */
}
