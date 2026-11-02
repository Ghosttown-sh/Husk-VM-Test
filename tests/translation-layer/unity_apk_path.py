#!/usr/bin/env python3
"""Run the user's original Unity ARM64 archive-path parser, without game uploads.

Requires pyelftools and unicorn. This tests a native routine, not a Unity launch.
Offsets are pinned to the exact user-provided APK; no proprietary bytes included.
"""
import argparse
import hashlib
import io
import struct
import zipfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_TPIDR_EL0

APK_SHA256 = 'a267a2bd1b9e192e28130554c323d42378219d54e9b94a715e1aca8ebe92d712'
BASE, DATA, STACK, STOP = 0x100000000, 0x200000000, 0x300000000, 0x400000000

def run(binary, path, lookup=False):
    elf = ELFFile(io.BytesIO(binary))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    segs = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    end = max(s['p_vaddr'] + s['p_memsz'] for s in segs)
    uc.mem_map(BASE, (end + 4095) & ~4095)
    for s in segs:
        uc.mem_write(BASE + s['p_vaddr'], s.data())
    for sec in elf.iter_sections():
        if sec['sh_type'] == 'SHT_RELA':
            for r in sec.iter_relocations():
                if r['r_info_type'] == 1027:  # R_AARCH64_RELATIVE
                    uc.mem_write(BASE + r['r_offset'], struct.pack('<Q', BASE + r['r_addend']))
    uc.mem_map(DATA, 0x10000)
    uc.mem_map(STACK, 0x10000)
    encoded = path.encode()
    uc.mem_write(DATA + 0x100, encoded + b'\0')
    uc.mem_write(DATA, struct.pack('<QQ', DATA + 0x100, len(encoded)))
    uc.reg_write(UC_ARM64_REG_X0, DATA)
    uc.reg_write(UC_ARM64_REG_X1, DATA + 0x20)
    uc.reg_write(UC_ARM64_REG_X2, DATA + 0x40)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff00)
    uc.reg_write(UC_ARM64_REG_LR, STOP)
    uc.reg_write(UC_ARM64_REG_TPIDR_EL0, DATA + 0xf000)
    archive = DATA + 0xe000
    probed = []
    next_string = DATA + 0x4000
    def string(ptr):
        tag = uc.mem_read(ptr + 31, 1)[0]
        if tag < 64:
            return bytes(uc.mem_read(ptr, 31 - tag))
        data, _, n = struct.unpack('<QQQ', uc.mem_read(ptr, 24))
        return bytes(uc.mem_read(data, n))
    if lookup:
        # A registered archive is available. Exercise the real caller's failure
        # branch, with allocation/string ownership and ZIP-entry lookup supplied
        # by the harness; this is still not full engine initialization.
        uc.mem_write(DATA, struct.pack('<QQQ', DATA + 0x100, 0, len(encoded)) + b'\0'*7 + b'\x80' + b'\0'*8)
        uc.reg_write(UC_ARM64_REG_X0, DATA + 0xd000)
        uc.reg_write(UC_ARM64_REG_X1, DATA)
        uc.reg_write(UC_ARM64_REG_X2, 1)
    def strlen(uc, pc, size, unused):
        nonlocal next_string
        off = pc - BASE
        a, b, c = (uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        if off == 0x12c19c0:
            result = 0
            while uc.mem_read(a + result, 1) != b'\0':
                result += 1
        elif lookup and off == 0x467674:
            result = a if uc.mem_read(a + 31, 1)[0] < 64 else struct.unpack('<Q', uc.mem_read(a, 8))[0]
        elif lookup and off == 0x6e22c4:
            result = archive
        elif lookup and off == 0x445c10:
            uc.mem_write(next_string, bytes(uc.mem_read(b, c)) + b'\0')
            uc.mem_write(a, struct.pack('<QQQ', next_string, c + 1, c) + b'\0'*7 + b'\x40')
            next_string += (c + 16) & ~15
            result = a
        elif lookup and off == 0x445ad4:
            uc.mem_write(a, bytes(uc.mem_read(b, 40)))
            result = a
        elif lookup and off == 0x48397c:
            result = 0
        elif lookup and off == 0x6fa27c:
            probed.append(string(b).decode())
            result = int(probed[-1] == 'assets/bin/Data/unity_app_guid')
        else:
            return
        uc.reg_write(UC_ARM64_REG_X0, result)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    uc.hook_add(UC_HOOK_CODE, strlen)
    # 0x6e2d38 calls Unity's actual case-sensitive string search at 0x4b0050.
    uc.emu_start(BASE + (0x6e4418 if lookup else 0x6e2d38), STOP, count=100000)
    assert uc.reg_read(UC_ARM64_REG_PC) == STOP
    if lookup:
        result = uc.reg_read(UC_ARM64_REG_X0)
        assert result in (0, archive)
        assert probed == (['assets/bin/Data/unity_app_guid'] if result else [])
        return bool(result)
    if not uc.reg_read(UC_ARM64_REG_X0):
        return None
    def view(addr):
        ptr, n = struct.unpack('<QQ', uc.mem_read(addr, 16))
        return bytes(uc.mem_read(ptr, n)).decode()
    return view(DATA + 0x20), view(DATA + 0x40)

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('apk')
    args = p.parse_args()
    with open(args.apk, 'rb') as f:
        assert hashlib.file_digest(f, 'sha256').hexdigest() == APK_SHA256
    with zipfile.ZipFile(args.apk) as z:
        binary = z.read('lib/arm64-v8a/libunity.so')
        for entry in ('assets/bin/Data/unity_app_guid', 'assets/bin/Data/Managed/Metadata/global-metadata.dat'):
            data = z.read(entry)  # also checks CRC of the actual compressed entry
            assert data
            print(f'PASS: original APK contains readable {entry} ({len(data)} bytes)')
    prefix = '/private/var/mobile/Containers/Data/Application/' + 'A'*36 + '/Documents/TranslationLayer/' + 'B'*36
    entry = 'assets/bin/Data/unity_app_guid'
    original = prefix + '/Visual Maimai Mobile release 1.1.1.aPK'
    assert run(binary, original + '/' + entry) is None
    assert not run(binary, original + '/' + entry, lookup=True)
    print('REPRODUCED: original ARM64 parser rejects .aPK; its real archive-lookup caller returns NULL before probing the entry')
    for name in ('Visual Maimai Mobile release 1.1.1.apk', 'java-data/.husk-package/base.apk'):
        apk = prefix + '/' + name
        assert run(binary, apk + '/' + entry) == (apk, entry)
        assert run(binary, apk + '/' + entry, lookup=True)
        print(f'PASS: original ARM64 parser accepts {name}; real lookup caller probes the correct entry and returns the registered archive')
