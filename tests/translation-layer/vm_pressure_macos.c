/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../src/translation-layer-next/husk-tl-bionic-io.c"
#include <mach/mach_vm.h>
#include <assert.h>
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
int main(void) {
    assert(getpagesize()==16384); /* run on an Apple Silicon Mac */
    const uintptr_t start=0x7000000000ULL, gib=(uintptr_t)1<<30;
    husk_prepare_compressed_heap(); assert(g_compressed_heap==start);
    uintptr_t blockers[]={start+4*gib,start+4*gib+3*gib/4,start+5*gib+gib/2};
    for(int i=0;i<3;i++) {
        mach_vm_address_t at=blockers[i];
        assert(mach_vm_map(mach_task_self(),&at,gib/2,0,VM_FLAGS_FIXED,
            MACH_PORT_NULL,0,FALSE,VM_PROT_READ|VM_PROT_WRITE,VM_PROT_ALL,VM_INHERIT_COPY)==KERN_SUCCESS);
        *(volatile char *)at=(char)(i+40);
    }
    void *hint=(void *)(start+0x400000);
    assert(mmap(hint,0x1fffc000,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0)==MAP_FAILED && errno==ENOMEM);
    char *raw=b_mmap(hint,0x1fffc000,PROT_NONE,0x22,-1,0);
    assert(raw!=MAP_FAILED && !g_compressed_heap);
    uintptr_t aligned=((uintptr_t)raw+0xfffffff)&~(uintptr_t)0xfffffff;
    size_t prefix=aligned-(uintptr_t)raw, tail=(uintptr_t)raw+0x1fffc000-aligned-0x10000000;
    if(prefix) assert(b_munmap(raw,prefix)==0);
    if(tail) assert(b_munmap((void *)(aligned+0x10000000),tail)==0);
    assert(b_mprotect((void *)aligned,16384,PROT_READ|PROT_WRITE)==0);
    assert(b_mprotect((void *)(aligned+0x10000000-16384),16384,PROT_READ|PROT_WRITE)==0);
    assert(*(volatile char *)aligned==0 && *(volatile char *)(aligned+0x10000000-1)==0);
    *(volatile char *)aligned=42; *(volatile char *)(aligned+0x10000000-1)=43;
    assert(*(volatile char *)aligned==42 && *(volatile char *)(aligned+0x10000000-1)==43);
    assert(b_munmap((void *)aligned,0x10000000)==0);
    for(int i=0;i<3;i++) {
        assert(*(volatile char *)blockers[i]==i+40);
        assert(mach_vm_deallocate(mach_task_self(),blockers[i],gib/2)==KERN_SUCCESS);
    }
    puts("PASS: real Mach pressure recovery, Unity alignment trim, mprotect, zero fill, writes and munmap");
}
