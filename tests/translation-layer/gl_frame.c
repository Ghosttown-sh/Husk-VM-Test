/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/translation-layer-next/husk-tl-gl-frame.c"

static void *ctx = (void *)1;
static unsigned id=20, images, fences, destroyed, copies, waits, finishes, errors, injected;
static int tex=7, fb=9, unpack=11;
static bool image_fail, wait_fail, fence_fail, import_fail, missing_extension, combined_image;
static void *expected_wait;
static const char *query(void *d,int n) { (void)d;(void)n; return missing_extension ? "EGL_KHR_image_base_suffix" : combined_image ? "EGL_KHR_image" : "EGL_KHR_image_base EGL_KHR_gl_texture_2D_image EGL_KHR_fence_sync EGL_KHR_wait_sync"; }
static const unsigned char *string(unsigned n) { return (const unsigned char *)(n==0x1f02 ? "OpenGL ES 3.0" : "GL_OES_EGL_image"); }
static void *context(void) { return ctx; }
static void *image(void *d,void *c,unsigned t,void *b,const int *a) { (void)d; assert(c==ctx && t==0x30b1 && b && a[0]==0x30d2); if(image_fail)return NULL; images++;return (void *)(uintptr_t)++id; }
static unsigned del_image(void *d,void *i) { (void)d;assert(i);images--;return 1; }
static void *sync_new(void *d,unsigned t,const int *a) { (void)d;(void)a;assert(t==0x30f9);if(fence_fail)return NULL;fences++;return (void *)(uintptr_t)++id; }
static unsigned sync_del(void *d,void *s) { (void)d;assert(s && fences);fences--;destroyed++;return 1; }
static int wait(void *d,void *s,int flags) { (void)d;assert(s && !flags);if(expected_wait)assert(s==expected_wait);waits++;return !wait_fail; }
static int client(void *d,void *s,int flags,uint64_t time) { (void)d;(void)s;assert(!flags && time==1000000000ULL);return 0x30f5; }
static void get(unsigned n,int *v) { *v = n==0x8069 ? tex : n==0x8caa ? fb : n==0x88ef ? unpack : 0; }
static void gen(int n,uint32_t *t) { assert(n==1);*t=++id; }
static void del(int n,const uint32_t *t) { assert(n==1 && *t); }
static void bind(unsigned t,uint32_t x) { assert(t==0xde1);tex=(int)x; }
static void param(unsigned t,unsigned p,int v) { (void)p;(void)v;assert(t==0xde1); }
static void storage(unsigned t,int l,int fmt,int w,int h,int border,unsigned f,unsigned ty,const void *p) { assert(t==0xde1&&!l&&fmt==0x8058&&w>0&&h>0&&!border&&f==0x1908&&ty==0x1401&&!p&&unpack==0); }
static void copy(unsigned t,int l,int x,int y,int sx,int sy,int w,int h) { assert(t==0xde1&&!l&&!x&&!y&&!sx&&!sy&&w>0&&h>0&&fb==0);copies++;if(injected)errors=injected; }
static void frame(unsigned t,uint32_t v) { assert(t==0x8ca8);fb=(int)v; }
static void buffer(unsigned t,uint32_t v) { assert(t==0x88ec);unpack=(int)v; }
static void target(unsigned t,void *i) { assert(t==0xde1&&i);if(import_fail)errors=0x502; }
static void flush(void) {}
static void finish(void) { finishes++; }
static unsigned error(void) { unsigned e=errors;errors=0;return e; }
static void *resolve(const char *name) {
#define ENTRY(n,f) if(!strcmp(name,n))return (void *)f
    ENTRY("eglQueryString",query);ENTRY("glGetString",string);ENTRY("eglGetCurrentContext",context);
    ENTRY("eglCreateImageKHR",image);ENTRY("eglDestroyImageKHR",del_image);ENTRY("eglCreateSyncKHR",sync_new);
    ENTRY("eglDestroySyncKHR",sync_del);ENTRY("eglWaitSyncKHR",wait);ENTRY("eglClientWaitSyncKHR",client);
    ENTRY("glGetIntegerv",get);ENTRY("glGenTextures",gen);ENTRY("glDeleteTextures",del);ENTRY("glBindTexture",bind);
    ENTRY("glTexParameteri",param);ENTRY("glTexImage2D",storage);ENTRY("glCopyTexSubImage2D",copy);
    ENTRY("glBindFramebuffer",frame);ENTRY("glBindBuffer",buffer);ENTRY("glEGLImageTargetTexture2DOES",target);
    ENTRY("glFlush",flush);ENTRY("glFinish",finish);ENTRY("glGetError",error);
#undef ENTRY
    return NULL; /* ReadPixels/TexSubImage2D are deliberately absent. */
}
static tl_gl_frame *fresh(void) { ctx=(void *)1;return tl_gl_frame_create(resolve,(void *)10); }
int main(void) {
    missing_extension=true;assert(!fresh());missing_extension=false;
    tl_gl_frame *f=fresh();assert(f);
    assert(!tl_gl_frame_publish(f,(void *)3,2420,1668)); /* consumer cannot publish to itself */
    uint32_t t;
    for(int i=0;i<240;i++) {
        ctx=(void *)2; expected_wait=f->read;
        errors=0x500;assert(tl_gl_frame_publish(f,(void *)3,2420,1668));
        assert(tex==7 && fb==9 && unpack==11 && images==1 && fences==1);
        assert(tl_gl_frame_take_guest_error(f)==0x500 && !tl_gl_frame_take_guest_error(f));
        ctx=(void *)1;expected_wait=f->written;
        assert(tl_gl_frame_acquire(f,&t) && t);
        tl_gl_frame_release(f);
        assert(fences==1);
    }
    assert(copies==240 && waits==479 && !finishes);
    puts("PASS: 240 cross-context frames, producer/consumer fences, bounded image/sync ownership, guest GL state/errors retained; no CPU pixels API");
    ctx=(void *)2;expected_wait=f->read;
    assert(tl_gl_frame_publish(f,(void *)3,1668,2420) && images==1);
    ctx=(void *)1;expected_wait=f->written;assert(tl_gl_frame_acquire(f,&t));tl_gl_frame_release(f);
    ctx=(void *)4;assert(!tl_gl_frame_publish(f,(void *)3,1668,2420));
    ctx=(void *)2;assert(!tl_gl_frame_publish(f,(void *)5,1668,2420));
    expected_wait=NULL;tl_gl_frame_destroy(f);assert(!images&&!fences);
    puts("PASS: resize replaces image storage, context/surface mismatch refuses safely and cleanup releases images/fences");
    f=fresh();
    for (int i=0;i<32;i++) {
        ctx=(void *)2;assert(tl_gl_frame_publish(f,(void *)3,64,64));
        uint32_t saved=f->src;
        ctx=(void *)1;tl_gl_frame_reset(f);
        assert(f->src==saved && f->producer==(void *)2 && !images && !fences);
    }
    ctx=(void *)2;tl_gl_frame_reset(f);assert(!f->src);ctx=(void *)1;tl_gl_frame_destroy(f);
    puts("PASS: 32 hide/show cycles from a foreign context preserve source ownership for reclamation");
    f=fresh();ctx=(void *)2;image_fail=true;
    assert(!tl_gl_frame_publish(f,(void *)3,64,64));image_fail=false;tl_gl_frame_destroy(f);
    f=fresh();ctx=(void *)2;injected=0x505;
    assert(!tl_gl_frame_publish(f,(void *)3,64,64));injected=0;tl_gl_frame_destroy(f);
    f=fresh();ctx=(void *)2;assert(tl_gl_frame_publish(f,(void *)3,64,64));
    ctx=(void *)1;wait_fail=true;assert(!tl_gl_frame_acquire(f,&t));wait_fail=false;tl_gl_frame_destroy(f);
    f=fresh();ctx=(void *)2;assert(tl_gl_frame_publish(f,(void *)3,64,64));
    ctx=(void *)1;import_fail=true;assert(!tl_gl_frame_acquire(f,&t));import_fail=false;tl_gl_frame_destroy(f);
    f=fresh();ctx=(void *)2;fence_fail=true;assert(tl_gl_frame_publish(f,(void *)3,64,64));
    ctx=(void *)1;assert(tl_gl_frame_acquire(f,&t));tl_gl_frame_release(f);fence_fail=false;tl_gl_frame_destroy(f);
    assert(finishes==2 && !images && !fences);
    puts("PASS: unsupported image/copy/import/wait errors refuse; fence failure uses real glFinish completion, never fake signalling");
    combined_image=true;f=fresh();ctx=(void *)2;assert(tl_gl_frame_publish(f,(void *)3,64,64));
    ctx=(void *)1;assert(tl_gl_frame_acquire(f,&t));tl_gl_frame_release(f);tl_gl_frame_destroy(f);
    assert(finishes==4 && !images && !fences);
    puts("PASS: combined EGL_KHR_image supported; missing fence extensions use real completion");
}
