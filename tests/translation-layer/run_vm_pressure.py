#!/usr/bin/env python3
"""Compile the actual reservation and b_mmap functions against a bounded VM.

This is a deterministic policy regression, not an iPad kernel emulator. Darwin
integration is covered separately by run_vm_pressure_macos.sh. Extracting these
two self-contained regions avoids changing production layout for a Windows test.
Use --source with the original bionic-io.c to demonstrate the regression.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=root / 'src/translation-layer-next/husk-tl-bionic-io.c')
p.add_argument('--cc', default='cc')
p.add_argument('--msvc', action='store_true')
args = p.parse_args()
source = args.source.read_text()
reservation = source[source.index('#define PHANTOM_HALF'):source.index('/* Whether only the JIT region')]
mmap = source[source.index('static void *b_mmap('):source.index('static int b_munmap(')]
errno_h = (root / 'src/translation-layer-next/husk-tl-bionic.h').read_text()
errno_defs = '\n'.join(line for line in errno_h.splitlines() if line.startswith('#define TL_ERRNO_'))
work = (root / 'build/vm-pressure').resolve()
work.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='husk-vm-', dir=work) as tmp:
    out = Path(tmp)
    assert out.resolve().is_relative_to(work)
    (out / 'production_mm.inc').write_text(errno_defs + '\n' + reservation + '\n' + mmap)
    shutil.copyfile(Path(__file__).with_name('vm_pressure.c'), out / 'test.c')
    exe = out / ('test.exe' if args.msvc else 'test')
    if args.msvc:
        command = [args.cc, '/nologo', '/std:c11', '/W4', '/wd4100', '/wd4127', '/wd4189', '/wd4244', '/wd4996', str(out/'test.c'), '/Fe:'+str(exe)]
    else:
        command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-variable', str(out/'test.c'), '-o', str(exe)]
    subprocess.run(command, cwd=out, check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([str(exe)], env=dict(os.environ, TL_MM_MAX_GIB='0'), check=True)
