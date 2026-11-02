#!/usr/bin/env python3
"""Compile the production object allocation path with injected calloc failures."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
jni = (root / 'src/translation-layer-next/husk-tl-jni.c').read_text()
dvm = (root / 'src/translation-layer-next/husk-tl-dvm.c').read_text()
def function(text, name):
    start = text.index(name)
    start = text.rfind('\n', 0, start) + 1
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'

code = 'static jobj *g_oom;\n'
for name in ('tl_jni_allocation_failed(', 'obj_alloc(', 'tl_jni_new_object('):
    code += function(jni, name)
code += function(dvm, 'dvm_new_object(')
# The opcode must route NULL to exception dispatch, not dereference/publish it.
op = dvm[dvm.index('case 0x22:'):dvm.index('case 0x23:')]
assert 'if (!o) goto exception;' in op
Path(sys.argv[1]).write_text(code)
