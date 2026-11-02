#!/usr/bin/env python3
"""Local-only ARM64 regression with the user's unchanged APK.

Consumes the redirect/trampoline emitted by the production C installer in the
Mac test. Unicorn models the host callback; actual Mach mapping and the C
callback are tested separately by vm_aligned_macos.c. No APK bytes are shipped.
"""
import argparse
import hashlib
import io
from pathlib import Path
import re
import struct
import zipfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from unity_apk_allocator import APK_SHA256, BASE, SCRATCH, STACK, STOP, PAGE, SIZE, ALIGN, REGS

ENTRY, END, STUB, HOST = 0xf000ac, 0xf006b4, 0x1600000, 0x500000000
ROOT = Path(__file__).resolve().parents[2]

def run(data, emitted, args, patched, host_success, initial_error=0):
    elf = ELFFile(io.BytesIO(data))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    segments = [s for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    end = max(s['p_vaddr']+s['p_memsz'] for s in segments)
    uc.mem_map(BASE,(end+0xfff)&~0xfff)
    for s in segments: uc.mem_write(BASE+s['p_vaddr'],s.data())
    uc.mem_map(BASE+STUB,0x1000)
    uc.mem_map(HOST,0x1000)
    uc.mem_map(SCRATCH,0x10000)
    uc.mem_map(STACK,0x10000)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
    uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00)
    uc.reg_write(UC_ARM64_REG_LR,STOP)
    for reg,val in zip(REGS,args+[SCRATCH]): uc.reg_write(reg,val)
    uc.reg_write(UC_ARM64_REG_X8,SCRATCH+0x100)
    uc.mem_write(SCRATCH+40,struct.pack('<I',initial_error))
    saved = list(range(UC_ARM64_REG_X19,UC_ARM64_REG_X28+1))
    # Unicorn's register enum is sequential for x19..x28.
    for i,reg in enumerate(saved): uc.reg_write(reg,0x98760000+i)
    if patched:
        # Rebase just the host function pointer for the emulation address map.
        uc.mem_write(BASE+ENTRY,emitted[:8]+struct.pack('<Q',HOST))
        uc.mem_write(BASE+STUB,emitted[16:])
    trace=[]
    def hook(uc,address,size,user):
        off=address-BASE
        if address==HOST:
            values=[uc.reg_read(r) for r in REGS]
            assert values==args+[SCRATCH], values
            assert uc.reg_read(UC_ARM64_REG_X8)==SCRATCH+0x100
            page,count,alignment,state,flags=args
            valid=(page==PAGE and count and alignment and not alignment&(alignment-1)
                   and count<=((1<<64)-1)//page and alignment<=((1<<64)-1)//page
                   and not state and not flags and not initial_error)
            if valid and host_success:
                # A real native test supplies only 256 MiB holes, never 512 MiB.
                assert count*page<=SIZE
                uc.mem_write(SCRATCH+0x100,struct.pack('<QQQ',0x70f0000000,page,count))
                uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
            else: uc.reg_write(UC_ARM64_REG_PC,BASE+STUB)
            return
        if off not in [0x12c2360,0x12c2040,0x12c2050,0x12c1d20]: return
        values=[uc.reg_read(r) for r in REGS]
        if off==0x12c2360: result=PAGE
        elif off==0x12c1d20: result=SCRATCH+0x200
        elif off==0x12c2040:
            trace.append(('mmap',values))
            uc.mem_write(SCRATCH+0x200,struct.pack('<I',12))
            result=(1<<64)-1
        else:
            trace.append(('munmap',values[:2])); result=0
        uc.reg_write(UC_ARM64_REG_X0,result)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hook)
    uc.emu_start(BASE+ENTRY,STOP,count=10000)
    assert uc.reg_read(UC_ARM64_REG_PC)==STOP
    assert uc.reg_read(UC_ARM64_REG_SP)==STACK+0xff00
    for i,reg in enumerate(saved): assert uc.reg_read(reg)==0x98760000+i, reg
    return bytes(uc.mem_read(SCRATCH+0x100,24)),bytes(uc.mem_read(SCRATCH,48)),trace

def main():
    p=argparse.ArgumentParser(); p.add_argument('apk',type=Path); p.add_argument('redirect',type=Path)
    a=p.parse_args()
    with a.apk.open('rb') as f: assert hashlib.file_digest(f,'sha256').hexdigest()==APK_SHA256
    with zipfile.ZipFile(a.apk) as z: data=z.read('lib/arm64-v8a/libunity.so')
    elf=ELFFile(io.BytesIO(data))
    code=next(s.data()[ENTRY-s['p_vaddr']:END-s['p_vaddr']] for s in elf.iter_segments()
              if s['p_type']=='PT_LOAD' and s['p_vaddr']<=ENTRY and END<=s['p_vaddr']+s['p_filesz'])
    loader=(ROOT/'src/translation-layer-next/husk-tl-ld.c').read_text(encoding='utf-8')
    match=re.search(r'const uint8_t digest\[32\] = \{([^}]+)\}',loader)
    expected=bytes(int(x,16) for x in re.findall(r'0x[0-9a-f]+',match[1]))
    assert hashlib.sha256(code).digest()==expected
    assert hashlib.sha256(bytes([code[0]^1])+code[1:]).digest()!=expected
    emitted=a.redirect.read_bytes(); assert len(emitted)==48 and emitted[16:32]==code[:16]
    args=[PAGE,SIZE//PAGE,ALIGN//PAGE,0,0]
    baseline=run(data,emitted,args,False,False)
    assert baseline[0]==bytes(24) and baseline[2][0][1][1]==0x1fffc000
    good=run(data,emitted,args,True,True)
    assert struct.unpack('<QQQ',good[0])==(0x70f0000000,PAGE,SIZE//PAGE)
    assert good[1]==bytes(48) and not good[2]
    print('PASS: exact APK allocator+validator fingerprint; native-emitted redirect returns full aligned 256 MiB result without temporary mmap/munmap')
    cases=[args,[PAGE,0,1,0,0],[PAGE,1,3,0,0],[4096,1,1,0,0],
           [PAGE,1,1,1,0],[PAGE,1,1,0,1],[PAGE,(1<<64)-1,1,0,0],
           [PAGE,1,1<<63,0,0]]
    for case in cases:
        assert run(data,emitted,case,True,False)==run(data,emitted,case,False,False),case
    assert run(data,emitted,args,True,True,7)==run(data,emitted,args,False,False,7)
    print('PASS: original ARM64 fallback has identical result, error bytes and syscall trace for exhaustion, unsupported/invalid parameters and existing errors')
    print('PASS: ARM64 x8 struct result, x0-x5 arguments, stack and callee-saved registers preserved')
    print('Not a Unity startup or iPad execution test. The original APK SHA256 remains unchanged.')

if __name__=='__main__': main()
