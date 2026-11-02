/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* macOS release kernels may disable map ranges entirely (ENOENT), whereas
 * iOS enables them. In that case model ONLY range registration/bounds over a
 * test-owned, max-protection-zero window. All resulting mappings are real.
 * The log explicitly distinguishes this from testing range_create on iOS. */
static uintptr_t model_start, model_end;
static struct { uintptr_t start,end; } model_registered[64];
static int model_count;
static int range_sysctl(const char *name,void *old,size_t *len,void *newp,size_t newlen) {
    if (!model_start || strcmp(name,"vm.malloc_ranges")) return sysctlbyname(name,old,len,newp,newlen);
    char text[128];
    int n=snprintf(text,sizeof(text),"0x%llx:0x%llx 0x%llx:0x%llx",
        (unsigned long long)model_start,(unsigned long long)model_end,
        (unsigned long long)model_end,(unsigned long long)model_end);
    if(*len<(size_t)n) { errno=ENOMEM; return -1; }
    memcpy(old,text,n); *len=n; return 0;
}

/* Only the ordinary heap/gap placement is constrained. Delegation, protection,
 * data access, trimming, freeing and reusing use the real Darwin kernel. */
static uintptr_t occupied;
static kern_return_t bounded_task_info(task_name_t t, task_flavor_t flavor, task_info_t info,
                                       mach_msg_type_number_t *count) {
    kern_return_t kr = task_info(t, flavor, info, count);
    if (kr == KERN_SUCCESS && occupied && flavor == TASK_VM_INFO) {
        task_vm_info_t v = (task_vm_info_t)info;
        v->min_address = occupied; v->max_address = occupied + getpagesize();
    }
    return kr;
}
static void *full_heap_mmap(void *a, size_t n, int p, int flags, int fd, off_t off) {
    if ((flags & MAP_ANON) && !(flags & MAP_FIXED) && p == PROT_NONE) {
        errno = ENOMEM; return MAP_FAILED;
    }
    return mmap(a,n,p,flags,fd,off);
}
#define sysctlbyname range_sysctl
#include "../../src/translation-layer-next/husk-tl-vm-range.c"
#undef sysctlbyname
static kern_return_t model_create(vm_map_t task,uint32_t flavor,uint8_t *bytes,uint32_t size) {
    if(size!=sizeof(range_recipe) || flavor!=1) return KERN_INVALID_ARGUMENT;
    range_recipe *r=(range_recipe *)bytes;
    if(r->flags_and_tags!=(UINT64_C(2)<<48) || r->start<model_start || r->end>model_end
       || r->start>=r->end || ((r->start|r->end)&(getpagesize()-1))) return KERN_INVALID_ARGUMENT;
    for(int i=0;i<model_count;i++)
        if(r->start<model_registered[i].end && r->end>model_registered[i].start) return KERN_MEMORY_PRESENT;
    if(model_count==64) return KERN_NO_SPACE;
    vm_address_t at=r->start;
    /* OVERWRITE belongs only to this model of the kernel's delegation step:
     * it replaces our own original test-window padding, after range checks.
     * Production userspace code never uses it. */
    kern_return_t kr=vm_map(task,&at,r->end-r->start,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
        MACH_PORT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_COPY);
    if(kr==KERN_SUCCESS) {
        model_registered[model_count].start=r->start;
        model_registered[model_count++].end=r->end;
    }
    return kr;
}
#define task_info bounded_task_info
#define mmap full_heap_mmap
#include "../../src/translation-layer-next/husk-tl-vm.c"
#include "production_vm.inc"
#undef task_info
#undef mmap
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
static int registrations(void) { int n=0; for(owned_range *r=ranges;r;r=r->next)n++; return n; }
static void *unity_reserve(void) {
    uintptr_t raw = (uintptr_t)b_mmap(NULL,0x1fffc000,PROT_NONE,0x22,-1,0);
    assert((void *)raw != MAP_FAILED);
    uintptr_t a = (raw + 0xfffffff) & ~(uintptr_t)0xfffffff;
    size_t prefix=a-raw, suffix=raw+0x1fffc000-a-0x10000000;
    if(prefix) assert(b_munmap((void *)raw,prefix)==0);
    if(suffix) assert(b_munmap((void *)(a+0x10000000),suffix)==0);
    size_t page=getpagesize();
    assert(b_mprotect((void *)a,page,PROT_READ|PROT_WRITE)==0);
    assert(b_mprotect((void *)(a+0x10000000-page),page,PROT_READ|PROT_WRITE)==0);
    assert(*(volatile char *)a==0);
    assert(*(volatile char *)(a+0x10000000-1)==0);
    *(volatile char *)a=42; *(volatile char *)(a+0x10000000-1)=73;
    return (void *)a;
}
static kern_return_t denied(vm_map_t task,uint32_t flavor,uint8_t *recipe,uint32_t size) {
    (void)task;(void)flavor;(void)recipe;(void)size; return KERN_PROTECTION_FAILURE;
}
static range_create_fn real_create;
static uintptr_t raced;
static kern_return_t race_create(vm_map_t task,uint32_t flavor,uint8_t *recipe,uint32_t size) {
    range_recipe *r=(range_recipe *)recipe;
    assert(size==sizeof(*r) && flavor==1 && r->flags_and_tags==(UINT64_C(2)<<48));
    assert(real_create(task,flavor,recipe,size)==KERN_SUCCESS);
    raced=r->start;
    assert(mprotect((void *)raced,getpagesize(),PROT_READ|PROT_WRITE)==0);
    *(volatile char *)raced=91;
    range_create=denied; /* no second racing allocation */
    return KERN_MEMORY_PRESENT;
}
int main(void) {
    printf("Delegated VM integration host page size: %d bytes\n",getpagesize());
    setvbuf(stdout,NULL,_IONBF,0);
    uintptr_t b[4];
    assert(parse_ranges("0x1000:0x2000 0x3000:0x4000",b));
    assert(!parse_ranges("0x2000:0x1000 0x3000:0x4000",b));
    assert(!parse_ranges("0x1:0x2 0x3:0x4 trailing",b));
    assert(!parse_ranges("-1:0x2 0x3:0x4",b));
    assert(tl_vm_reserve_delegated(0)==MAP_FAILED);
    assert(tl_vm_reserve_delegated(SIZE_MAX)==MAP_FAILED);
    pthread_once(&range_once,find_range_create);
    char reported[128]; size_t reported_size=sizeof(reported)-1;
    if(!range_create || sysctlbyname("vm.malloc_ranges",reported,&reported_size,NULL,0)) {
        printf("SKIP: real mach_vm_range_create integration unavailable on this Mac (bounds errno=%d); using registration-contract model with real Mach mappings\n",errno);
        vm_address_t window=0;
        assert(vm_map(mach_task_self(),&window,16ULL<<30,0,VM_FLAGS_ANYWHERE,MACH_PORT_NULL,
            0,FALSE,VM_PROT_NONE,VM_PROT_NONE,VM_INHERIT_COPY)==KERN_SUCCESS);
        model_start=window; model_end=window+(16ULL<<30); range_create=model_create;
    } else puts("Testing REAL kernel range registration, not the contract model");
    void *guard=mmap(NULL,getpagesize(),PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(guard!=MAP_FAILED); occupied=(uintptr_t)guard;
    task_vm_info_data_t before={0},after={0}; assert(vm_info(&before));
    void *live[24];
    for(int i=0;i<24;i++) live[i]=unity_reserve();
    assert(registrations()==24);
    assert(vm_info(&after));
    assert(after.phys_footprint < before.phys_footprint+(64ULL<<20));
    printf("PASS: 24 real 512 MiB Unity reservations with ordinary heap and gaps exhausted; 6 GiB live aligned VA, footprint delta %llu KiB\n",
        (unsigned long long)((after.phys_footprint-before.phys_footprint)>>10));
    void *reused=live[7]; assert(b_munmap(reused,0x10000000)==0);
    live[7]=unity_reserve(); assert(live[7]==reused && registrations()==24);
    puts("PASS: freed delegated range reused without another kernel registration; first/last committed pages zero-filled");
    real_create=range_create; assert(real_create);
    range_create=denied;
    assert(tl_vm_reserve_delegated(0x1fffc000)==MAP_FAILED && errno==ENOMEM);
    assert(registrations()==24);
    range_create=race_create;
    assert(tl_vm_reserve_delegated(0x1fffc000)==MAP_FAILED);
    assert(raced && *(volatile char *)raced==91 && registrations()==24);
    for(int i=0;i<24;i++) {
        assert(*(volatile char *)live[i]==42);
        assert(*(volatile char *)((uintptr_t)live[i]+0x10000000-1)==73);
        assert(b_munmap(live[i],0x10000000)==0);
    }
    assert(g_nanon==0);
    puts("PASS: kernel denial and racing ownership preserved, no overwritten mappings or fake success, all guest mappings released");
    range_create=real_create;
    assert(munmap((void *)raced,0x1fffc000)==0);
    assert(munmap(guard,getpagesize())==0);
    if(model_start) assert(munmap((void *)model_start,model_end-model_start)==0);
}
