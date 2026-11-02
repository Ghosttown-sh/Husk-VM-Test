# Follow-up: data/JIT separation and concurrent audio

The iPad report for `0f4869e-dirty` confirms installation of the GPU compositor build. It records the GPU image path, usually 50-57 produced/presented frames per second after loading, and 0.09-0.38 ms average copy/fence enqueue time. These are averages, not worst-frame latency or proof that audio is smooth. The reported stutter remains a real failure.

## Writable heap incorrectly classified as JIT code

`b_mmap` previously treated a small anonymous writable mapping with a hint near the executable pool as future JIT code. It returned the **RX** alias even though the caller requested **RW** memory. A hint alone cannot establish executable intent.

Local inspection of the user's unchanged ARM64 IL2CPP library identifies the heap allocation call at virtual offset `0x195917c`. It passes its remembered previous allocation end as a hint, `0x22` flags, and RW (optionally RWX according to its executable-pages setting). The decommit call at `0x194f52c` uses `MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS` and `PROT_NONE`, and aborts if the returned address differs. This matches Boehm's `GC_unix_mmap_get_mem` and `GC_unmap` contracts in [upstream os_dep.c](https://github.com/ivmai/bdwgc/blob/master/os_dep.c). No APK bytes are modified or included here.

The device log proves the non-executable hint heuristic was taken. Its eventual failed fixed mapping at `0x13b2fc000`, length `0x278000`, lies inside the recorded RX pool `0x134c00000..0x154c00000`. Thus the failing decommit targets a region which cannot be replaced as ordinary anonymous data. The guest then explicitly calls abort (signal 6); this is not a Jetsam-only report.

The same incorrect protection makes ordinary heap writes invoke store-fault emulation. A kernel read into such a buffer cannot use that user-space store handler and can fail with EFAULT. The reported managed FileStream failure is consistent with this mechanism, but its particular errno was not logged, so its complete resolution still needs a device test.

The fix removes address-hint inference. Non-executable data mappings get actual data permissions and ordinary mapping lifetime. Explicit executable allocations still use the existing JIT path; requests to change data into executable memory use the existing real mprotect success/failure policy. This deliberately removes the old speculative LuaJIT workaround: merely requesting RW near code no longer promises eventual execution. The VM reservation, assets/metadata and GPU compositor fixes are retained.

A lock-free count of emulated stores is now included in the Unity heartbeat, so subsequent logs can identify unexpected remaining store traps without adding logging to the signal handler.

## Concurrent audio was concatenated

This run opens Unity AAudio at 48 kHz and ten OpenSL players at 44.1 kHz. Their dedicated pump threads all called a hook backed by one global ring. A silent producer consumed the same FIFO playback time as another producer's sound, and the first rate remained the only AudioQueue format. This is neither mixing nor rate conversion.

The host now gives each persistent mixer producer thread an independent queue and bounded ring. Core Audio handles simultaneous playback and each queue's declared sample rate. This follows Apple's [multiple AudioQueue playback guidance](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/MultimediaPG/UsingAudio/UsingAudio.html). The existing low-rate FMOD conversion remains, with independent interpolation history and bounded scratch storage.

Thread-local destructors dispose queues and buffers when AA/OpenSL pump threads exit. Stop operations cancel a blocked writer before joining. An atomic lifetime check also covers a stop racing the very first sink creation while paused. Queue control and disposal do not hold the callback mutex. Buffer allocation/queue creation errors are checked and partial initialization is cleaned up.

The existing 8192-frame ring and three 1024-frame host buffers have not been enlarged. This repair does not claim minimal audio latency. Periodic per-producer supplied/drained/empty-frame counters distinguish starvation from serial contention; empty frames also include legitimate silence after a one-shot sound.

## Regression evidence

The Windows production-mmap policy test fails on the delivered old source at the assertion that a nearby RW heap must not consume the executable pool. It passes after the correction, together with the previous pressure/cage tests.

New Darwin tests exercise 128 real hinted RW mappings, kernel reads into them, GC-style fixed decommit/recommit, zero fill, unmap, and preservation of an RX sentinel and explicit executable allocation routing.

New audio tests compile the actual host sink with a deterministic AudioQueue backend and real pthread producers, under AddressSanitizer and UndefinedBehaviorSanitizer. They cover eleven concurrent sources, 44.1/48 kHz and mono/stereo separation, independent backpressure, exact drained PCM, pause/resume, cancellation while paused, TLS cleanup, low-rate interpolation isolation, format replacement and injected creation failures. They do not replace listening on the iPad or measure its hardware scheduling.

The [Mac run 41538703978](https://github.com/Ghosttown-sh/Husk-VM-Test/actions/runs/41538703978) completed successfully on 2026-11-02. Its ARM64 Darwin tests report 16384-byte host pages. Both new suites passed, including the AddressSanitizer/UndefinedBehaviorSanitizer audio run. Existing VM pressure, gap search, aligned reservation, delegated range, compressed cage, QEMU RAM, APK path and graphics regressions also passed. The real ANGLE/Metal compositor tests passed at 2420x1668 with both zero and four-sample MSAA.

The installable test IPA is built from `41957ae7b21752c871778c9bf198db00ae737ca3` and reports **`41957ae-dirty`** (the existing packaging process stages build dependencies). Its size is 142254716 bytes and SHA256 is `e87618741c091583b5ce6b22eb1b99414f3ee26fc54709222a946192e9df0645`. The pipeline verified strict code signatures; local revalidation checked nine ARM64 iOS binaries, bundle identities, required resources, ZIP integrity and presence of the new implementation markers. The removed RW-hint heuristic is absent from the executable.

Forty-five bundled Java runtime and ANGLE files are byte-identical to the preceding `0f4869e-dirty` IPA. Main app and extension identifiers remain unchanged (`com.husk.vmtest` before sideload signing). Prior IPAs are retained for rollback. This is an ad-hoc signed test artifact for the existing sideload workflow, not an App Store distribution certificate.

Physical-device smooth audio, file import, chart editing and saving remain unverified for this follow-up. The previous report's AVPro encoder/native-init limitations are separate unresolved compatibility issues; this patch does not claim recording/export support. Device validation should listen to the startup sound, import audio, exercise chart edits/save and retain the resulting build line, audio-producer counters and emulated-store counts if a problem remains.
