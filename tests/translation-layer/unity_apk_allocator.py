#!/usr/bin/env python3
"""Execute the original APK's ARM64 reservation routine under Unicorn.

Requires capstone-free pyelftools + unicorn, and a user's local Visual Maimai
Mobile 1.1.1 APK. No game bytes are bundled, modified, or uploaded. This verifies
the allocator ABI/size/trim/error path, NOT Unity startup or Darwin execution.
"""
import argparse
import hashlib
import io
import struct
import zipfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
    UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5,
    UC_ARM64_REG_X8, UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_LR,
    UC_ARM64_REG_CPACR_EL1)

APK_SHA256 = 'a267a2bd1b9e192e28130554c323d42378219d54e9b94a715e1aca8ebe92d712'
BASE, SCRATCH, STACK, STOP = 0x100000000, 0x200000000, 0x300000000, 0x400000000
PAGE, SIZE, ALIGN = 0x4000, 0x10000000, 0x10000000
REGS = [UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5]

def run(data, fail):
    elf = ELFFile(io.BytesIO(data))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    segments = [s for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    end = max(s['p_vaddr']+s['p_memsz'] for s in segments)
    uc.mem_map(BASE, (end+0xfff)&~0xfff)
    for s in segments:
        uc.mem_write(BASE+s['p_vaddr'], s.data())
    uc.mem_map(SCRATCH,0x10000)
    uc.mem_map(STACK,0x10000)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
    uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00)
    uc.reg_write(UC_ARM64_REG_LR,STOP)
    for reg,val in zip(REGS,[PAGE,SIZE//PAGE,ALIGN//PAGE,0,0,SCRATCH]):
        uc.reg_write(reg,val)
    uc.reg_write(UC_ARM64_REG_X8,SCRATCH+0x100)
    maps, trims, calls = [], [], []
    allocated = 0x7000400000  # deliberately page-aligned but not 256 MiB-aligned
    def hook(uc,address,size,user):
        off=address-BASE
        if off not in [0x12c2360,0x12c2040,0x12c2050,0x12c1d20]: return
        args=[uc.reg_read(r) for r in REGS]
        if off==0x12c2360: result=PAGE
        elif off==0x12c1d20: result=SCRATCH+0x200
        elif off==0x12c2040:
            calls.append(args)
            assert args==[0,0x1fffc000,0,0x22,0xffffffff,0], args
            if fail:
                uc.mem_write(SCRATCH+0x200,struct.pack('<I',12))
                result=0xffffffffffffffff
            else:
                maps.append((allocated,allocated+args[1]))
                result=allocated
        else:
            a,n=args[:2]
            assert a%PAGE==0 and n%PAGE==0 and n>0
            assert len(maps)==1
            lo,hi=maps.pop()
            assert lo<=a<a+n<=hi and (a==lo or a+n==hi)
            if a==lo: maps.append((a+n,hi))
            else: maps.append((lo,a))
            trims.append((a,n)); result=0
        uc.reg_write(UC_ARM64_REG_X0,result)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hook)
    uc.emu_start(BASE+0xf000ac,STOP,count=10000)
    assert uc.reg_read(UC_ARM64_REG_PC)==STOP, 'allocator did not return'
    result=struct.unpack('<QQQ',uc.mem_read(SCRATCH+0x100,24))
    assert len(calls)==1
    if fail:
        assert result==(0,0,0) and not trims, result
        assert struct.unpack('<Q',uc.mem_read(SCRATCH+0x18,8))[0]==12
        print('PASS: original ARM64 allocator propagates ENOMEM and returns an empty reservation')
    else:
        assert result[0]%ALIGN==0 and result[0]!=0, result
        assert maps==[(result[0],result[0]+SIZE)], (maps,result)
        assert sum(n for a,n in trims)+SIZE==0x1fffc000
        print(f'PASS: original ARM64 allocator reserves 0x1fffc000, trims to 256 MiB aligned at {result[0]:#x}; result={result}')

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('apk'); args=p.parse_args()
    with open(args.apk,'rb') as f:
        assert hashlib.file_digest(f,'sha256').hexdigest()==APK_SHA256, 'offsets only verified for this exact APK'
    with zipfile.ZipFile(args.apk) as z:
        data=z.read('lib/arm64-v8a/libunity.so')
    run(data,True)
    run(data,False)
