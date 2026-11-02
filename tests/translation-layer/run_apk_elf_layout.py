#!/usr/bin/env python3
"""Compile and run the production ELF analyzer on locally supplied APK libraries."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import zipfile

root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(); p.add_argument('apk',type=Path)
p.add_argument('--cc',default='cc'); p.add_argument('--msvc',action='store_true')
a=p.parse_args()
work=(root/'build/apk-layout').resolve(); work.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=work) as tmp:
    out=Path(tmp); assert out.resolve().is_relative_to(work)
    exe=out/('analyze.exe' if a.msvc else 'analyze')
    sources=[str(root/'tests/translation-layer/apk_elf_layout.c'),str(root/'src/translation-layer/husk-tl-elf.c')]
    if a.msvc:
        # The shared header declares a window mutex unused by this analyzer.
        (out/'pthread.h').write_text('typedef int pthread_mutex_t;\n')
        cmd=[a.cc,'/nologo','/std:c11','/experimental:c11atomics','/I'+str(out),
             '/I'+str(root/'src/translation-layer'),*sources,'/Fe:'+str(exe)]
    else:
        cmd=[a.cc,'-std=c11','-I'+str(root/'src/translation-layer'),*sources,'-o',str(exe)]
    subprocess.run(cmd,cwd=out,check=True)
    files=[]
    with zipfile.ZipFile(a.apk) as z:
        for name in z.namelist():
            if name.startswith('lib/arm64-v8a/') and name.endswith('.so'):
                f=out/Path(name).name; f.write_bytes(z.read(name)); files.append(str(f))
    assert files, 'APK contains no ARM64 libraries'
    subprocess.run([str(exe),*files],check=True)
