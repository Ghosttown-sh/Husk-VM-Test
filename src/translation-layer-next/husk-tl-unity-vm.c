/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-unity-vm.h"
#include "husk-tl-vm.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static tl_unity_page_fn original;

/* Only the verified reservation-only ABI takes this path. All other page
 * states/flags, existing errors, validation failures and kernel refusals run
 * the original function, including its error translation. The APK is untouched.
 * This is a Baselib adaptation, NOT a change to mmap's requested byte count. */
tl_unity_pages tl_unity_vm_allocate(size_t page, size_t count, size_t alignment_pages,
                                   uint32_t state, uint32_t flags, void *error)
{
    uint32_t error_code = 1;
    if (error) memcpy(&error_code, (uint8_t *)error + 40, sizeof(error_code));
    if (!state && !flags && !error_code && page == (size_t)getpagesize() && count &&
        alignment_pages && !(alignment_pages & (alignment_pages - 1)) &&
        count <= SIZE_MAX / page && alignment_pages <= SIZE_MAX / page) {
        void *p = tl_bionic_reserve_aligned(page * count, page * alignment_pages);
        if (p != MAP_FAILED) return (tl_unity_pages){p, page, count};
    }
    return original(page, count, alignment_pages, state, flags, error);
}

/* Called while the loader holds its lock, before any library constructors.
 * The full allocator+validator digest is checked by the loader first. These
 * four position-independent prologue instructions are also checked here; the
 * trampoline executes them and branches into the already translated original.
 * Both code writes use the existing JIT writable view, never RWX or new JIT. */
bool tl_unity_vm_redirect(uint8_t *site_rw, uint8_t *site_rx, uint8_t *stub_rw, uint8_t *stub_rx)
{
    const uint32_t prologue[] = {0xd10283ff, 0xf9002bfe, 0xa90667fa, 0xa9075ff8};
    intptr_t delta = (intptr_t)(uintptr_t)(site_rx + 16) - (intptr_t)(uintptr_t)(stub_rx + 16);
    if (original || memcmp(site_rw, prologue, sizeof(prologue)) ||
        delta % 4 || delta < -0x8000000 || delta >= 0x8000000) return false;
    uint32_t back = 0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu);
    uint32_t jump[] = {0x58000050u, 0xd61f0200u}; /* ldr x16, +8; br x16 */
    uintptr_t target = (uintptr_t)tl_unity_vm_allocate;
    memcpy(stub_rw, prologue, sizeof(prologue));
    memcpy(stub_rw + 16, &back, 4);
    original = (tl_unity_page_fn)(void *)stub_rx;
    memcpy(site_rw, jump, sizeof(jump));
    memcpy(site_rw + 8, &target, sizeof(target));
    return true;
}
