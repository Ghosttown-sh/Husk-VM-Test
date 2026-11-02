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
name = 'cef_binary_150.0.21+ga61e9a5+chromium-150.0.7871.255_macosarm64_minimal.tar.bz2'
archive = out / name
if not archive.exists():
    urllib.request.urlretrieve('https://cef-builds.spotifycdn.com/' + name, archive)
assert hashlib.sha1(archive.read_bytes()).hexdigest() == '4057b6558bc83c93eb955b6983e00ef6361a0ffc'
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
for source, flags, obj in (
    ('husk-tl-gl-frame.c', ['-std=c11'], 'frame.o'),
    ('husk-tl-metal-frame.m', ['-fobjc-arc'], 'metal.o'),
):
    subprocess.run(['clang', *flags, '-Wall', '-Wextra', '-Werror', '-O2', '-g', '-c',
                    str(root / 'src/translation-layer-next' / source), '-o', str(out / obj)], check=True)
subprocess.run(['clang', '-dynamiclib', str(out / 'frame.o'), str(out / 'metal.o'),
                '-framework', 'Metal', '-framework', 'Foundation', '-o', str(bridge)], check=True)
for samples in ('0', '4'):
    command = [sys.executable, '-u', str(root / 'tests/translation-layer/gl_frame_angle.py'),
               str(out), str(bridge), '--backend', 'metal', '--samples', samples]
    result = subprocess.run(command, timeout=180)
    if result.returncode < 0:
        subprocess.run(['lldb','--batch','-o','settings set target.process.stop-on-exec false',
                        '-o','run','-o','thread backtrace all','--',*command], timeout=60)
    result.check_returncode()
