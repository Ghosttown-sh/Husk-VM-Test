/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-vm.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

void tl_log_line(const char *, ...);

/* Public XNU ABI: osfmk/mach/vm_types.h and mach_vm.defs. Resolve dynamically:
 * older systems and sandboxes may not provide/allow this interface. A recipe
 * describes a FIXED user range, not an entitlement or a larger RAM limit.
 * The kernel validates it against vm.malloc_ranges and rejects overlaps with
 * other delegated ranges. It atomically creates a real PROT_NONE reservation.
 * No VM_FLAGS_OVERWRITE, private task port, GPU mapping or APK patch is used. */
typedef struct {
    uint64_t flags_and_tags; /* flags:48, range_tag:8, vm_tag:8 (little endian) */
    uint64_t start, end;
} range_recipe;
_Static_assert(sizeof(range_recipe) == 24, "Mach range recipe ABI");
typedef kern_return_t (*range_create_fn)(vm_map_t, uint32_t, uint8_t *, uint32_t);
static range_create_fn range_create;
static pthread_once_t range_once = PTHREAD_ONCE_INIT;
static void find_range_create(void) {
    range_create = (range_create_fn)dlsym(RTLD_DEFAULT, "mach_vm_range_create");
}

/* Registration lasts for the process lifetime, even when all its pages have
 * been unmapped. Remember boundaries so freed space is reused and a mapping
 * never crosses two registrations. Metadata is allocated BEFORE registering. */
typedef struct owned_range {
    uintptr_t start, end;
    struct owned_range *next;
} owned_range;
static owned_range *ranges;
static pthread_mutex_t range_lock = PTHREAD_MUTEX_INITIALIZER;
static bool bounds_failure_logged;

static int parse_ranges(const char *s, uintptr_t out[4]) {
    for (int i = 0; i < 4; i++) {
        char *end;
        if (!*s || *s == '-' || *s == '+') return 0;
        errno = 0;
        unsigned long long value = strtoull(s, &end, 16);
        if (end == s || errno || value > UINTPTR_MAX) return 0;
        out[i] = (uintptr_t)value;
        if (*end != (i == 0 || i == 2 ? ':' : i == 1 ? ' ' : '\0')) return 0;
        s = end + 1;
    }
    return out[0] <= out[1] && out[2] <= out[3];
}

static kern_return_t region_at(uintptr_t *at, vm_size_t *span,
                               vm_region_submap_info_data_64_t *info) {
    natural_t depth = 0; /* Never descend into shared-cache/GPU submaps. */
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    vm_address_t address = *at;
    kern_return_t kr = vm_region_recurse_64(mach_task_self(), &address, span,
        &depth, (vm_region_recurse_info_t)info, &count);
    *at = address;
    return kr;
}

static void *reuse_range(const owned_range *r, size_t size, size_t alignment) {
    uintptr_t cursor = r->start;
    while (cursor < r->end && size <= r->end - cursor) {
        uintptr_t next = cursor;
        vm_size_t span = 0;
        vm_region_submap_info_data_64_t info = {0};
        kern_return_t kr = region_at(&next, &span, &info);
        if (kr != KERN_SUCCESS && kr != KERN_INVALID_ADDRESS) break;
        uintptr_t end = kr == KERN_SUCCESS && next < r->end ? next : r->end;
        uintptr_t aligned = cursor <= UINTPTR_MAX - (alignment - 1)
            ? (cursor + alignment - 1) & ~(uintptr_t)(alignment - 1) : UINTPTR_MAX;
        if (end > aligned && size <= end - aligned) {
            vm_address_t at = aligned;
            kr = vm_map(mach_task_self(), &at, size, 0, VM_FLAGS_FIXED,
                MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
            if (kr == KERN_SUCCESS) return (void *)at;
            /* Another mapper won. Do not overwrite it or spin on this gap. */
        }
        if (!span || next > UINTPTR_MAX - span || next + span <= cursor) break;
        cursor = next + span;
    }
    return MAP_FAILED;
}

void *tl_vm_reserve_delegated(size_t size) {
    return tl_vm_reserve_delegated_aligned(size, (size_t)getpagesize());
}

void *tl_vm_reserve_delegated_aligned(size_t size, size_t alignment) {
    size_t mask = (size_t)getpagesize() - 1;
    if (!size || size > SIZE_MAX - mask) { errno = ENOMEM; return MAP_FAILED; }
    if (alignment < mask + 1 || (alignment & (alignment - 1))) { errno = EINVAL; return MAP_FAILED; }
    size = (size + mask) & ~mask;
    pthread_once(&range_once, find_range_create);
    if (!range_create) {
        tl_log_line("mm: VM range delegation unavailable (mach_vm_range_create is absent)");
        errno = ENOMEM; return MAP_FAILED;
    }
    pthread_mutex_lock(&range_lock);
    for (owned_range *r = ranges; r; r = r->next) {
        void *p = reuse_range(r, size, alignment);
        if (p != MAP_FAILED) {
            tl_log_line("mm: reused delegated VM range addr=%p len=%#zx", p, size);
            pthread_mutex_unlock(&range_lock);
            return p;
        }
    }
    char text[128]; size_t text_size = sizeof(text) - 1;
    uintptr_t bounds[4];
    if (sysctlbyname("vm.malloc_ranges", text, &text_size, NULL, 0) || text_size >= sizeof(text)) {
        if (!bounds_failure_logged) {
            int saved_errno = errno;
            /* XNU accepts an empty recipe list only when this process uses
             * user ranges. This probes support without registering anything. */
            kern_return_t probe = range_create(mach_task_self(), 1, NULL, 0);
            tl_log_line("mm: VM range bounds unavailable errno=%d; empty range probe=%d", saved_errno, probe);
            bounds_failure_logged = true;
        }
        goto failed;
    }
    text[text_size] = 0;
    if (!parse_ranges(text, bounds)) goto failed;
    owned_range *node = malloc(sizeof(*node));
    if (!node) goto failed;
    kern_return_t last = KERN_NO_SPACE;
    /* Query each permitted void; inspect its map entries rather than guessing
     * addresses for a device/OS version. Only inaccessible, max-protection-zero
     * padding is proposed. The kernel is the final authority on eligibility.
     * Delegate exactly the failed request, without speculative extra capacity. */
    for (int i = 0; i < 4; i += 2) {
        if (bounds[i] > UINTPTR_MAX - mask) continue;
        uintptr_t cursor = (bounds[i] + mask) & ~mask, limit = bounds[i+1];
        while (cursor < limit && size <= limit - cursor) {
            uintptr_t at = cursor;
            vm_size_t span = 0;
            vm_region_submap_info_data_64_t info = {0};
            if (region_at(&at, &span, &info) != KERN_SUCCESS || !span || at > UINTPTR_MAX - span) break;
            uintptr_t end = at + span;
            if (end > limit) end = limit;
            if (at > cursor) cursor = at;
            if (end <= cursor) break;
            uintptr_t aligned = cursor <= UINTPTR_MAX - (alignment - 1)
                ? (cursor + alignment - 1) & ~(uintptr_t)(alignment - 1) : UINTPTR_MAX;
            if (!info.is_submap && info.protection == VM_PROT_NONE && info.max_protection == VM_PROT_NONE
                && end > aligned && size <= end - aligned) {
                range_recipe recipe = { UINT64_C(2) << 48, aligned, aligned + size };
                last = range_create(mach_task_self(), 1, (uint8_t *)&recipe, sizeof(recipe));
                if (last == KERN_SUCCESS) {
                    node->start = aligned; node->end = aligned + size; node->next = ranges; ranges = node;
                    tl_log_line("mm: delegated VM range addr=%p len=%#zx (kernel-approved PROT_NONE reservation)",
                        (void *)aligned, size);
                    pthread_mutex_unlock(&range_lock);
                    return (void *)aligned;
                }
                /* Permissions/unsupported ABI cannot be repaired by another
                 * address. A racing registration can be retried next call. */
                if (last != KERN_MEMORY_PRESENT && last != KERN_INVALID_ARGUMENT) break;
            }
            cursor = end;
        }
    }
    tl_log_line("mm: VM range delegation refused len=%#zx mach_error=%d permitted=%s", size, last, text);
    free(node);
failed:
    pthread_mutex_unlock(&range_lock);
    errno = ENOMEM;
    return MAP_FAILED;
}
