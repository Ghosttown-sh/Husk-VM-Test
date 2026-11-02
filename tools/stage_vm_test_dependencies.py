#!/usr/bin/env python3
"""Stage unchanged, redistributable dependencies from the exact official 3.3 IPA.

The patched app + extensions are still compiled from source. This avoids the
author-local Android/PS4 paths in the clean-build scripts. Never copies Husk's
executable, PlugIns, user data, APKs, signing identities, or provisioning profiles.
"""
import argparse
import bz2
import hashlib
from pathlib import Path, PurePosixPath
import plistlib
import zipfile

SHA256='47cdbac8ba310886bdd74d148ea411298e5cf0dd7bedc7f93d6da810681f695f'
PREFIX='Payload/Husk.app/'
ROOT=Path(__file__).resolve().parents[1]

def stage(ipa):
    with ipa.open('rb') as f:
        actual=hashlib.file_digest(f,'sha256').hexdigest()
    if actual!=SHA256:
        raise ValueError('Not the verified official Husk 3.3 IPA: '+actual)
    def put(path,data):
        target=(ROOT/path).resolve()
        assert target.is_relative_to(ROOT.resolve())
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(data)
    with zipfile.ZipFile(ipa) as z:
        info=plistlib.loads(z.read(PREFIX+'Info.plist'))
        assert info['HuskBuildCommit']=='9345aab' and str(info['CFBundleVersion'])=='148'
        files={
            'Frameworks/libqemu-aarch64-softmmu.dylib':'build/ios-arm64/lib/libqemu-aarch64-softmmu.dylib',
            'Frameworks/libhuskps4.dylib':'build/ios-arm64/lib/libhuskps4.dylib',
            'Frameworks/libANGLE-shared.dylib':'build/ios-arm64/sysroot/lib/libANGLE-shared.dylib',
            'vmlinuz-virt':'guests/phase0/vmlinuz-virt',
            'initramfs-virt':'guests/phase0/initramfs-virt',
        }
        for src,dst in files.items(): put(dst,z.read(PREFIX+src))
        trees={
            'java-runtime/':'build/java-runtime/',
            'android-system/':'build/android-system/',
            'pc-bios/':'third_party/build/qemu-10.0.12-utm/pc-bios/',
            'Frameworks/MoltenVK.framework/':'build/ios-arm64/lib/MoltenVK.xcframework/ios-arm64/MoltenVK.framework/',
        }
        count=0
        for item in z.infolist():
            if item.is_dir() or not item.filename.startswith(PREFIX): continue
            name=item.filename[len(PREFIX):]
            assert '..' not in PurePosixPath(name).parts and not name.startswith('/')
            for src,dst in trees.items():
                if name.startswith(src):
                    put(dst+name[len(src):],z.read(item)); count+=1; break
        put('third_party/build/qemu-10.0.12-utm/pc-bios/edk2-aarch64-code.fd.bz2',
            bz2.compress(z.read(PREFIX+'edk2-aarch64-code.fd')))
        put('build/ios-arm64/lib/MoltenVK.xcframework/Info.plist',plistlib.dumps({
            'CFBundlePackageType':'XFWK','XCFrameworkFormatVersion':'1.0',
            'AvailableLibraries':[{'LibraryIdentifier':'ios-arm64','LibraryPath':'MoltenVK.framework',
                'SupportedArchitectures':['arm64'],'SupportedPlatform':'ios'}]}))
    required=['build/java-runtime/husk-framework.dex','build/android-system/fonts',
              'src/app/Frameworks/StikJIT.xcframework/ios-arm64/StikJIT.framework/StikJIT',
              'src/app/Frameworks/libFilePickerFix.dylib']
    for path in required: assert (ROOT/path).exists(), 'Missing dependency: '+path
    print(f'PASS: official 3.3 SHA256/commit verified; staged {len(files)+count} unchanged runtime files')
    print('No patched app has been compiled by this staging step.')

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('ipa',type=Path); args=p.parse_args()
    stage(args.ipa)
