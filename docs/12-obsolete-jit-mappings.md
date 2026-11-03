# Follow-up: reclaim obsolete JIT mappings

The next iPad report reaches about 1,013 Unity frames, keeps GPU presentation at 44–57 fps, then reports failure to allocate a 72-byte host Java object. The user identifies **opening the BPM input field, before typing anything**, as the triggering operation. The preallocated OutOfMemoryError now propagates instead of dereferencing NULL. This protects the failure path but does **not** resolve allocation exhaustion. Physical headroom remains roughly 3.6 GiB.

Every large guest reservation reports `VM range bounds unavailable errno=2`, so the previous proactive delegated-range path was not accepted on this device. [XNU's sysctl implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vm/vm_unix.c) can return ENOENT when the process does not use user ranges; it is not proof that the API name simply disappeared. The diagnostic now reports a non-mutating empty-recipe capability probe on the first bounds failure. It does not guess usable addresses or overwrite kernel padding. Mac registration-model results from the previous build did not establish iPad support.

## Confirmed redundant mappings

The debugger supplies a 512 MiB RX mapping, and Husk creates its 512 MiB writable alias. When that alias is too far away for the native loader, `near_alias` creates another nearby mapping. In this run only a 288 MiB prefix fits. Previously the original 512 MiB writable view and 224 MiB unused RX tail stayed mapped indefinitely, although the native loader used the new 288 MiB view. The far alias occupies the same high address region used by subsequent guest heap reservations. These are virtual-address costs; duplicate aliases do not mean duplicate physical copies of all pages.

The shared JIT pool now owns the handoff. Under its sharing lock it lets the native loader establish a near alias, publishes the resulting capacity, deallocates the obsolete writable view and trims the unused RX tail. In the reported layout this can release **736 MiB of virtual address space**. Full-size relocation releases just the redundant writable view. Failure to deallocate retains the original shared descriptor.

The handoff cannot reclaim any mapping already published to the legacy loader, probe, console arena or QEMU, or claimed from the bottom by the native runtime. In those cases the near alias remains private and the original pool remains valid. QEMU takes its top slice under the same lock, using the post-handoff capacity. A request too large for the remaining pool fails rather than overlapping native code. The callback is internal and synchronous and must not reenter pool APIs. No new executable pages, TXM changes or debugger reattachment are involved.

The allocator lives in the QEMU dylib. The test build therefore rebuilds QEMU and its pinned public dependencies instead of reusing the official release's stale allocator. Both the new exported entry point and packaged implementation marker are checked. Java and ANGLE runtime resources remain the verified release versions; proprietary game binaries, music and full device reports are excluded.

## Validation and limits

The Mac regression uses real Mach aliases and the production pool implementation. It checks 512-to-288 MiB handoff, live ARM64 execution, shared data, reuse of the released range for ordinary writable memory, native/QEMU overlap refusal, existing readers, failed deallocation, full-size relocation and concurrent publication. Kernel delegation is not needed for this repair. The unchanged audio, compositor, import-allocation and JNI regressions remain build gates.

This removes a demonstrated source of address-space waste; it does not yet establish that the iPad has enough remaining space for BPM editing or repeated imports. A successful Mac build cannot certify physical-device song speed, OGG import, editing or saving. Device confirmation must include the `reclaimed obsolete JIT mappings` line, opening the BPM editor, changing/confirming BPM repeatedly, and create/import operations without host allocation failure. The first failed host allocation now records the interpreter's existing Java frames before they unwind, without constructing another Throwable or allocating a Java backtrace. This distinguishes the input dialog and callback paths if exhaustion remains.

## Completed build evidence

[Mac CI run 41551115698](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41551115698) passed on 2026-11-03 for source `1b088990a36ba8d525aafd4a63c19b290a12ae22`. The IPA identifies itself as `1b08899-dirty` because packaging generates project files. The complete QEMU build uses pinned EGL headers, an isolated Python generator environment, and an explicit Meson device-runner requirement so that iOS compiler probes are not executed on macOS.

- Real Mach tests passed with 16 KiB host pages: 736 MiB reclaimed, live ARM64 execution, released-address reuse, existing-reader protection and concurrent publication.
- 100 injected JNI allocation-failure cycles passed, including reserved OOM propagation, first-failure stack reporting and subsequent allocation recovery. Audio, compositor, guest allocation and package regression gates also passed.
- The downloaded IPA passed ZIP, resource, bundle-ID and all nine ARM64 iOS-device binary checks. Its rebuilt QEMU exports the new handoff and retains all 33 previous Husk exports; all dynamic dependency paths target device libraries. Both new allocation-failure diagnostics are present in the shipped app.
- IPA: 142,252,670 bytes; SHA-256 `767d9311ba0122e2b7cbf6e361925e0ae4c0ef2f99c2304afb92cdc75bab92fc`.

Physical iPad confirmation remains pending. First open the existing chart and tap the BPM field before typing; then edit/confirm repeatedly, play a song, and import another audio file. Preserve the preceding `d93ed8e` IPA for rollback.
