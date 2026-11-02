/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded address-space model for the production b_mmap and cage lifecycle.
 * Darwin's MAP_ANON differs from Android's; deliberately keep that distinction. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANON 0x1000
#define MAP_FAILED ((void *)(intptr_t)-1)
#define TL_XMEM_PAGE 16384
#define PATH_MAX 1024
#define F_GETPATH 50
#define VM_FLAGS_ANYWHERE 1
#define MACH_PORT_NULL 0
#define FALSE 0
#define VM_PROT_NONE 0
#define VM_PROT_ALL 7
#define VM_INHERIT_COPY 1
#define KERN_SUCCESS 0
#define KERN_NO_SPACE 3
typedef uintptr_t vm_address_t;
typedef int kern_return_t;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static void pthread_mutex_lock(pthread_mutex_t *m) { assert(!*m); *m = 1; }
static void pthread_mutex_unlock(pthread_mutex_t *m) { assert(*m); *m = 0; }
static int mach_task_self(void) { return 1; }
static int getpagesize(void) { return TL_XMEM_PAGE; }

#define GIB ((size_t)1 << 30)
#define START ((uintptr_t)0x7000000000ULL)
#define END (START + 6 * GIB)
static struct region { uintptr_t addr; size_t size; int prot; } maps[32];
static int nmaps, calls, unmaps, fail_errno, injected_retry_errno, dealloc_fails;
static int guest_errno, loaded, log_clobbers_errno;
static void *last_hint;
static size_t last_size;
static int last_prot, last_flags, last_fd;
static long last_off;

static void add(uintptr_t a, size_t n, int prot) {
    assert(nmaps < 32); maps[nmaps++] = (struct region){a,n,prot};
}
static bool overlaps(uintptr_t a, size_t n) {
    for (int i=0;i<nmaps;i++) if (a < maps[i].addr+maps[i].size && a+n > maps[i].addr) return true;
    return false;
}
static void *reserve(size_t n, uintptr_t mask, int prot) {
    for (uintptr_t a=(START+mask)&~mask; a<=END && n<=END-a;) {
        if (!overlaps(a,n)) { add(a,n,prot); return (void *)a; }
        uintptr_t next=a+TL_XMEM_PAGE;
        for (int i=0;i<nmaps;i++) if (a < maps[i].addr+maps[i].size && a+n > maps[i].addr)
            if (maps[i].addr+maps[i].size>next) next=maps[i].addr+maps[i].size;
        a=(next+mask)&~mask;
    }
    errno=ENOMEM; return MAP_FAILED;
}
static void *mmap(void *a, size_t n, int p, int f, int fd, long off) {
    calls++; last_hint=a; last_size=n; last_prot=p; last_flags=f; last_fd=fd; last_off=off;
    if (fail_errno || (injected_retry_errno && unmaps)) { errno=fail_errno ? fail_errno : injected_retry_errno; return MAP_FAILED; }
    if (f&MAP_FIXED) { errno=ENOMEM; return MAP_FAILED; }
    return reserve(n,TL_XMEM_PAGE-1,p);
}
static int munmap(void *p, size_t n) {
    for (int i=0;i<nmaps;i++) if (maps[i].addr==(uintptr_t)p && maps[i].size==n) {
        maps[i]=maps[--nmaps]; unmaps++; return 0;
    }
    errno=EINVAL; return -1;
}
static kern_return_t vm_map(int task, vm_address_t *a, size_t n, uintptr_t mask,
        int flags, int object, int off, int copy, int prot, int max, int inherit) {
    void *p=reserve(n,mask,prot);
    if (p==MAP_FAILED) return KERN_NO_SPACE;
    *a=(uintptr_t)p; return KERN_SUCCESS;
}
static kern_return_t vm_deallocate(int task, vm_address_t a, size_t n) {
    return dealloc_fails ? KERN_NO_SPACE : munmap((void *)a,n);
}
static void tl_log_line(const char *fmt, ...) { if (log_clobbers_errno) errno=EIO; }
static void tl_note_once(const char *s) {}
static void tl_set_guest_errno(int e) { guest_errno=e; }
static int tl_errno_to_guest(int e) { return e; } /* these cases have identical Linux/Darwin values */
static bool husk_binder_is_fd(int fd) { return false; }
static void *husk_binder_mmap(int fd,size_t n) { assert(false); return NULL; }
static int fcntl(int fd,int op,char *p) { return -1; }
static void *tl_ld_map_code(const char *p,uint64_t o,size_t n,void *a) { return NULL; }
static bool tl_codewrite_enabled(void) { return false; }
static bool phone_jit(void) { return false; }
static bool tl_xmem_near(void *a,size_t n) { return false; }
static bool tl_xmem_alloc(size_t n,uint8_t **rx,uint8_t **rw) { return false; }
static bool tl_ld_covers(void *a,size_t n) { return loaded; }
static void *shifted_file_map(void *a,size_t n,int p,int f,int fd,long off) { assert(false); return MAP_FAILED; }
static int prot_filter(int p,const char *what) { return p & ~PROT_EXEC; }
static void anon_add(void *p,size_t n) {}
static void mm_trace(const char *what,void *a,size_t n,long x,long y) {}

#include "production_mm.inc"

static void reset(bool prewarm) {
    nmaps=calls=unmaps=fail_errno=injected_retry_errno=dealloc_fails=loaded=log_clobbers_errno=0;
    g_compressed_heap=0; g_nphantom=0; guest_errno=123;
    if (prewarm) { husk_prepare_compressed_heap(); assert(g_compressed_heap==START); }
    /* Three live JIT/runtime ranges, leaving only separated 256 MiB holes
     * beside the 4 GiB speculative cage. None may be overwritten/released. */
    add(START+4*GIB,GIB/2,PROT_READ|PROT_EXEC);
    add(START+4*GIB+3*GIB/4,GIB/2,PROT_READ|PROT_WRITE);
    add(START+5*GIB+GIB/2,GIB/2,PROT_READ|PROT_WRITE);
    errno=0;
}
static void blockers_intact(void) {
    const uintptr_t b[]={START+4*GIB,START+4*GIB+3*GIB/4,START+5*GIB+GIB/2};
    for (int k=0;k<3;k++) {
        bool found=false;
        for (int i=0;i<nmaps;i++) if (maps[i].addr==b[k] && maps[i].size==GIB/2) found=true;
        assert(found);
    }
}
int main(void) {
    const size_t unity=0x1fffc000;
    if (getenv("TL_MM_MAX_GIB")) {
        reset(true);
        assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED);
        assert(g_compressed_heap==START && calls==0 && unmaps==0 && guest_errno==ENOMEM);
        puts("PASS: explicit TL_MM_MAX_GIB refusal is not bypassed by reclamation");
        return 0;
    }
    reset(true);
    assert(mmap(NULL,unity,0,MAP_PRIVATE|MAP_ANON,-1,0)==MAP_FAILED && errno==ENOMEM);
    calls=0;
    void *p=b_mmap(NULL,unity,0,0x22,-1,0);
    if (p==MAP_FAILED) { puts("FAIL: Unity's exact reservation remains blocked by the unused 4 GiB cage"); return 1; }
    assert(p==(void *)START && !g_compressed_heap && calls==2 && unmaps==1);
    assert(last_size==unity && last_prot==PROT_NONE && last_flags==(MAP_PRIVATE|MAP_ANON));
    assert(last_fd==-1 && last_off==0 && last_hint==NULL && errno==0 && guest_errno==123);
    blockers_intact();
    puts("PASS: exact 0x1fffc000/PROT_NONE/0x22 request recovers; live JIT/runtime ranges intact");

    reset(true); assert(b_mmap(NULL,16384,0,0x22,-1,0)!=MAP_FAILED);
    assert(g_compressed_heap==START && unmaps==0 && calls==1);
    reset(false); fail_errno=ENOMEM;
    assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED && calls==1 && guest_errno==ENOMEM);
    reset(true); dealloc_fails=1;
    assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED && calls==1 && g_compressed_heap==START);
    dealloc_fails=0; assert(b_mmap(NULL,unity,0,0x22,-1,0)!=MAP_FAILED);
    puts("PASS: successful mappings retain the cage; no cage and failed release preserve errors");

    reset(true); p=phantom_reserve(8*GIB);
    assert(p==(void *)START && !g_compressed_heap && g_nphantom==1);
    assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED && unmaps==0);
    husk_release_unused_compressed_heap(); assert(unmaps==0 && overlaps(START,4*GIB));
    reset(true); p=b_mmap(NULL,8*GIB,0,0x22,-1,0);
    assert(p==(void *)START && !g_compressed_heap && unmaps==0 && g_nphantom==1);
    assert(phantom_clamp(START+4*GIB,4*GIB)==0);
    reset(true); p=b_mmap(NULL,8*GIB-TL_XMEM_PAGE,0,0x22,-1,0);
    assert(p==(void *)START && !g_compressed_heap && unmaps==0 && g_nphantom==1);
    puts("PASS: compressed-heap requests consume the cage; a guest-owned cage is never reclaimed");

    const int errors[]={EINVAL,EACCES};
    for(int i=0;i<2;i++) {
        reset(true); fail_errno=errors[i];
        assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED && calls==1 && unmaps==0 && guest_errno==errors[i]);
    }
    reset(true); loaded=1;
    assert(b_mmap((void *)START,unity,0,0x32,-1,0)==MAP_FAILED && calls==0 && unmaps==0);
    reset(true); fail_errno=ENOMEM;
    assert(b_mmap(NULL,unity,0,2,9,0)==MAP_FAILED && calls==1 && unmaps==0);
    reset(true); fail_errno=ENOMEM;
    assert(b_mmap(NULL,unity,0,0x21,-1,0)==MAP_FAILED && calls==1 && unmaps==0);
    puts("PASS: file/shared/fixed mappings, loader refusals and non-ENOMEM errors are unchanged");

    reset(true); injected_retry_errno=EACCES; log_clobbers_errno=1;
    assert(b_mmap(NULL,unity,0,0x22,-1,0)==MAP_FAILED && calls==2 && unmaps==1);
    assert(errno==EACCES && guest_errno==EACCES);
    reset(true); log_clobbers_errno=1;
    p=b_mmap((void *)(START+GIB),unity,0,0x22,4,0x4000);
    assert(p!=MAP_FAILED && last_hint==(void *)(START+GIB) && last_off==0x4000 && errno==0 && guest_errno==123);
    puts("PASS: retry is bounded, preserves arguments and publishes final errno despite logging");
    return 0;
}
