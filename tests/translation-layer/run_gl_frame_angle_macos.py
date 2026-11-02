#!/usr/bin/env python3
"""Real Metal regression, using pinned public CEF ANGLE host binaries only.

These are test dependencies, not replacements for Husk's iOS ANGLE frameworks.
No APK or user data is needed. The production C bridge is compiled locally.
"""
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[2]
assert platform.system() == 'Darwin' and platform.machine() == 'arm64'
out = root / 'build/angle-host-test'
out.mkdir(parents=True, exist_ok=True)
name = 'cef_binary_156.0.15+gcf671c0+chromium-156.0.8078.80_macosarm64_minimal.tar.bz2'
archive = out / name
if not archive.exists():
    urllib.request.urlretrieve('https://cef-builds.spotifycdn.com/' + name, archive)
assert hashlib.sha1(archive.read_bytes()).hexdigest() == 'bcdd1877edb300b0fb91d8f71c0b14aae256cd0f'
found = set()
with tarfile.open(archive) as tar:
    for member in tar:
        base = Path(member.name).name
        if member.isfile() and base in ('libEGL.dylib', 'libGLESv2.dylib'):
            assert base not in found
            with tar.extractfile(member) as src, (out / base).open('wb') as dst:
                shutil.copyfileobj(src, dst)
            found.add(base)
assert len(found) == 2
bridge = out / 'gl-frame.dylib'
subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-dynamiclib',
                str(root / 'src/translation-layer-next/husk-tl-gl-frame.c'), '-o', str(bridge)], check=True)
for samples in ('0', '4'):
    subprocess.run([sys.executable, str(root / 'tests/translation-layer/gl_frame_angle.py'),
                    str(out), str(bridge), '--backend', 'metal', '--samples', samples], check=True)
