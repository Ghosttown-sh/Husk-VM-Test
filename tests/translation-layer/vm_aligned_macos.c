/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mach/mach.h>
#include <sys/mman.h>
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#define START 0x7200000000ULL
#define CHUNK 0x10000000ULL
#define SLOTS 12
#define END (START + SLOTS * CHUNK)
static bool bounded = true;
static unsigned fallback_calls;
static kern_return_t test_info(task_name_t t, task_flavor_t f, task_info_t v, mach_msg_type_number_t *n) {
    kern_return_t kr = task_info(t,f,v,n);
    if (kr == KERN_SUCCESS && bounded && f == TASK_VM_INFO) {
        ((task_vm_info_t)v)->min_address=START;
        ((task_vm_info_t)v)->max_address=END;
    }
    return kr;
}
static kern_return_t test_map(vm_map_t t, vm_address_t *a, vm_size_t n, vm_address_t mask,
        int flags, mem_entry_name_port_t o, vm_offset_t off, boolean_t copy,
        vm_prot_t p, vm_prot_t max, vm_inherit_t inherit) {
    assert(!(flags & VM_FLAGS_OVERWRITE));
    if (bounded && (flags & VM_FLAGS_ANYWHERE)) return KERN_NO_SPACE;
    return vm_map(t,a,n,mask,flags,o,off,copy,p,max,inherit);
}
#define task_info test_info
#define vm_map test_map
#define TL_VM_NO_DELEGATED_RANGES 1
#include "../../src/translation-layer-next/husk-tl-vm.c"
#include "production_vm.inc"
#include "../../src/translation-layer-next/husk-tl-unity-vm.c"
#undef vm_map
#undef task_info
void tl_log_line(const char *fmt, ...) { (void)fmt; }
void tl_note_once(const char *s) { (void)s; }
void tl_set_guest_errno(int e) { (void)e; }
int tl_errno_to_guest(int e) { return e; }
bool tl_ld_covers(const void *p,size_t n) { (void)p; (void)n; return false; }
bool tl_codewrite_enabled(void) { return false; }
bool tl_xmem_contains(const void *p) { (void)p; return false; }
bool tl_ld_aot_contains(const void *p) { (void)p; return false; }
static tl_unity_pages fallback(size_t page,size_t count,size_t alignment,uint32_t state,uint32_t flags,void *error) {
    (void)page;(void)count;(void)alignment;(void)state;(void)flags;(void)error;
    fallback_calls++;
    return (tl_unity_pages){NULL,0,0};
}
static void check_none(uintptr_t p, size_t size) {
    vm_address_t a=p; vm_size_t n=0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t c=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj=MACH_PORT_NULL;
    assert(vm_region_64(mach_task_self(),&a,&n,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&c,&obj)==KERN_SUCCESS);
    if(obj) mach_port_deallocate(mach_task_self(),obj);
    assert(a<=p && n>=size && info.protection==VM_PROT_NONE);
}
int main(int argc, char **argv) {
    size_t page=getpagesize();
    printf("Aligned VM host page size: %zu; pointer size: %zu\n",page,sizeof(void *));
    original=fallback;
    /* Six independent 256 MiB holes, separated by existing JIT-like mappings.
     * The final occupied entry also proves that the walk stays within bounds. */
    for(unsigned i=1;i<=SLOTS;i+=2) {
        vm_address_t a=START+i*CHUNK;
        assert(vm_map(mach_task_self(),&a,CHUNK,0,VM_FLAGS_FIXED,MACH_PORT_NULL,0,FALSE,
            VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_COPY)==KERN_SUCCESS);
        assert(mprotect((void *)a,page,PROT_READ|PROT_WRITE)==0);
        *(volatile uint32_t *)a=0x42420000+i;
    }
    assert(tl_vm_reserve_gap(0x1fffc000)==MAP_FAILED);
    puts("PASS: original near-512 MiB reservation cannot fit any 256 MiB hole");
    task_vm_info_data_t before={0},after={0}; assert(vm_info(&before));
    unsigned char error[48]={0};
    for(unsigned cycle=0;cycle<24;cycle++) {
        tl_unity_pages live[SLOTS/2];
        for(unsigned i=0;i<SLOTS/2;i++) {
            live[i]=tl_unity_vm_allocate(page,CHUNK/page,CHUNK/page,0,0,error);
            assert(live[i].address==(void *)(START+2*i*CHUNK));
            assert(live[i].page_size==page && live[i].page_count==CHUNK/page);
            assert(anon_contains((uintptr_t)live[i].address,CHUNK));
            check_none((uintptr_t)live[i].address,CHUNK);
            assert(b_mprotect(live[i].address,page,PROT_READ|PROT_WRITE)==0);
            assert(*(volatile uint32_t *)live[i].address==0);
            *(volatile uint32_t *)live[i].address=cycle+1;
            assert(*(volatile uint32_t *)(START+(2*i+1)*CHUNK)==0x42420000+2*i+1);
        }
        unsigned calls=fallback_calls;
        assert(!tl_unity_vm_allocate(page,CHUNK/page,CHUNK/page,0,0,error).address);
        assert(fallback_calls==calls+1); /* real exhaustion delegates error handling */
        assert(vm_info(&after));
        assert(after.phys_footprint < before.phys_footprint + (64ULL<<20));
        for(unsigned i=0;i<SLOTS/2;i++) assert(b_munmap(live[i].address,CHUNK)==0);
        assert(g_nanon==0);
    }
    puts("PASS: 144 native aligned allocations, honest exhaustion/recovery, zero-fill, JIT sentinels and low physical footprint");
    void *p=tl_bionic_reserve_aligned(CHUNK,CHUNK);
    assert(p==(void *)START);
    assert(b_munmap((char *)p+CHUNK/2,CHUNK/2)==0);
    void *right=tl_bionic_reserve_aligned(CHUNK/2,CHUNK/2);
    assert(right==(char *)p+CHUNK/2);
    assert(b_munmap((char *)p+1,page)==-1);
    assert(anon_contains((uintptr_t)p,CHUNK/2));
    assert(b_munmap(p,CHUNK/2)==0 && b_munmap(right,CHUNK/2)==0 && g_nanon==0);
    puts("PASS: partial unmap, smaller aligned reuse and failed-unmap ownership");
    unsigned calls=fallback_calls;
    (void)tl_unity_vm_allocate(page,0,1,0,0,error);
    (void)tl_unity_vm_allocate(page,1,3,0,0,error);
    (void)tl_unity_vm_allocate(page,1,1,1,0,error);
    (void)tl_unity_vm_allocate(page,1,1,0,1,error);
    (void)tl_unity_vm_allocate(page,SIZE_MAX,1,0,0,error);
    (void)tl_unity_vm_allocate(page,1,(SIZE_MAX/2)+1,0,0,error);
    error[40]=1; (void)tl_unity_vm_allocate(page,1,1,0,0,error);
    assert(fallback_calls==calls+7 && g_nanon==0);
    assert(tl_vm_reserve_aligned(page,3*page)==MAP_FAILED && errno==EINVAL);
    assert(tl_vm_reserve_aligned(page-1,page)==MAP_FAILED && errno==EINVAL);
    puts("PASS: unsupported parameters, existing errors and overflow retain original ABI/error path");
    setenv("TL_MM_MAX_GIB","0",1);
    assert(tl_bionic_reserve_aligned(page,page)==MAP_FAILED && errno==ENOMEM);
    unsetenv("TL_MM_MAX_GIB");
    g_nanon=MAX_ANON;
    assert(tl_bionic_reserve_aligned(CHUNK,CHUNK)==MAP_FAILED && errno==ENOMEM);
    g_nanon=0;
    p=tl_bionic_reserve_aligned(CHUNK,CHUNK);
    assert(p==(void *)START && b_munmap(p,CHUNK)==0);
    puts("PASS: explicit allocation limit and full ownership table fail without leaking a reservation");
    for(unsigned i=1;i<=SLOTS;i+=2) assert(munmap((void *)(START+i*CHUNK),CHUNK)==0);
    bounded=false;
    p=tl_bionic_reserve_aligned(CHUNK,CHUNK);
    assert(p!=MAP_FAILED && !((uintptr_t)p&(CHUNK-1)));
    check_none((uintptr_t)p,CHUNK);
    assert(b_munmap(p,CHUNK)==0);
    puts("PASS: real unrestricted Mach aligned allocation uses requested alignment without temporary overreservation");
    /* Export only Husk's generated 16-byte redirect and 20-byte trampoline,
     * never any APK bytes. The local ARM64 APK test consumes this exact output. */
    original=NULL;
    uint32_t entry[4]={0xd10283ff,0xf9002bfe,0xa90667fa,0xa9075ff8};
    uint8_t trampoline[32]={0};
    entry[0]^=1;
    assert(!tl_unity_vm_redirect((void *)entry,(void *)0x100f000acULL,trampoline,(void *)0x101600000ULL));
    entry[0]^=1;
    assert(!tl_unity_vm_redirect((void *)entry,(void *)0x100f000acULL,trampoline,(void *)0x200000000ULL));
    assert(tl_unity_vm_redirect((void *)entry,(void *)0x100f000acULL,trampoline,(void *)0x101600000ULL));
    assert(!tl_unity_vm_redirect((void *)entry,(void *)0x100f000acULL,trampoline,(void *)0x101600000ULL));
    if(argc==2) {
        FILE *f=fopen(argv[1],"wb"); assert(f);
        assert(fwrite(entry,1,sizeof(entry),f)==sizeof(entry));
        assert(fwrite(trampoline,1,sizeof(trampoline),f)==sizeof(trampoline));
        assert(!fclose(f));
    }
    puts("PASS: redirect rejects changed prologue, out-of-range trampoline and duplicate installation");
}
