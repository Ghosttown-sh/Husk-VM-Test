/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mach/mach.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* An isolated range in the Mac's real task map. The synthetic heap mmap
 * fails after a reclaimed cage has already been consumed. The fallback's
 * task bounds are constrained here; all mappings/protections remain real. */
#define START 0x7200000000ULL
#define END   (START + 0x80000000ULL)
static bool bounded = true, inject_race;
static vm_address_t raced;
static unsigned fallback_maps;
static kern_return_t test_task_info(task_name_t t, task_flavor_t flavor, task_info_t info,
                                    mach_msg_type_number_t *count) {
    kern_return_t kr = task_info(t, flavor, info, count);
    if (kr == KERN_SUCCESS && bounded && flavor == TASK_VM_INFO) {
        task_vm_info_t v = (task_vm_info_t)info;
        v->min_address = START; v->max_address = END;
    }
    return kr;
}
static void *test_mmap(void *a, size_t n, int p, int flags, int fd, off_t off) {
    if ((flags & MAP_ANON) && !(flags & MAP_FIXED) && p == PROT_NONE) {
        errno = ENOMEM; return MAP_FAILED;
    }
    return mmap(a,n,p,flags,fd,off);
}
static kern_return_t test_vm_map(vm_map_t task, vm_address_t *at, vm_size_t n,
        vm_address_t mask, int flags, mem_entry_name_port_t object, vm_offset_t off,
        boolean_t copy, vm_prot_t prot, vm_prot_t max, vm_inherit_t inherit) {
    assert(!(flags & VM_FLAGS_OVERWRITE));
    if (inject_race) {
        inject_race = false;
        raced = *at;
        assert(vm_map(task, &raced, getpagesize(), 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0,
            FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY) == KERN_SUCCESS);
        *(volatile char *)raced = 73;
    }
    fallback_maps++;
    return vm_map(task, at, n, mask, flags, object, off, copy, prot, max, inherit);
}
#define task_info test_task_info
#define mmap test_mmap
#define vm_map test_vm_map
#include "../../src/translation-layer-next/husk-tl-vm.c"
#include "production_vm.inc"
#undef task_info
#undef mmap
#undef vm_map
void tl_log_line(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); fputc('\n',stderr); va_end(ap);
}
void tl_note_once(const char *s) { (void)s; }
void tl_set_guest_errno(int e) { (void)e; }
int tl_errno_to_guest(int e) { return e; }
bool tl_ld_covers(const void *p,size_t n) { (void)p; (void)n; return false; }
bool tl_codewrite_enabled(void) { return false; }
bool tl_xmem_contains(const void *p) { (void)p; return false; }
bool tl_ld_aot_contains(const void *p) { (void)p; return false; }

static void *reserve_unity(void) {
    char *raw = b_mmap(NULL,0x1fffc000,PROT_NONE,0x22,-1,0);
    assert(raw != MAP_FAILED);
    uintptr_t aligned = ((uintptr_t)raw + 0xfffffff) & ~(uintptr_t)0xfffffff;
    size_t prefix = aligned - (uintptr_t)raw;
    size_t suffix = (uintptr_t)raw + 0x1fffc000 - aligned - 0x10000000;
    if (prefix) assert(b_munmap(raw,prefix) == 0);
    if (suffix) assert(b_munmap((void *)(aligned+0x10000000),suffix) == 0);
    assert(b_mprotect((void *)aligned,getpagesize(),PROT_READ|PROT_WRITE) == 0);
    assert(*(volatile char *)aligned == 0);
    *(volatile char *)aligned = 42;
    return (void *)aligned;
}
static void readv_tests(void) {
    size_t page = getpagesize();
    char *src=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    char *dst=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(src!=MAP_FAILED && dst!=MAP_FAILED);
    memset(src,0x5a,page*2);
    struct iovec l[]={{dst,3},{dst+3,page*2-3}}, r[]={{src,7},{src+7,page*2-7}};
    assert(tl_vm_process_readv(getpid(),l,2,r,2,0)==(long)(page*2));
    assert(memcmp(src,dst,page*2)==0);
    assert(tl_vm_process_readv(getpid(),l,2,r,2,1)==-22);
    assert(tl_vm_process_readv(getpid(),l,1025,r,2,0)==-22);
    assert(tl_vm_process_readv(getpid(),(void *)1,1,r,2,0)==-14);
    assert(mprotect(src+page,page,PROT_NONE)==0);
    l[0]=(struct iovec){dst,page*2}; r[0]=(struct iovec){src,page*2};
    assert(tl_vm_process_readv(getpid(),l,1,r,1,0)==(long)page);
    r[0]=(struct iovec){src+page,page};
    assert(tl_vm_process_readv(getpid(),l,1,r,1,0)==-14);
    r[0]=(struct iovec){src,page}; l[0]=(struct iovec){(void *)1,page};
    assert(tl_vm_process_readv(getpid(),l,1,r,1,0)==-14);
    assert(tl_vm_process_readv(getpid(),NULL,0,NULL,0,0)==0);
    munmap(src,page*2); munmap(dst,page*2);
    puts("PASS: process_vm_readv copies real data, gathers/scatters, short reads, EFAULT, EINVAL and zero-length reads");
}
int main(void) {
    printf("VM gap integration host page size: %d bytes\n",getpagesize());
    vm_address_t boundary=END;
    assert(vm_map(mach_task_self(),&boundary,getpagesize(),0,VM_FLAGS_FIXED,MACH_PORT_NULL,
        0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_COPY)==KERN_SUCCESS);
    assert(test_mmap(NULL,0x1fffc000,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0)==MAP_FAILED);
    task_vm_info_data_t before={0}, after={0}; assert(vm_info(&before));
    inject_race=true;
    void *live[5];
    for (int i=0;i<5;i++) live[i]=reserve_unity();
    assert(vm_info(&after));
    assert(after.phys_footprint < before.phys_footprint + (64ULL<<20));
    assert(*(volatile char *)raced==73);
    void *released=live[2];
    assert(b_munmap(live[2],0x10000000)==0);
    /* A smaller reservation reuses the freed hole, not a monotonic cursor. */
    live[2]=b_mmap(NULL,0x10000000,PROT_NONE,0x22,-1,0);
    assert(live[2]==released);
    assert(b_mprotect(live[2],getpagesize(),PROT_READ|PROT_WRITE)==0);
    assert(*(volatile char *)live[2]==0);
    /* Deliberately invalid unmap must not remove the ownership record. */
    assert(b_munmap((char *)live[2]+1,getpagesize())==-1);
    assert(anon_contains((uintptr_t)live[2],0x10000000));
    unsigned saved=fallback_maps;
    assert(b_mmap(NULL,0x1fffc000,PROT_NONE,0x21,-1,0)==MAP_FAILED);
    assert(fallback_maps==saved); /* shared is never changed into private */
    for(int i=0;i<5;i++) assert(b_munmap(live[i],0x10000000)==0);
    assert(g_nanon==0);
    assert(munmap((void *)raced,getpagesize())==0);
    /* Fill the entire permitted test window: must honestly return ENOMEM. */
    vm_address_t full=START;
    assert(vm_map(mach_task_self(),&full,END-START,0,VM_FLAGS_FIXED,MACH_PORT_NULL,
        0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_COPY)==KERN_SUCCESS);
    assert(b_mmap(NULL,0x1fffc000,PROT_NONE,0x22,-1,0)==MAP_FAILED);
    assert(munmap((void *)full,END-START)==0);
    assert(munmap((void *)boundary,getpagesize())==0);
    puts("PASS: repeated real 512 MiB reservations after heap ENOMEM, trims, reuse, no overwrite during race, low footprint, honest exhaustion and failed-unmap bookkeeping");
    bounded=false;
    readv_tests();
}
