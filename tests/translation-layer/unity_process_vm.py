#!/usr/bin/env python3
"""Execute the supplied APK's unmodified libunwindstack read routine.

Only OS/libc boundaries are intercepted. Demonstrates why ENOSYS is different
from EFAULT; native Darwin compatibility behavior is tested separately. Requires
the user's local APK, pyelftools and Unicorn. No game bytes are distributed.
"""
import argparse
import hashlib
import io
import struct
import zipfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_LR

SHA = 'a267a2bd1b9e192e28130554c323d42378219d54e9b94a715e1aca8ebe92d712'
BASE, DATA, STACK, STOP = 0x100000000, 0x200000000, 0x300000000, 0x400000000
REGS = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6]

def run(code, syscall_errno, readable):
    e = ELFFile(io.BytesIO(code))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    segments = [s for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    end = max(s['p_vaddr'] + s['p_memsz'] for s in segments)
    uc.mem_map(BASE, (end + 4095) & ~4095)
    for s in segments: uc.mem_write(BASE + s['p_vaddr'], s.data())
    uc.mem_map(DATA, 0x10000); uc.mem_map(STACK, 0x10000)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff00); uc.reg_write(UC_ARM64_REG_LR, STOP)
    src = DATA + 0x8000 if readable else 0x800000000
    uc.mem_write(DATA + 0x8000, b'test')
    for reg, val in zip(REGS, [1234, src, DATA + 0x4000, 4]): uc.reg_write(reg, val)
    calls = []
    unsafe = []
    def hook(uc, address, size, _):
        off = address - BASE
        if off not in (0x12c2360, 0x12c1e50, 0x12c1d20, 0x12c1b80, 0x12c19d0): return
        a = [uc.reg_read(r) for r in REGS]
        if off == 0x12c2360: result = 16384
        elif off == 0x12c1d20: result = DATA
        elif off == 0x12c1b80: result = 1234
        elif off == 0x12c1e50:
            calls.append(a)
            assert a[0:2] == [270, 1234] and a[3] == a[5] == 1 and a[6] == 0
            assert struct.unpack('<QQ', uc.mem_read(a[2],16)) == (DATA + 0x4000,4)
            assert struct.unpack('<QQ', uc.mem_read(a[4],16)) == (src,4)
            uc.mem_write(DATA,struct.pack('<I',syscall_errno))
            result = 0xffffffffffffffff
        else:
            unsafe.append(a[:3])
            uc.mem_write(a[0],bytes(uc.mem_read(a[1],a[2])))
            result = a[0]
        uc.reg_write(UC_ARM64_REG_X0,result)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hook)
    failed = False
    try: uc.emu_start(BASE + 0xa6b1a0, STOP, count=10000)
    except UcError:
        failed = True
        assert syscall_errno == 38 and not readable and unsafe
    assert len(calls) == 1
    if syscall_errno == 38:
        assert unsafe and failed == (not readable)
        if readable: assert uc.reg_read(UC_ARM64_REG_X0) == 4
    else:
        assert not unsafe and not failed and uc.reg_read(UC_ARM64_REG_X0) == 0
    print(f'PASS: original Unity libunwindstack: errno={syscall_errno}, readable={readable}, unsafe memcpy={bool(unsafe)}, secondary fault={failed}')

if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('apk'); args = p.parse_args()
    with open(args.apk,'rb') as f: assert hashlib.file_digest(f,'sha256').hexdigest() == SHA
    with zipfile.ZipFile(args.apk) as z: code = z.read('lib/arm64-v8a/libunity.so')
    run(code,38,True); run(code,38,False); run(code,14,False)
