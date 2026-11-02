#!/usr/bin/env python3
"""Extract the unchanged production VM implementation for native Mac tests.

The complete translation unit also implements ARM64 signal contexts, which an
Intel Mac cannot compile. Keep the VM code and all its preceding dependencies
verbatim; exclude the unrelated time/signals section. Host mmap, Mach calls,
pthread ownership locks, trimming and protection stay real in these tests.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
path = root / 'src/translation-layer-next/husk-tl-bionic-io.c'
source = path.read_text()
boundary = '/* ------------------------------------------------------------------- time */'
assert source.count(boundary) == 1, 'Production section boundary changed; review test extraction'
vm = source[:source.index(boundary)]
for required in ('static void *b_mmap(', 'static int b_munmap(', 'static int b_mprotect('):
    assert required in vm, f'Production function missing: {required}'
Path(sys.argv[1]).write_text('#line 1 "husk-tl-bionic-io.c"\n' + vm)
