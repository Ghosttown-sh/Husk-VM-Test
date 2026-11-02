#!/usr/bin/env python3
"""Structural/device-target validation; never a substitute for an iPad launch."""
import argparse
import hashlib
import plistlib
import struct
import zipfile

def device_arm64(data, name):
    magic=struct.unpack_from('<I',data)[0]
    if magic==0xbebafeca:  # universal Mach-O (big endian fat header)
        count=struct.unpack_from('>I',data,4)[0]
        slices=[]
        for i in range(count):
            cpu,sub,off,size,align=struct.unpack_from('>IIIII',data,8+20*i)
            if cpu==0x100000c: slices.append(data[off:off+size])
        assert slices, f'{name}: no arm64 slice'
        for item in slices: device_arm64(item,name)
        return
    assert magic==0xfeedfacf, f'{name}: not 64-bit Mach-O'
    cpu,sub,kind,ncmds,sizeofcmds,flags,reserved=struct.unpack_from('<7I',data,4)
    assert cpu==0x100000c, f'{name}: not arm64'
    pos=32; ios=False
    for i in range(ncmds):
        cmd,size=struct.unpack_from('<II',data,pos)
        assert size>=8 and pos+size<=len(data)
        if cmd==0x32:
            platform=struct.unpack_from('<I',data,pos+8)[0]
            assert platform==2, f'{name}: build platform {platform}, expected iOS device (2)'
            ios=True
        elif cmd==0x25: ios=True  # LC_VERSION_MIN_IPHONEOS
        elif cmd in (0x24,0x2f,0x30): raise AssertionError(f'{name}: non-iOS target')
        pos+=size
    assert ios, f'{name}: missing iOS device load command'
    assert pos==32+sizeofcmds

def validate(path,commit):
    with zipfile.ZipFile(path) as z:
        assert z.testzip() is None, 'ZIP CRC failure'
        root='Payload/Husk.app/'
        names=set(z.namelist())
        info=plistlib.loads(z.read(root+'Info.plist'))
        assert info['CFBundleIdentifier']=='com.husk.vmtest'
        assert info['CFBundleDisplayName']=='Husk VM Test'
        for name in names:
            if name.startswith(root) and name[len(root):].count('/') == 1 and name.endswith('.lproj/InfoPlist.strings'):
                localized=plistlib.loads(z.read(name))
                assert localized.get('CFBundleDisplayName')=='Husk VM Test', f'{name}: test name is hidden by localization'
                assert localized.get('CFBundleName')=='Husk VM Test', f'{name}: wrong localized bundle name'
        assert 2 in info['UIDeviceFamily'] and 'iPhoneOS' in info['CFBundleSupportedPlatforms']
        assert info['HuskBuildCommit']==commit, (info.get('HuskBuildCommit'),commit)
        schemes=[s for t in info['CFBundleURLTypes'] for s in t['CFBundleURLSchemes']]
        assert schemes==['husk-vmtest']
        executables=[root+info['CFBundleExecutable']]
        app_code=z.read(executables[0])
        assert b'mm: released unused 4 GiB compressed heap; retry' in app_code, 'Main executable lacks the VM recovery implementation diagnostic'
        assert b'javaapp: installed base APK: %s' in app_code, 'Main executable lacks installed APK path normalization'
        assert b'mm: reserved real VM gap addr=' in app_code, 'Main executable lacks real VM gap reservation'
        assert b'unity-vm: verified Baselib allocator uses native aligned reservations' in app_code, 'Main executable lacks verified native aligned allocator bridge'
        assert b'egl: native-window GPU image compositor' in app_code, 'Main executable lacks native GPU compositor bridge'
        assert b'mm: delegated VM range addr=' in app_code, 'Main executable lacks kernel-approved VM range delegation'
        assert b'mm: state[%s] footprint=' in app_code, 'Main executable lacks VM footprint diagnostics'
        assert b'Could not prepare RAM for the Android snapshot. See the Husk log.' in app_code, 'Main executable lacks the corrected snapshot RAM failure path'
        required=['Frameworks/libqemu-aarch64-softmmu.dylib','Frameworks/libANGLE-shared.dylib',
                  'Frameworks/libhuskps4.dylib','Frameworks/libFilePickerFix.dylib',
                  'Frameworks/MoltenVK.framework/MoltenVK','Frameworks/StikJIT.framework/StikJIT',
                  'java-runtime/husk-framework.dex','vmlinuz-virt','initramfs-virt',
                  'edk2-aarch64-code.fd','lineage-efi-vars-seed.fd','lineage-vdb-seed.qcow2','husk-jit.js']
        for name in required: assert root+name in names, 'Missing '+name
        extensions=[]
        for name in names:
            if name.startswith(root+'PlugIns/') and name.endswith('.appex/Info.plist'):
                ex=plistlib.loads(z.read(name))
                assert ex['CFBundleIdentifier'].startswith('com.husk.vmtest.')
                extensions.append(ex['CFBundleIdentifier'])
                executables.append(name[:-len('Info.plist')]+ex['CFBundleExecutable'])
        assert set(extensions)=={'com.husk.vmtest.JITHelper','com.husk.vmtest.Downloads'}
        executables.extend(root+s for s in required if s.startswith('Frameworks/'))
        for name in executables: device_arm64(z.read(name),name)
        assert not any(n.endswith('.apk') and '/java-runtime/' not in n and '/android-system/' not in n for n in names)
    with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
    print(f'PASS: {len(executables)} device arm64 Mach-O binaries, iPad family, test IDs, resources, ZIP CRC; commit {commit}')
    print('SHA256 '+digest)
    print('Structural validation only. Physical iPad launch/editor/touch/save remain unverified.')

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('ipa'); p.add_argument('--commit',required=True)
    a=p.parse_args(); validate(a.ipa,a.commit)
