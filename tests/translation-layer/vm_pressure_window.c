/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real Mach mappings constrained to a synthetic 6 GiB test window. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#define START 0x7000000000ULL
#define END 0x7180000000ULL
static kern_return_t bounded_map(vm_map_t task, vm_address_t *address, vm_size_t size,
        vm_address_t mask, int flags, mem_entry_name_port_t object, vm_offset_t offset,
        boolean_t copy, vm_prot_t prot, vm_prot_t max, vm_inherit_t inherit) {
    if (!(flags & VM_FLAGS_ANYWHERE))
        return mach_vm_map(task, (mach_vm_address_t *)address, size, mask, flags,
                           object, offset, copy, prot, max, inherit);
    uint64_t start = *address > START ? *address : START;
    for (uint64_t pos = (start + mask) & ~mask;
         pos <= END && size <= END - pos; pos += 0x4000) {
        if (pos & mask) continue;
        mach_vm_address_t at = pos;
        kern_return_t kr = mach_vm_map(task, &at, size, 0, flags & ~VM_FLAGS_ANYWHERE,
                                      object, offset, copy, prot, max, inherit);
        if (kr == KERN_SUCCESS) { *address = at; return kr; }
    }
    return KERN_NO_SPACE;
}
static void *bounded_mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off) {
    if (!(flags & MAP_ANON) || (flags & MAP_FIXED)) return mmap(addr,n,prot,flags,fd,off);
    vm_address_t at = (uintptr_t)addr;
    kern_return_t kr = bounded_map(mach_task_self(), &at, n, 0x3fff, VM_FLAGS_ANYWHERE,
                                  MACH_PORT_NULL, 0, FALSE, prot, VM_PROT_ALL, VM_INHERIT_COPY);
    if (kr != KERN_SUCCESS) { errno = ENOMEM; return MAP_FAILED; }
    return (void *)at;
}
__attribute__((used,section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } hooks[] = {
    { bounded_map, vm_map }, { bounded_mmap, mmap }
};
