# Visual Maimai Mobile: VM investigation and test build

Status on 2026-11-02: source fixes implemented; Windows, Linux CI and native
macOS regression tests passed, including real Darwin mappings with both 4 KiB
and 16 KiB host pages. A real ARM64 iOS device IPA was built successfully on
Apple Silicon with Xcode 27.1, structurally validated and signature-verified
on the Mac, then downloaded and independently revalidated on Windows.
The user's 19:44:49Z physical iPad run confirms that the memory fix succeeded:
the exact 0x1fffc000 retry returned 0x7000000000 with errno=0. Unity reached
five graphics swaps, then crashed during resource/IL2CPP initialization.
Complete Unity startup and physical iPad editor/touch/save remain unverified.

Current follow-up IPA: [Actions run 41514373867](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41514373867),
source `90b1e42068da4289f968d10457c1a0a8b2b4c542`, build stamp
`90b1e42-dirty`, 142,245,606 bytes. SHA256:
`1524621f2e9a0df785b9dca2f7b40d09c98d5a24b292baf2b71e51fe78395689`.
The source fix, native filesystem tests, 16 KiB Darwin memory tests, full
ARM64 iOS compilation and signature validation passed on Apple Silicon.
Windows independently validated all nine device binaries, identity, resources,
ZIP CRC and the new code diagnostic. Linux filesystem and memory tests passed
in run 41514378587. This follow-up IPA has not yet run on the physical iPad.

Earlier memory-fix build: [Actions run 41507991694](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41507991694).
The source revision is `10a4cd0e44a538958591d827217877d5e4df874e`; the IPA's
build stamp is `10a4cd0-dirty` (the build regenerates tracked Xcode project
and dependency-notice files). Later documentation commits do not change this
IPA's executable. File: `Husk-VM-Test.ipa`, 142,243,910 bytes. SHA256:
`e0720ca4c119a6ba29279b2eb229d68f5724e5ea7819020e2d8900f96806d92b`.

## Reproducible source and inputs

- Husk base: `9345aab`, version 3.3 (148), from <https://github.com/Leviidev/Husk>.
- Working branch: `fix/android-vm-reservations`.
- User's device: iPad17,1 / M5 / iPadOS 27.0. Existing JIT self-test passes.
- Local APK: `Visual Maimai Mobile release 1.1.1.aPK`, 133,937,775 bytes.
- APK SHA256: `a267a2bd1b9e192e28130554c323d42378219d54e9b94a715e1aca8ebe92d712`.
- Package supplied by user: `com.CH3COOOH.VisualMaimaiMobile`.
- Both `libunity.so` and `libil2cpp.so` contain Unity version `6000.0.10f1`.
- No APK/game binary, extracted game library, private credential, or user data
  is included in the commits or delivery archive. The APK was read locally.

## The exact allocation

The ARM64 reservation routine in this APK's `libunity.so` starts at ELF virtual
address `0xf000ac`. Its call at `0xf00258` targets the `mmap` PLT entry at
`0x12c2040`. For a 256 MiB reservation with 256 MiB alignment and 16 KiB pages,
it calls:

```c
mmap(NULL, 0x1fffc000, PROT_NONE, 0x22, -1, 0);
// 0x4000 * (0x4000 + 0x4000 - 1): size + alignment - one page
```

The routine aligns the returned pointer and unmaps the unused prefix/suffix.
`0x1fffc000` is 536,854,528 bytes, already a multiple of the host's 16 KiB
page size. This is a virtual reservation; the request does not touch 512 MiB
of RAM. The 4 KiB ELF segment alignment is a separate issue from this call.

Husk's `husk-tl-bionic-io.c:b_mmap` correctly converts Android `0x22` to
Darwin `MAP_PRIVATE | MAP_ANON`; `PROT_NONE` stays zero. The fd becomes -1,
offset remains zero, and no `MAP_FIXED`, `MAP_JIT`, or `PROT_EXEC` is added.
This path reaches host `mmap` directly. Its special compressed-heap fallback
only recognizes 8 GiB (or 8 GiB minus one host page), so it cannot service
Unity's request. `TL_MM_MAX_GIB` is a test-only override; the supplied log
does not show its explicit refusal message.

The native routine was executed under Unicorn using the original APK bytes
and intercepted OS calls. It issued the exact arguments above, propagated
ENOMEM into an empty result, and on a successful mock OS reservation trimmed
both ends to a contiguous 256 MiB aligned range. This executes the
allocator instructions; it does **not** execute the full Unity engine or
Darwin's VM implementation.

## Identified defect and limits of the diagnosis

Commit `e49ddfd` introduced an early, aligned 4 GiB `vm_map(PROT_NONE)` for
Dart/V8. `JITBootstrap.prewarm()` calls `husk_prepare_compressed_heap()` before
JIT allocation. On successful prewarming this reservation is retained. Only
`phantom_reserve()` consumes it, and only a failed prewarm releases it.
Unity does not request that cage, so all four GiB can remain unused while
Unity allocates its own heaps.

The loader (`husk-tl-ld.c`) carves native images/stubs from `tl_xmem_alloc`.
`husk-tl-xmem.c` adopts the prewarmed RX/RW views; `near_alias()` can also
create a nearer RW alias. These occupy virtual addresses even when pages
are shared. The unused cage, aliases, other libraries, and allocator mappings
can leave no sufficiently large contiguous gap in the applicable VM range.

Apple's published XNU maps `KERN_NO_SPACE` and `KERN_INVALID_ADDRESS` from
the mapping call to `ENOMEM`; it separately selects VM allocation ranges.
Thus free physical RAM and a high returned address do not establish that a
particular reservation fits. See [the mmap error translation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_mman.c)
and [VM range selection](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c).
These public sources establish the mechanism, not the private VM map of the
user's particular iPadOS 27 process.

The source defect is reproduced with the **actual production C functions** in
a bounded 6 GiB VM model: an unused 4 GiB cage and three live ranges leave
separated 256 MiB holes. The exact Unity request fails in unmodified
`9345aab`, and succeeds with the patch. The range sizes are test fixtures,
not new limits in the app.

The initial log lacked the process VM map. The subsequent physical iPad run
of 10a4cd0-dirty now confirms this recovery on the user's device: releasing
the unclaimed cage made the identical failed reservation succeed. This is
not proof that every possible ENOMEM has this cause, nor that Unity startup
is complete; the next failure is described below.

## Primary fix and compatibility boundaries

After an ordinary private anonymous `PROT_NONE` reservation fails with
ENOMEM, `b_mmap` releases the still-unclaimed cage under its ownership mutex
and retries the identical mapping once. It does not retry loader/test
refusals, fixed/shared/file mappings, other errors, or compressed-heap
requests. A cage already transferred to a guest cannot be released through
this path. Retry errors are preserved across logging and translated through
the existing guest errno path.

No size limit, entitlement, JIT mapping size, Unity setting, APK instruction,
or crash signal handler was changed. The returned mapping has the entire
requested span. Unity's subsequent `munmap` trims and `mprotect` commits keep
their existing behavior.

The broader compatibility layer is not a complete Linux VM implementation:
the existing V8 phantom reservation deliberately represents a larger range
than is mapped, shifted file mappings have special handling, and `mremap`
does not implement every Linux flag. Those paths were inspected but were not
rewritten or represented as newly conformant by this fix. Unity's failing
request uses none of them.

## The `0x50` fault and other warnings

The inspected reservation routine explicitly tests `MAP_FAILED` and returns
an empty result; it does not itself dereference `0x50` on that failure path.
A higher-level null-object access after allocation failure is consistent
with the reported fault, but an exact instruction cannot be attributed from
the fault address alone. Husk's existing crash reporter prints PC, LR,
frames and registers after the supplied fatal line. Those lines are needed
if the fault survives the retry fix.

- The “alias too far” message is from a JIT placement attempt that rejects
  and deallocates that candidate, then tries other placements. The supplied
  successful JIT self-test is evidence against simply diagnosing “JIT off”.
- Unresolved imports are logged during binding. Husk separately logs
  `CALLED an unresolved import` if a stub is actually invoked. No such call
  to `_Unwind_RaiseException` was supplied. This warning may matter later;
  it does not explain this ordinary non-executable mapping failure.
- The libcore `IOException` lacks the missing path and stack in the supplied
  excerpt. It was not suppressed or asserted to be harmless; it is a possible
  later launch blocker, not evidence that the mmap flag conversion is wrong.

## QEMU mismatch

`largestMappableMiB()` extends the RAM file to a probed candidate, possibly
2048 MiB. The old code then selected `max(anonymousTarget, fileBackedTarget)`.
An anonymous budget of 3324 MiB produces `0xcfc00000`, even though the file
still has the probed `0x80000000` length. Independently, `ramFileReady` meant
only that the path could be created, yet was used to select the file backend
even when every actual mapping probe failed. Snapshot early returns also
skipped preparing their exact file size.

`GuestRAMPlan` now carries the size and backend together. A successful file
probe determines the file-backed size; failed probes select anonymous RAM.
Pinned snapshot sizes explicitly prepare that exact file size or report an
error before QEMU starts, without silently changing the snapshot's machine.
`lastGuestMiB` is also populated on the formerly missed early-return paths.
No Android snapshot download is required for this work.

## Verification ledger

| Check | Result / execution host |
| --- | --- |
| Original `9345aab` C functions under bounded VM test | Expected failure reproduced |
| Patched production reservation + mmap functions compiled with MSVC | Passed |
| Exact size/flags, one retry, preserved hint/offset, guest errno | Passed |
| Live JIT ranges, successful mappings, guest-owned cage preserved | Passed |
| No cage, failed deallocation, other errors, failed retry | Passed |
| Original APK ARM64 allocator, success and ENOMEM paths in Unicorn | Passed |
| Husk production ELF analyzer on all 8 APK ARM64 libraries | Passed; no unsupported relocations reported |
| Existing `plan_test.c`, 4/16 KiB layouts, RELRO, BSS, holes and invalid input | Passed |
| Edited Swift syntax, workflow YAML, shell syntax | Passed; not Swift compilation |
| Dependency staging from checksum-verified official 3.3 IPA | Passed; 416 files staged |
| Official dependency Mach-O targets, wrong-platform rejection, reject official IPA as test build | Passed |
| Portable production C policy regression on Linux CI | Passed, run 41505148094 |
| Real Mach reservation/trim/protect/unmap integration | Passed on Intel macOS (4096-byte pages), run 41506268685, and Apple Silicon macOS (16384-byte pages), run 41507991694 |
| Existing compressed-heap Darwin regression | Passed on Intel and Apple Silicon macOS |
| Swift `GuestRAMPlan` execution tests | Passed with Swift 6.2.4 on Linux, Xcode 26.3 on Intel macOS and Xcode 27.1 on Apple Silicon |
| Patched app and both extensions compiled | Passed, Xcode 27.1 / Swift 6.4 / ARM64 iOS device, run 41507991694 |
| IPA ZIP/resources/identity/platform/patched-code validation | Passed on Mac and independently on Windows; 9 ARM64 device Mach-O binaries checked |
| IPA code signatures | Passed `codesign --verify --deep --strict` on Mac; ad-hoc signatures still need installer re-signing |
| Exact 0x1fffc000 reservation recovery on the physical iPad | Passed in user's 10a4cd0-dirty launch log |
| Full Unity startup on first test IPA | Failed later during resource/IL2CPP initialization, fault 0x135 |
| Original Unity archive parser and actual lookup caller, Unicorn | Reproduced .aPK failure; standard base.apk succeeds |
| Installed APK filesystem regression | Passed on Linux (41514378587) and Apple Silicon macOS (41514373867) |
| Follow-up IPA compiled, signature-verified and structurally validated | Passed, run 41514373867; Windows independent validation passed |
| Follow-up IPA physical launch; editor UI, touch, edit/save/reopen | Not yet tested |

The remote Swift test sent only `GuestRAMPlan.swift` and its test source to
Compiler Explorer's documented compile/execute API. Its compiler exit code and
execution exit code were both zero, `didExecute` was true, and all assertions
passed. No APK, device logs, pairing record or credential was sent. This tests
the platform-independent memory planning logic, not `QemuRunner`'s Apple APIs
or the QEMU machine. The raw response and source hash accompany the evidence.

The static ELF checks do not claim native libraries were actually loaded on
Windows. ANGLE/Metal and full Unity execution require an Apple host/device.

## Build and installation path

Upstream documents `scripts/ci_build.sh` on a Mac with Xcode. Inspection found
that the Java/system runtime and PS4 build scripts also refer to author-local
paths. Repeating that clean build on a fresh runner would leave essential
resources missing. The test workflow therefore compiles the changed app and
both extensions from source, builds the pairing static library from its locked
Rust dependencies, and reuses the unchanged runtimes from the exact official
[3.3 release](https://github.com/Leviidev/Husk/releases/tag/3.3).

Official IPA SHA256:
`47cdbac8ba310886bdd74d148ea411298e5cf0dd7bedc7f93d6da810681f695f`.
Its plist was checked for build 148 and commit `9345aab`. The official IPA
is an input dependency only; it is **not** a fixed output IPA.

`scripts/build_vm_test.sh` is the complete, provider-independent entry point.
It runs the native regressions, stages verified dependencies, compiles, then
validates the IPA and its signatures before writing the final output. It can
run on a Mac with selected Xcode 26+, Swift 6.4+, Python 3.11+, XcodeGen,
qemu-img and Rust/rustup. Intel hosts cross-compile the app for ARM64 iOS;
the native VM tests explicitly report the host's actual page size. The build
script needs no GitHub login or Apple signing account:

```sh
bash scripts/build_vm_test.sh
```

The public upstream source and official dependency IPA can be downloaded
without logging in. Set `HUSK_OFFICIAL_IPA` to an existing verified local copy
to reuse it. The host/Xcode checks fail before building on unsupported hosts.
Shell syntax, Windows rejection, all native Mac regression stages, dependency
staging, ARM64 iOS pairing-library build, full app build and IPA packaging and
validation have been exercised successfully.

`.github/workflows/vm-test-ipa.yml` calls that script on a choice of standard
public Mac runners. The `xcode-27` image supplies Xcode 27.1 / Swift 6.4.
Older images select Xcode 26.3 and install the signed official Swift 6.4.2
toolchain, checking its Swift Open Source installer identity before installing.
This is required because the unchanged upstream StikJIT module interface was
built by Swift 6.4. The compiler selection follows
[Swift's documented custom-toolchain procedure](https://www.swift.org/install/macos/package_installer/).
No framework interface is rewritten to hide the mismatch.

The workflow refuses private repositories, uses read-only repository token
permissions, and uploads an IPA only after tests, compilation, resource/ZIP/
Mach-O validation, and signature verification pass. Standard hosted runners
are [free for public repositories](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
No paid runner or Apple signing secret is configured. The authenticated public
fork is [Ghosttown-sh/Husk-VM-Test](https://github.com/Ghosttown-sh/Husk-VM-Test).
Builds have been dispatched; their logs and downloadable evidence are in its
Actions history. GitHub is the build host provider used here, not a dependency
of the bug fix or the provider-independent build script.

The validated result is `Husk-VM-Test.ipa`, with app identifier
`com.husk.vmtest`, display name **Husk VM Test**, extension IDs beneath that
identifier, and URL scheme `husk-vmtest`. Defaults for ordinary builds remain
`com.husk.app` / **Husk** / `husk`. VPN, certificate and game-link callbacks
use the installed build's scheme. Separate app containers preserve the
original installation and its data. The test app starts with its own data.

On Windows, download the successful workflow artifact and install the test
IPA using the user's existing installer, or AltStore Classic with AltServer
for Windows ([official Windows guide](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows)).
An ad-hoc build is structurally signed but must be re-signed by the installer.
Keep the original Husk installed. Import a copy of the original APK into
**Husk VM Test**. Existing VPN/pairing infrastructure should be reused; a
separate app container may need the existing pairing record imported once,
and JIT must attach to that test process. This is not a request to recreate
the already-working pairing/VPN setup.

## Follow-up: installed APK naming and resource initialization

The user's physical run logged `Failed to read assets/bin/Data/unity_app_guid`,
then failure to open the extracted `il2cpp/Metadata/global-metadata.dat`.
Both inputs exist in the original APK: the GUID is 36 bytes and the metadata
is 11,016,632 bytes; ZIP decompression and CRC validation pass locally.
The native PC is in libil2cpp at 0x1940b74: `ldrb w8, [x0, #0x135]` with
x0=0. The caller at 0x191ccc8 explains LR=0x191cccc. This is a null object
access after initialization failed, distinct from the old 0x50 allocator fault.

Husk exposes the imported filename directly through Native.apkPath() to
ApplicationInfo.sourceDir/publicSourceDir and getPackageCodePath(). Here that
filename ends in `.aPK`. Android's installed package naming is different;
[ApplicationInfo.sourceDir](https://developer.android.com/reference/android/content/pm/ApplicationInfo#sourceDir)
identifies the installed base APK, and the
[AOSP installer](https://android.googlesource.com/platform/frameworks/base/+/master/services/core/java/com/android/server/pm/PackageInstallerSession.java)
stages the base package as base.apk.

The APK's actual Unity ARM64 parser at 0x6e2d38 uses a table containing
`.apk/` and calls the case-sensitive search at 0x4b0050. Its archive lookup
caller at 0x6e4418 depends on successful splitting of archive and entry paths.
`tests/translation-layer/unity_apk_path.py` executes those unchanged original
instructions under Unicorn: the imported `.aPK/assets/...` fails; `.apk/assets/...`
and `.husk-package/base.apk/assets/...` both split correctly. The real caller
at 0x6e4418 was also executed with an available registered archive: `.aPK`
returns NULL before querying the entry, whereas base.apk queries the correct
entry and returns the archive. Allocation/string ownership and the registered
archive/entry lookup are harness callbacks, not a full Unity runtime. Spaces alone
do not fail. This confirms a compatibility defect matching the resource
failure, but the full resource initialization path must still be retested
on the iPad; the routine test is not full Unity execution.

The Java app driver now exposes `<data>/.husk-package/base.apk`, a hard link
to the original package. It preserves the exact APK bytes and its signature,
does not duplicate 128 MiB of content, and keeps the installed spelling even
after realpath(). Repeated launches reuse the same inode. A replaced source
updates only the managed alias atomically. Invalid paths and unexpected
symlinks fail explicitly; the app does not proceed with a truncated path.
The original APK, game data, loader and JIT behavior are preserved.

`run_apk_path.sh` tests the production helper's real filesystem operations:
mixed-case names, preserved bytes/inodes, canonical paths, repeat launch,
source replacement, undersized output, missing/nonregular sources, and
conflicting symlinks. The Mac IPA pipeline and Linux regression workflow
run it. No Java framework changes or game modifications are needed.

## Remaining actions, in order

1. Update the existing **Husk VM Test** with the validated follow-up IPA
   through Sideloadly with the same signing account and identifier. Preserve
   its installed data and pairing; do not uninstall either Husk app.
2. Launch the already-imported original APK through Translation Layer / java
   using the existing working JIT route. No repeated pairing setup is needed.
3. Capture the installed-base-APK diagnostic, GUID/metadata initialization,
   and any remaining crash PC/LR/registers. This is not a QEMU guest boot.
4. Confirm the original editor appears; tap controls, create/edit a small
   throwaway chart, save it, close/reopen the editor, and verify persistence.
   Record each result separately. Only then can the end-to-end goal be
   marked achieved.
