# Visual Maimai Mobile: VM investigation and test build

Status on 2026-11-02: the user's physical runs confirm the first memory
recovery and subsequent APK pathname/metadata fix. Build 90b1e42 progresses
through Unity, metadata, ANGLE, SwappyWrapper and AAudio, then a later
reservation/startup failure remains. The exact cause of the separate silent
termination is unconfirmed: no matching JetsamEvent is available.

Current follow-up IPA: [Actions run 41517898221](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41517898221),
source `b4509b776535ba6841b5794f76bf7e893a532586`, build stamp
`b4509b7-dirty`, 142,245,723 bytes. SHA256:
`6f8f21a0f15e684d6cb891df1f609d5fe31bc2638aa556f9ed367152f9c6081c`.
The real-gap reservation fallback, safe self process_vm_readv, earlier memory
and asset-path tests, QEMU RAM policy, full ARM64 iOS compilation, and strict
signature validation all passed on the Apple Silicon Xcode 27.1 runner.
The native tests explicitly reported 16,384-byte host pages. Windows then
independently validated all nine ARM64 iOS binaries, test identity, resources,
ZIP CRC, source stamp and new code diagnostics. This IPA has not yet run on
the physical iPad. Editor UI/touch/chart editing/save/reopen remain unverified.

Previous metadata-fix IPA: [Actions run 41514373867](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41514373867),
source `90b1e42068da4289f968d10457c1a0a8b2b4c542`, build stamp
`90b1e42-dirty`, 142,245,606 bytes. SHA256:
`1524621f2e9a0df785b9dca2f7b40d09c98d5a24b292baf2b71e51fe78395689`.
The user now confirms this build passed the former resource/metadata failure;
the repeated reservation and unknown-termination evidence is detailed below.

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

## Follow-up after 90b1e42: repeated reservations and termination evidence

The user now confirms 90b1e42 passed metadata loading and reached ANGLE,
SwappyWrapper and AAudio. Preserve both earlier fixes. The new supplied log
excerpt reports another `0x1fffc000` reservation failure, syscall 270 ENOSYS,
and a Unity Serialization allocation failure for 25,348,708 bytes. The
screenshot separately ends after four frames with 4,013 MiB available. These
may be different launches. No matching JetsamEvent or complete new native
report has been supplied; neither excerpt establishes a Jetsam kill.

### Repeated reservation mechanism

Apple's public `bsd/kern/kern_mman.c` explicitly forces untagged anonymous
mmap into `UMEM_RANGE_ID_HEAP` when map ranges are enabled. The VM range
selection in `osfmk/vm/vm_map.c` then limits an anywhere search to that range.
This is narrower than Linux's non-fixed anonymous reservation semantics.
Once the unused cage is gone, a later heap-range ENOMEM cannot be recovered
by releasing it again. The physical run proves the first reclamation helped;
it does not expose the device's entire map or prove which holes remain at the
second failure. The new fallback addresses that source-level incompatibility,
with on-device gap/footprint diagnostics to confirm or reject it for this run.

`tl_vm_reserve_gap` walks the actual task map within TASK_VM_INFO's min/max
bounds, including existing inaccessible reservations and submaps. Only after
an ordinary private anonymous PROT_NONE request fails with ENOMEM, it asks
Mach to reserve an entire suitable gap using VM_FLAGS_FIXED **without**
VM_FLAGS_OVERWRITE. The kernel atomically refuses collisions and applies its
usual address and protection checks. A competing allocation triggers a new
query; mapped bytes are never treated as free. There are no hard-coded device
addresses, larger process limits, smaller/fictitious reservations, or eager
page writes. No-fit returns ENOMEM. All Mach object ports returned by map
queries are released. Fixed/shared/file/executable mappings and the existing
Dart/V8 cage protocol retain their prior paths.

The result still has real PROT_NONE pages, VM_PROT_ALL maximum protection,
copy inheritance, normal mprotect/munmap and zero-fill behavior. The native
regression uses the production mmap/trim/protect code and real Mach mappings
in a constrained test window. It checks five successive Unity-size requests,
256 MiB alignment trimming, reuse after release, concurrent collision refusal,
real zero-fill/commit, small physical footprint, and honest exhaustion.
Anonymous ownership records are now changed only after a successful host
munmap, under the ownership lock; invalid unmaps cannot lose live records.

JIT RX, the original shared RW alias, and the nearer loader RW alias remain
owned by the JIT/QEMU and loader subsystems. The old RW alias cannot safely be
deleted merely because the loader prefers another: QEMU's prewarmed mapping
still points to it. Their virtual cost is distinct from physical page cost.
The fallback neither overwrites nor frees them. The guest's anonymous ranges
remain independently releasable rather than using a growing arena.

### ARM64 syscall 270 is process_vm_readv

The Android/Linux asm-generic syscall table defines 270 as process_vm_readv:
https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/kernel/uapi/asm-generic/unistd.h

The supplied libunity.so calls it at 0xa6b268, within the memory reader at
0xa6b1a0. Its embedded source path is
`PlatformDependent/AndroidPlayer/External/aosp/libunwindstack/Memory.cpp`.
It checks errno==38 (ENOSYS), then for its own PID substitutes memcpy at
0xa6b2a8. That fallback can fault while inspecting an unreadable address,
including while constructing an OOM/crash report. This is a diagnostic reader,
not the allocator issuing the 512 MiB reservation.

`unity_process_vm.py` executes this unmodified ARM64 routine locally from the
hash-verified user APK. ENOSYS + readable memory copies successfully; ENOSYS +
unmapped memory faults in the memcpy fallback; EFAULT + unmapped memory
returns zero bytes without executing memcpy. No game code/bytes are changed
or published. This proves the fallback hazard, not that this particular
unrecorded physical termination followed it.

The shim now implements self-process scatter/gather reads through Mach
vm_read_overwrite, validates vector counts/lengths/flags and descriptors,
returns actual bytes transferred, EFAULT for inaccessible memory, and partial
counts for a later unreadable page. Cross-process access is denied (or ESRCH
for nonexistent PIDs); no other task port or private permission is acquired.
No error is replaced with fake success. Native tests cover real copies,
invalid source/destination/descriptors, partial transfer and argument errors.

The inspected Unity and IL2CPP ELF imports have mmap/munmap but no mremap
import. Husk's existing mremap has incomplete Linux flag/protection semantics;
there is no supplied evidence it is on this failure path, so this change does
not redesign it or attribute the observed failure to it. General mremap and
other unrelated compatibility limitations remain follow-up work if reached.

### What the next physical run can establish

New logs include large reservations/unmaps, failed mprotect, Mach gap failure
codes and bounds, and TASK_VM_INFO footprint/resident/virtual bytes alongside
os_proc_available_memory on every startup heartbeat and mapping failure.
The generic crash dialog and low-memory diagnosis no longer assert Jetsam
from a missing report or one low available-memory estimate. A matching iPadOS
JetsamEvent is still needed to confirm an actual process-memory kill.

Automated results before cloud testing: Windows production cage-policy tests
PASS; the original pre-fix source still reproduces the intended failing
baseline; the local original-ARM64 syscall reader tests PASS. Native Darwin
regressions and the new IPA build subsequently passed in run 41517898221.
The gap test performed five real 0x1fffc000 reservations after injected
heap-mmap ENOMEM, each trimmed to a live 256 MiB arena, with approximately
1 MiB reported physical footprint. It preserved a racing mapping, reused a
freed hole, and returned ENOMEM when the entire test range was occupied.
The self-read tests verified real data and errors against Mach on 16 KiB
pages. The Mac log's `available=0` is a platform placeholder, not a measured
Mac memory budget; os_proc_available_memory is queried only on iOS.

A scan of direct syscall PLT call sites in this exact Unity/IL2CPP pair found
futex (98), affinity (122/123), gettid (178), and process_vm_readv (270), but
no direct mremap syscall (216). This further narrows the observed path; it
cannot exclude future indirect/native-plugin calls.

The updated IPA was downloaded and independently validated on Windows and
handed to Sideloadly by its normal file argument. No Apple device was
connected during the final check, so installation and physical execution
cannot be confirmed. Use the same signing account and existing Husk VM Test
identifier to update in place. Do not uninstall the app or clear its data.
Complete editor launch, touch input, chart loading/editing/saving and reopening
still require the user's physical test. If startup fails, retain the full new
run report and a matching-time iPadOS JetsamEvent/crash report when available.

## Physical follow-up: b4509b7, 2026-11-02 20:24 UTC

The user tested this IPA. Metadata, ANGLE ES 3.0, SwappyWrapper and AAudio
initialization still succeeded. The latest allocation failure is now measured:
`footprint=1113 MiB`, `available=4006 MiB`, largest ordinary VM gap
`0x10000000`, requested size `0x1fffc000`, zero suitable mapping attempts,
task bounds `0x102a48000..0x7180000000`. About fourteen retained 256 MiB
reservations precede it. Unity then reports a 25,348,708-byte Serialization
allocation failure. These are virtual reservations, not 512 MiB of dirty RAM
per call. This disproves the hypothesis that merely searching ordinary gaps
is sufficient for this run. It does not establish why the process ultimately
terminated; no matching OS termination/Jetsam report has been provided.

The original caller at ELF VA `0x489150` explicitly divides a 256 MiB alignment
by the reported page size before calling Baselib. This is not a fourfold
allocation inflation caused by reporting 16 KiB pages. Reducing getpagesize,
returning 256 MiB for a 512 MiB mmap, or overwriting inaccessible map entries
would violate the existing contracts and is not the fix.

Darwin also has padding VA which is deliberately represented as inaccessible
map entries. It is not an ordinary free gap. Apple's exported
`mach_vm_range_create` interface delegates such space after kernel validation;
`vm.malloc_ranges` reports the eligible intervals. The new fallback uses that
interface only after heap mmap and real-gap allocation fail. It proposes only
max-protection-zero, non-submap padding within the reported intervals. The
kernel must accept and create the entire requested PROT_NONE mapping. No
entitlements, process limits, guessed iPad addresses, guest constants, original
APK bytes, or live JIT aliases change.

Each registration is exactly the failed request's rounded size, with no
speculative extra capacity. Boundaries are remembered for reuse after guest
munmap (kernel registrations persist for the task lifetime). Reuse performs
atomic no-overwrite mappings inside a single registration. Unsupported APIs,
unavailable bounds, kernel refusal, metadata allocation failure and true
exhaustion all retain ENOMEM. This path still requires physical-iPad validation.

Primary sources: Apple's [range ABI](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_types.h),
[kernel implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_user.c),
[range reporting](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vm/vm_unix.c),
and [native regression examples](https://github.com/apple-oss-distributions/xnu/blob/main/tests/vm/vm_ranges.c).
Public XNU is implementation evidence, not a claim that every detail of iPadOS
27's private kernel is identical. API errors are logged for that reason.

### Follow-up build and test evidence

Run [41520853386](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41520853386)
completed successfully on an ARM64 Mac with 16 KiB host pages. Its source is
`f7126ab81d5ecaf1c8c7a4420e11147e0a7c5ed7`; the IPA reports `f7126ab-dirty`
because packaging regenerates the Xcode project and notices. The resulting
IPA is 142,249,210 bytes, SHA-256
`71aae4dbf3fb7b8b938115edafd24dd27aa76a3203ea877c5f5bc671ec4f74ae`.
Xcode compilation, strict deep code-signature verification, and a separate
Windows inspection of all nine ARM64 iOS binaries, iPad support, bundle IDs,
resources, new diagnostic markers and ZIP CRC passed. Sideloadly must still
sign it for the user's device using the existing test-app identity.

The Mac kernel does not expose `vm.malloc_ranges` (ENOENT). The first candidate
run, 41520490826, failed its integration assertion for this reason and produced
no IPA. The revised suite explicitly logs SKIP for actual kernel range
registration, then models only registration/bounds over a test-owned padding
window. Mappings, protection changes, memory access and unmapping remain real
Mach operations; this does **not** validate iPadOS acceptance of the range API.

With ordinary placement forced to fail, the revised suite passed 24 successive
Unity-size reservations, retaining 6 GiB of aligned virtual space with a
1,539 KiB physical-footprint increase. It passed release/reuse without another
registration, zero fill at both ends, refusal propagation, a concurrent
registration collision, and preservation of every live mapping. Previous
native cage, ordinary-gap, syscall 270, APK alias and QEMU RAM-plan regressions
also passed. The unmodified APK's ARM64 allocator and syscall-reader routines
were rechecked locally; no proprietary files were uploaded.

The new IPA has not yet run on the iPad. The next report should contain either
`mm: delegated VM range addr=` or a specific delegation-unavailable/refused
message if ordinary reservations are again exhausted. A successful build and
the modeled registration test do not establish successful Unity startup,
editor interaction or chart saving. Final process termination remains unknown
without a matching OS report; no JetsamEvent was accessible from this PC.
