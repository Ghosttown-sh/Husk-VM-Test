# Visual Maimai Mobile: VM investigation and test build

Status on 2026-11-02: source fixes implemented and local Windows tests passed.
The QEMU RAM-plan Swift source has also been compiled and executed successfully
by Swift 6.2.4 on a public Linux compiler service without a login.
No modified IPA has been compiled yet. The macOS integration tests, complete
Unity startup, and physical iPad editor/touch/save checks remain unverified.

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

The supplied iPad log lacks the process VM map and full crash registers.
Consequently this is a confirmed, reproducible address-space ownership defect
and a supported candidate cause for that device failure, **not proof that it
is the only cause on the iPad**. The patched build logs cage retention,
reclamation, the retry result, and more complete failed-call arguments so
the device test can distinguish it from genuine remaining exhaustion.

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

| Check | Result on Windows |
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
| Real Mach reservation/trim/protect/unmap integration | Written; requires macOS, not run |
| Existing compressed-heap Darwin regression | Requires macOS, not run |
| Swift `GuestRAMPlan` execution tests | Passed using actual Swift 6.2.4, Swift 5 language mode, Linux x86-64 via Compiler Explorer; Apple build remains untested |
| Patched app compilation / IPA signatures | Not run; no macOS executor connected |
| Full Unity startup, editor UI, touch, edit/save/reopen on iPad | Not run |

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
run on any Apple Silicon Mac with selected Xcode 26+, Python 3.11+, XcodeGen,
qemu-img and Rust/rustup. It needs no GitHub login or Apple signing account:

```sh
bash scripts/build_vm_test.sh
```

The public upstream source and official dependency IPA can be downloaded
without logging in. Set `HUSK_OFFICIAL_IPA` to an existing verified local copy
to reuse it. The host/Xcode checks fail before building on unsupported hosts.
Shell syntax and the Windows rejection path were checked; the Mac build
itself remains unexecuted.

`.github/workflows/vm-test-ipa.yml` calls that same script in a manual Actions build
on standard Apple Silicon `macos-15`, explicitly selecting Xcode 26.3 because
the runner's default Xcode 16.4 is too old for Husk's iOS 26 APIs. The
[runner inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md)
lists that Xcode/SDK. An iOS device build can run on newer iPadOS; it need
not be compiled against the exact device OS version.

The workflow refuses private repositories, uses read-only repository token
permissions, and uploads an IPA only after tests, compilation, resource/ZIP/
Mach-O validation, and signature verification pass. Standard hosted runners
are [free for public repositories](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
No paid runner or Apple signing secret is configured. It has **not been
dispatched**. The user has authorized proceeding, but there is no authenticated
GitHub session or connected Mac executor in this environment. Authorization
is no longer the blocker; a usable build host/session is. GitHub is one way
to obtain the Mac runner, not a dependency of the bug fix or build script.

The result, if the build succeeds, is `Husk-VM-Test.ipa`, with app identifier
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

## Remaining actions, in order

1. Provide access to an Apple Silicon Mac or complete the prepared official
   GitHub CLI browser sign-in so the free public Actions runner can be used.
   The user has already authorized the work; no further routine permission is
   needed. There is no requirement to install a Codex GitHub plugin. Only
   Husk source changes belong on the fork: no APK, Apple account secret or
   pairing file should be uploaded.
2. Run the prepared Darwin/Swift tests and compile. If any fail, fix and
   rerun before offering an IPA. This is still outstanding engineering work,
   not a claim that the workflow is already proven.
3. After the validated IPA exists, install it alongside Husk from Windows.
   Enable the existing working JIT route for the separate test process and
   launch the original APK through Translation Layer / java.
4. Capture the launch log, including any `mm: holding unused...`,
   `mm: released unused...`, retry result and complete PC/LR/register lines.
   If no cage was reclaimed, investigate the actual device VM map before
   changing more allocation behavior.
5. Confirm the original editor appears; tap controls, create/edit a small
   throwaway chart, save it, close/reopen the editor, and verify persistence.
   Record each result separately. Only then can the end-to-end goal be
   marked achieved.
