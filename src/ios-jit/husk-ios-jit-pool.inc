/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared by the iOS implementation and the real-Mach host regression. All
 * mapping publication and bottom/top claims use this lock. A raw descriptor
 * returned to an older consumer pins its addresses for the process lifetime. */
static HuskDualMapping husk_prewarmed;
static bool husk_prewarm_done;
static pthread_mutex_t husk_share_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t husk_runtime_high;
static size_t husk_qemu_top;
static bool husk_mapping_published;

HUSK_EXPORT HuskDualMapping *husk_ios_jit_get_mapping(void)
{
    pthread_mutex_lock(&husk_share_lock);
    HuskDualMapping *m = husk_prewarmed.rw_addr ? &husk_prewarmed : NULL;
    if (m) husk_mapping_published = true;
    pthread_mutex_unlock(&husk_share_lock);
    return m;
}

static bool husk_mapping_disjoint(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{
    return a <= UINTPTR_MAX - an && b <= UINTPTR_MAX - bn
        && (a + an <= b || b + bn <= a);
}

/* Only the native loader's synchronous alias-creation callback runs here.
 * It must not call another pool API. It creates an alias of the existing RX
 * pages; this function never grants new executable memory or changes TXM. */
HUSK_EXPORT bool husk_ios_jit_open_native(HuskDualMapping *out,
                                         void (*relocate)(HuskDualMapping *))
{
    if (!out) return false;
    pthread_mutex_lock(&husk_share_lock);
    HuskDualMapping old = husk_prewarmed, next = old;
    if (!old.rw_addr) {
        *out = next;
        pthread_mutex_unlock(&husk_share_lock);
        return false;
    }
    if (relocate) relocate(&next);
    if (next.rw_addr != old.rw_addr) {
        bool valid = next.rw_addr && next.rx_addr == old.rx_addr && next.size
            && next.size <= old.size
            && !(((uintptr_t)next.rw_addr | (uintptr_t)next.rx_addr | next.size | old.size) & 0x3fff)
            && husk_mapping_disjoint((uintptr_t)next.rw_addr, next.size, (uintptr_t)old.rx_addr, old.size)
            && husk_mapping_disjoint((uintptr_t)next.rw_addr, next.size, (uintptr_t)old.rw_addr, old.size)
            && husk_mapping_disjoint((uintptr_t)old.rw_addr, old.size, (uintptr_t)old.rx_addr, old.size);
        if (!valid) {
            fprintf(stderr, "[husk-jit] rejected invalid native alias handoff\n");
            next = old;
        } else if (husk_mapping_published || husk_runtime_high || husk_qemu_top) {
            /* Keep the private near alias, but leave every existing reader's
             * original mapping/capacity intact. Bottom reservations still
             * prevent the native loader from overlapping QEMU's top. */
            fprintf(stderr, "[husk-jit] retained original writable alias: shared mapping already in use\n");
        } else {
            kern_return_t kr = vm_deallocate(mach_task_self(), (vm_address_t)old.rw_addr, old.size);
            if (kr == KERN_SUCCESS) {
                husk_prewarmed = next;
                size_t released = old.size;
                if (next.size < old.size) {
                    kr = vm_deallocate(mach_task_self(), (vm_address_t)(old.rx_addr + next.size), old.size - next.size);
                    if (kr == KERN_SUCCESS) released += old.size - next.size;
                    else fprintf(stderr, "[husk-jit] unused RX tail retained: mach_error=%d\n", kr);
                }
                fprintf(stderr, "[husk-jit] reclaimed obsolete JIT mappings: %zu MiB; shared pool %zu MiB, rx=%p rw=%p\n",
                        released >> 20, next.size >> 20, (void *)next.rx_addr, (void *)next.rw_addr);
            } else {
                fprintf(stderr, "[husk-jit] original writable alias retained: deallocation failed (%d)\n", kr);
            }
        }
    }
    husk_mapping_published = true;
    *out = next;
    pthread_mutex_unlock(&husk_share_lock);
    return true;
}

HUSK_EXPORT bool husk_ios_jit_reserve_bottom(size_t end)
{
    pthread_mutex_lock(&husk_share_lock);
    bool ok = husk_prewarmed.rw_addr && end <= husk_prewarmed.size - husk_qemu_top;
    if (ok && end > husk_runtime_high) husk_runtime_high = end;
    if (!ok) fprintf(stderr, "[husk-jit] the native runtime wants %zu MiB of the JIT region, but QEMU holds its top %zu MiB\n",
                     end >> 20, husk_qemu_top >> 20);
    pthread_mutex_unlock(&husk_share_lock);
    return ok;
}

static HuskDualMapping husk_jit_take_top(size_t bytes)
{
    HuskDualMapping top = {0};
    pthread_mutex_lock(&husk_share_lock);
    if (bytes && husk_prewarmed.rw_addr && bytes <= husk_prewarmed.size) {
        size_t off = (husk_prewarmed.size - bytes) & ~(size_t)0x3fff;
        if (husk_runtime_high <= off) {
            husk_qemu_top = husk_prewarmed.size - off;
            husk_mapping_published = true;
            top = (HuskDualMapping){ husk_prewarmed.rw_addr + off, husk_prewarmed.rx_addr + off, husk_qemu_top };
            fprintf(stderr, "[husk-jit] using the top %zu MiB of the prewarmed region (%zu MiB)\n",
                    husk_qemu_top >> 20, husk_prewarmed.size >> 20);
        }
    }
    pthread_mutex_unlock(&husk_share_lock);
    return top;
}
