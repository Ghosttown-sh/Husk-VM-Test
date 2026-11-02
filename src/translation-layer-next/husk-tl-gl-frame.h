/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_GL_FRAME_H
#define HUSK_TL_GL_FRAME_H
#include <stdbool.h>
#include <stdint.h>
typedef struct tl_gl_frame tl_gl_frame;
/* Host-ABI entry points, NOT guest GL adapters. Caller serializes publish,
 * acquire/draw/release and reset with its compositor lock. Contexts need not
 * share object namespaces. No framebuffer pixels cross the CPU on this path. */
tl_gl_frame *tl_gl_frame_create(void *(*resolve)(const char *), void *display);
bool tl_gl_frame_publish(tl_gl_frame *, void *surface, int width, int height);
bool tl_gl_frame_acquire(tl_gl_frame *, uint32_t *texture);
void tl_gl_frame_release(tl_gl_frame *);
void tl_gl_frame_reset(tl_gl_frame *);
void tl_gl_frame_destroy(tl_gl_frame *);
unsigned tl_gl_frame_take_guest_error(tl_gl_frame *);
#endif
