#!/usr/bin/env python3
"""Pinned, hash-checked Khronos headers for rebuilding QEMU's GPU dependencies.

The release IPA supplies ANGLE binaries but has no C development headers.
These build inputs do not replace or modify the embedded ANGLE runtime.
"""
import hashlib
from pathlib import Path
import subprocess

REVISION = 'e964211ff5f7c2bcada786954a16d5b813379395'
HEADERS = {
    'EGL/egl.h': 'a7c24c828bf2e1a4dfe6511b4f4812b948a285eb54787074015896976d4da076',
    'EGL/eglext.h': 'a5f574a0074001400c6c50232235ee00e9a67b2168bfd1c364ba0f6f52e5a75c',
    'EGL/eglplatform.h': '25b5391655effcf363a38fa00c3154f356a3eab3d35dc067847875513cf1e441',
    'KHR/khrplatform.h': '7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83',
}
root = Path(__file__).resolve().parents[1]
destination = root / 'build/ios-arm64/sysroot/include'
# Use the same GitHub Git transport as the other pinned GPU dependencies.
# Some runners can reach github.com but cannot resolve raw.githubusercontent.com.
checkout = root / 'build/egl-registry-headers'
checkout.mkdir(parents=True, exist_ok=True)
subprocess.run(['git', 'init', '--quiet', str(checkout)], check=True)
subprocess.run(['git', '-C', str(checkout), 'fetch', '--quiet', '--depth=1',
                'https://github.com/KhronosGroup/EGL-Registry.git', REVISION], check=True)
for name, digest in HEADERS.items():
    data = subprocess.check_output(['git', '-C', str(checkout), 'show',
                                    f'{REVISION}:api/{name}'])
    assert hashlib.sha256(data).hexdigest() == digest, name
    path = destination / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
print(f'PASS: {len(HEADERS)} Khronos EGL/KHR development headers pinned to {REVISION} and SHA-256 verified')
