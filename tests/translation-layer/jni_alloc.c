/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include "husk-tl-jni-internal.h"
static int fail_at, calls;
static jobj *pending;
static void *fault_calloc(size_t n, size_t bytes) {
    if (++calls == fail_at) return NULL;
    return calloc(n,bytes);
}
void tl_log_line(const char *format, ...) { (void)format; }
void tl_jni_set_pending(jobj *e) { pending=e; }
static uint32_t slots(tl_jclass *cls) { (void)cls; return 12; }
static int stack_reports;
static void report_stack(void) { assert(pending != NULL); stack_reports++; }
static const tl_dvm_hooks hooks = { .instance_slots = slots, .allocation_failure_stack = report_stack };
const tl_dvm_hooks *tl_dvm = &hooks;
#define calloc fault_calloc
#include "production_jni_alloc.inc"
#undef calloc
int main(void) {
    tl_jclass cls={.dvm=(void *)1};
    jobj exception={.kind=TL_K_OBJECT}; g_oom=&exception;
    for (int i=0;i<100;i++) {
        calls=0;fail_at=1;pending=NULL;
        assert(!dvm_new_object(&cls) && pending==g_oom && calls==1);
        calls=0;fail_at=2;pending=NULL;
        assert(!dvm_new_object(&cls) && pending==g_oom && calls==2);
        calls=0;fail_at=0;pending=NULL;
        jobj *o=dvm_new_object(&cls);
        assert(o && o->cls==&cls && o->refs==(1u<<30) && o->nfields==12 && !pending);
        for (unsigned j=0;j<o->nfields;j++) assert(o->fields[j].j==0);
        free(o->fields);free(o);
    }
    assert(stack_reports == 1);
    puts("PASS: 100 object/field allocation failure cycles propagate preallocated OOM without allocating recursively; first failure reports stack once; subsequent zero-filled objects recover");
}
