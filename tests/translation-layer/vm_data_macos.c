/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the production mmap/read/remap functions against Darwin, with a
 * separate RX sentinel standing in for the one-shot device JIT region. */
#include "production_vm.inc"
#include <assert.h>
static char *jit_rx, *jit_rw;
static size_t page;
static unsigned jit_calls;
void tl_log_line(const char *fmt, ...) { (void)fmt; }
void tl_note_once(const char *s) { (void)s; }
void tl_set_guest_errno(int e) { (void)e; }
int tl_errno_to_guest(int e) { return e; }
bool tl_ld_covers(const void *p,size_t n) { (void)p; (void)n; return false; }
bool tl_ld_aot_contains(const void *p) { (void)p; return false; }
bool tl_codewrite_enabled(void) { return true; }
bool tl_xmem_contains(const void *p) { return (uintptr_t)p >= (uintptr_t)jit_rx && (uintptr_t)p < (uintptr_t)jit_rx+page; }
bool tl_xmem_near(const void *p,size_t n) { (void)p; (void)n; return true; }
bool tl_xmem_alloc(size_t n,uint8_t **rx,uint8_t **rw) {
    assert(n<=page); jit_calls++; *rx=(uint8_t *)jit_rx; *rw=(uint8_t *)jit_rw; return true;
}
bool husk_binder_is_fd(int fd) { (void)fd; return false; }
bool tl_timerfd_is(int fd) { (void)fd; return false; }
static void protection(void *p,int wanted) {
    vm_address_t a=(vm_address_t)p; vm_size_t n=0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t c=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object=MACH_PORT_NULL;
    assert(vm_region_64(mach_task_self(),&a,&n,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&c,&object)==KERN_SUCCESS);
    if(object) mach_port_deallocate(mach_task_self(),object);
    assert(a<=(vm_address_t)p && info.protection==wanted);
}
int main(void) {
    page=(size_t)getpagesize();
    printf("Data/JIT separation: real Darwin %zu-byte pages\n",page);
    setenv("TL_PHONE_JIT","1",1); /* Reproduces the old hint heuristic on Mac. */
    jit_rx=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    jit_rw=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(jit_rx!=MAP_FAILED && jit_rw!=MAP_FAILED);
    memset(jit_rx,0x6b,page);
    assert(mprotect(jit_rx,page,PROT_READ|PROT_EXEC)==0);
    FILE *f=tmpfile(); assert(f); char sample[64]; memset(sample,0x39,sizeof(sample));
    assert(fwrite(sample,1,sizeof(sample),f)==sizeof(sample)); fflush(f);
    void *hint=jit_rx;
    for(int cycle=0;cycle<128;cycle++) {
        char *p=b_mmap(hint,page,PROT_READ|PROT_WRITE,0x22,-1,0);
        assert(p!=MAP_FAILED && p!=jit_rx && jit_calls==0);
        protection(p,PROT_READ|PROT_WRITE);
        memset(p,0x55,page); /* No store emulation/signal handler in this test. */
        assert(lseek(fileno(f),0,SEEK_SET)==0);
        assert(b_read(fileno(f),p,sizeof(sample))==sizeof(sample));
        assert(memcmp(p,sample,sizeof(sample))==0);
        assert(b_mmap(p,page,PROT_NONE,0x32,-1,0)==p);
        protection(p,PROT_NONE);
        assert(b_mmap(p,page,PROT_READ|PROT_WRITE,0x32,-1,0)==p);
        protection(p,PROT_READ|PROT_WRITE);
        for(size_t i=0;i<page;i++) assert(p[i]==0);
        hint=p+page;
        assert(b_munmap(p,page)==0);
        protection(jit_rx,PROT_READ|PROT_EXEC);
        assert((unsigned char)jit_rx[page-1]==0x6b);
    }
    assert(b_mmap(NULL,page,PROT_READ|PROT_EXEC,0x22,-1,0)==jit_rx && jit_calls==1);
    fclose(f); munmap(jit_rx,page); munmap(jit_rw,page);
    puts("PASS: 128 hinted RW allocations, kernel reads, GC decommit/recommit, zero-fill and unmap; RX sentinel and explicit JIT preserved");
}
