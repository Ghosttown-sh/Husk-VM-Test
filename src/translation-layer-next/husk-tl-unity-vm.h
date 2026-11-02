/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_UNITY_VM_H
#define HUSK_TL_UNITY_VM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Baselib ARM64 page allocation result, returned indirectly through x8. */
typedef struct { void *address; size_t page_size, page_count; } tl_unity_pages;
typedef tl_unity_pages (*tl_unity_page_fn)(size_t, size_t, size_t, uint32_t, uint32_t, void *);
tl_unity_pages tl_unity_vm_allocate(size_t page, size_t count, size_t alignment_pages,
                                   uint32_t state, uint32_t flags, void *error);
bool tl_unity_vm_redirect(uint8_t *site_rw, uint8_t *site_rx, uint8_t *stub_rw, uint8_t *stub_rx);
#endif
