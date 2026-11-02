/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_VM_H
#define HUSK_TL_VM_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

/* Real, initially inaccessible anonymous memory. Never overwrites a mapping.
 * Used only after a non-fixed private anonymous reservation returns ENOMEM. */
void *tl_vm_reserve_gap(size_t size);
void tl_vm_log_state(const char *reason);
/* Linux raw-syscall convention: byte count or negative Linux errno. */
long tl_vm_process_readv(pid_t pid, const struct iovec *local, unsigned long nl,
                        const struct iovec *remote, unsigned long nr, unsigned long flags);
#endif
