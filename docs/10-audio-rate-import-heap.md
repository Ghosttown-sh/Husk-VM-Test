# Follow-up: stream rate and writable heap exhaustion

The physical iPad report for `41957ae-dirty` confirms that the app opens and all reported emulated-code-store counts are zero. The user reports double-speed audio and an OGG import crash. These are new acceptance failures, not evidence of completed device validation.

## Audio stream rate

Host queue counters increase by roughly 240640 frames per five seconds for AAudio and 221184 for each OpenSL player, consistent with their advertised 48000/44100 Hz rates. No empty frames are reported. This points upstream of the host queue clock; it does not prove that the advertised stream rate is correct.

`AAudioStreamBuilder_setSampleRate` was bound to an ignore stub. Opening a stream, its sample-rate getter, and host submission all unconditionally used 48000 Hz. [The Android AAudio contract](https://developer.android.com/ndk/reference/group/audio) requires an explicit rate to be honored or opening to fail. Only an unspecified rate allows the implementation to choose another value.

Local inspection of the unchanged Unity library shows its AAudio initializer resolving the setter at offset `0xfa8994` and calling it with the mixer's rate at `0xfa8ca0..0xfa8cac`. This initializer does not resolve/query `AAudioStream_getSampleRate`. Thus a mixer configured for 24000 Hz would be consumed at twice its intended rate. **The supplied old log does not record the requested value**, so the exact rate on this device remains to be confirmed, rather than inferred from the old hardcoded 48000 message.

The builder now retains the requested rate. Opening, the getter, host submission and fallback pacing consistently use it. Zero still chooses 48000 Hz; unsupported rates fail. Hardware-rate reporting remains distinct from the software stream rate. New logs report both requested and selected values. PCM conversion uses bounded stack chunks, preserving frame count and mono/stereo semantics without retaining per-thread conversion allocations. The host's existing low-rate interpolation doubles samples and queue rate together, preserving duration.

The AAudio regression compiles the actual adapter, checks the real symbol-table binding, and exercises 18 combinations of rate and channel count. Each written stream submits one second of PCM; callback streams receive 256 frames per call. Tests check PCM channel values, rate, duration and frame counters under ASan/UBSan. Existing host-queue tests separately check low-rate interpolation and concurrent playback. These are adapter/backend tests, not a listening test on the user's hardware.

## Import allocation failure

The cache copy completes and the plugin reports freeing its temporary memory. Then private anonymous RW mappings of `0x3e3c000` and `0x3588000` bytes fail with ENOMEM while approximately 3620 MiB remains available before Jetsam. The first fault is an actual null write, not an iOS memory-pressure termination.

Local IL2CPP disassembly identifies the fault at `0x191c414`: `stp x19, xzr, [x0]`, immediately after the allocation call at `0x191c410`. The returned pointer is zero. This ties the crash to allocation failure while constructing a managed array. No APK bytes, song files or full device logs are included in this repository.

Existing real-gap and delegated-range recovery only accepted PROT_NONE requests. The new path also accepts private anonymous non-fixed read/write data mappings, including soft address hints. It reserves the complete requested size and applies the requested permissions before returning. Failed protection changes unmap the reservation and preserve the actual errno. This does not use executable storage, overwrite live mappings, shrink allocations, return phantom data memory, bypass an explicit refusal, or increase the app's RAM entitlement.

Policy tests cover both reported sizes, protection failure rollback and excluded fixed/shared/file/unknown-flag mappings. Darwin tests force ordinary heap placement to fail, then exercise real gap mappings and delegated-range reuse: writes on every page, GC-style fixed decommit/recommit, zero fill, ownership tracking and unmapping. Delegation tests explicitly report whether actual kernel range creation or its existing registration-contract model is available on the Mac.

## Delivery status

Windows policy tests pass. Mac integration, sanitizer checks, IPA packaging and physical-device verification are pending for this change. Existing startup, JIT, GPU compositor and file-import path fixes remain in place; the same test bundle identifiers and prior IPA rollback copies are retained.
