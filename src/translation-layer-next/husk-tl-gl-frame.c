/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-gl-frame.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* EGLImage shares storage without merging the guest and compositor's texture
 * namespaces. Producer and consumer fence each other before reuse. In contrast
 * to glReadPixels -> memcpy -> glTexSubImage2D, only a GPU texture copy occurs.
 * Khronos EGL_KHR_image_base, EGL_KHR_fence_sync, EGL_KHR_wait_sync. */
struct tl_gl_frame {
    void *dpy, *producer, *consumer, *surface, *image, *written, *read;
    uint32_t src, dst;
    int width, height;
    bool imported, failed, async_sync;
    void *error_context;
    unsigned guest_errors;
    const char *(*query)(void *, int);
    const unsigned char *(*string)(unsigned);
    void *(*context)(void);
    void *(*create_image)(void *, void *, unsigned, void *, const int *);
    unsigned (*destroy_image)(void *, void *);
    void *(*create_sync)(void *, unsigned, const int *);
    unsigned (*destroy_sync)(void *, void *);
    int (*wait_sync)(void *, void *, int);
    int (*client_wait)(void *, void *, int, uint64_t);
    void (*get)(unsigned, int *);
    void (*gen)(int, uint32_t *);
    void (*del)(int, const uint32_t *);
    void (*bind)(unsigned, uint32_t);
    void (*param)(unsigned, unsigned, int);
    void (*storage)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*copy)(unsigned, int, int, int, int, int, int, int);
    void (*framebuffer)(unsigned, uint32_t);
    void (*buffer)(unsigned, uint32_t);
    void (*target)(unsigned, void *);
    void (*flush)(void);
    void (*finish)(void);
    unsigned (*error)(void);
};
static bool extension(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return true;
    return false;
}
tl_gl_frame *tl_gl_frame_create(void *(*resolve)(const char *), void *dpy)
{
    tl_gl_frame *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
#define LOAD(member, name) do { *(void **)(&f->member) = resolve(name); if (!f->member) goto fail; } while (0)
    LOAD(query,"eglQueryString"); LOAD(string,"glGetString"); LOAD(context,"eglGetCurrentContext");
    LOAD(create_image,"eglCreateImageKHR"); LOAD(destroy_image,"eglDestroyImageKHR");
    LOAD(create_sync,"eglCreateSyncKHR"); LOAD(destroy_sync,"eglDestroySyncKHR");
    LOAD(wait_sync,"eglWaitSyncKHR"); LOAD(client_wait,"eglClientWaitSyncKHR");
    LOAD(get,"glGetIntegerv"); LOAD(gen,"glGenTextures"); LOAD(del,"glDeleteTextures");
    LOAD(bind,"glBindTexture"); LOAD(param,"glTexParameteri"); LOAD(storage,"glTexImage2D");
    LOAD(copy,"glCopyTexSubImage2D"); LOAD(framebuffer,"glBindFramebuffer"); LOAD(buffer,"glBindBuffer");
    LOAD(target,"glEGLImageTargetTexture2DOES"); LOAD(flush,"glFlush"); LOAD(finish,"glFinish"); LOAD(error,"glGetError");
#undef LOAD
    const char *egl = f->query(dpy,0x3055), *gl = (const char *)f->string(0x1f03);
    /* EGL_KHR_image includes the original texture-2D source specification;
     * Metal advertises that combined extension rather than its split form. */
    if (!(extension(egl,"EGL_KHR_image") || (extension(egl,"EGL_KHR_image_base") &&
          extension(egl,"EGL_KHR_gl_texture_2D_image"))) ||
        !extension(gl,"GL_OES_EGL_image")) goto fail;
    f->async_sync = extension(egl,"EGL_KHR_fence_sync") && extension(egl,"EGL_KHR_wait_sync");
    f->dpy = dpy; f->consumer = f->context();
    if (!f->consumer) goto fail;
    return f;
fail:
    free(f); return NULL;
}
/* Queue a real cross-context wait; never pretend a missing context/fence was
 * signalled. A failed server wait gets a bounded real client wait. Refusal
 * disables this path, leaving the caller's CPU readback fallback available. */
static bool wait_for(tl_gl_frame *f, void **sync)
{
    if (!*sync) return true;
    if (!f->wait_sync(f->dpy,*sync,0) && f->client_wait(f->dpy,*sync,0,1000000000ULL) != 0x30f6) {
        f->failed = true; return false;
    }
    f->destroy_sync(f->dpy,*sync); *sync = NULL;
    return true;
}
static void fence(tl_gl_frame *f, void **sync)
{
    /* The current context is known. If fence creation fails, real completion
     * is required before publishing or allowing another context to overwrite. */
    void *next = f->async_sync ? f->create_sync(f->dpy,0x30f9,NULL) : NULL;
    if (next) f->flush(); else f->finish();
    if (*sync) f->destroy_sync(f->dpy,*sync);
    *sync = next;
}
void tl_gl_frame_reset(tl_gl_frame *f)
{
    if (!f) return;
    if (f->image) f->destroy_image(f->dpy,f->image);
    if (f->written) f->destroy_sync(f->dpy,f->written);
    if (f->read) f->destroy_sync(f->dpy,f->read);
    /* A hide/destroy can arrive on a different thread. Keep the one source
     * handle in that case, reusing it when its own context next publishes.
     * Never delete a guest name in a different namespace or lose a handle on
     * each hide/show cycle. Context replacement safely falls back to pixels. */
    if (f->src && f->context() == f->producer) {
        f->del(1,&f->src); f->src = 0; f->producer = NULL;
    }
    f->surface = f->image = f->written = f->read = NULL;
    f->width = f->height = 0; f->imported = false;
}
bool tl_gl_frame_publish(tl_gl_frame *f, void *surface, int w, int h)
{
    if (!f || f->failed || w <= 0 || h <= 0) return false;
    void *ctx = f->context();
    if (!ctx || ctx == f->consumer || (f->producer && ctx != f->producer) ||
        (f->surface && surface != f->surface)) return false;
    const char *version = (const char *)f->string(0x1f02);
    if (!version || !strstr(version,"OpenGL ES 3.")) return false;
    /* Keep the guest's distinct GL error flags for its next glGetError call,
     * then check our own copy/allocation errors rather than publishing garbage. */
    f->error_context = ctx;
    for (int i = 0; i < 16; i++) {
        unsigned e = f->error();
        if (!e) break;
        if (e >= 0x500 && e <= 0x507) f->guest_errors |= 1u << (e - 0x500);
        if (e == 0x507) { f->failed = true; return false; }
    }
    if (!wait_for(f,&f->read)) return false;
    if (f->width != w || f->height != h) tl_gl_frame_reset(f);
    f->producer = ctx; f->surface = surface;
    int tex = 0, fb = 0, unpack = 0;
    f->get(0x8069,&tex); f->get(0x8caa,&fb); f->get(0x88ef,&unpack);
    bool ok = false;
    if (!f->src) f->gen(1,&f->src);
    f->bind(0x0de1,f->src);
    if (!f->image) {
        f->param(0x0de1,0x2801,0x2601); f->param(0x0de1,0x2800,0x2601);
        f->param(0x0de1,0x2802,0x812f); f->param(0x0de1,0x2803,0x812f);
        f->buffer(0x88ec,0); /* NULL must not be an offset into a guest unpack PBO */
        f->storage(0x0de1,0,0x8058,w,h,0,0x1908,0x1401,NULL);
        f->buffer(0x88ec,(uint32_t)unpack);
        const int attributes[] = {0x30d2,1,0x3038}; /* EGL_IMAGE_PRESERVED_KHR */
        f->image = f->create_image(f->dpy,ctx,0x30b1,(void *)(uintptr_t)f->src,attributes);
        if (!f->image) { f->failed = true; goto done; }
        f->width = w; f->height = h;
    }
    f->framebuffer(0x8ca8,0); /* default read framebuffer, including MSAA resolve */
    f->copy(0x0de1,0,0,0,0,0,w,h);
    if (f->error()) f->failed = true;
    else { fence(f,&f->written); ok = true; }
done:
    /* These errors belong to the bridge, never to the guest. Its earlier
     * error flags were saved above. Do not leak a failed storage allocation. */
    for (int i = 0; i < 16 && f->error(); i++) { }
    f->framebuffer(0x8ca8,(uint32_t)fb);
    f->bind(0x0de1,(uint32_t)tex);
    return ok;
}
unsigned tl_gl_frame_take_guest_error(tl_gl_frame *f)
{
    if (f && f->context() == f->error_context)
        for (unsigned i = 0; i < 8; i++) if (f->guest_errors & (1u << i)) {
            f->guest_errors &= ~(1u << i); return 0x500 + i;
        }
    return 0;
}
bool tl_gl_frame_acquire(tl_gl_frame *f, uint32_t *texture)
{
    if (!f || f->failed || !f->image || f->context() != f->consumer) return false;
    if (!wait_for(f,&f->written)) return false;
    if (!f->dst) f->gen(1,&f->dst);
    if (!f->imported) {
        int old = 0; f->get(0x8069,&old);
        f->bind(0x0de1,f->dst);
        f->target(0x0de1,f->image);
        f->param(0x0de1,0x2801,0x2601); f->param(0x0de1,0x2800,0x2601);
        f->param(0x0de1,0x2802,0x812f); f->param(0x0de1,0x2803,0x812f);
        /* This is Husk's private compositor context; its errors are ours. */
        f->failed = f->error() != 0;
        f->bind(0x0de1,(uint32_t)old);
        if (f->failed) return false;
        f->imported = true;
    }
    *texture = f->dst; return true;
}
void tl_gl_frame_release(tl_gl_frame *f)
{
    if (f && f->context() == f->consumer) fence(f,&f->read);
}
void tl_gl_frame_destroy(tl_gl_frame *f)
{
    if (!f) return;
    tl_gl_frame_reset(f);
    if (f->dst && f->context() == f->consumer) f->del(1,&f->dst);
    free(f);
}
