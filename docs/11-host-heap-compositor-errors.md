# Follow-up: host allocation and compositor error isolation

The physical-device report for `4feb244-dirty` ends in a different failure from the prior OGG import. The main interpreter thread faults at `tl_jni_new_object + 0x28`. Inspection of the delivered ARM64 executable shows `calloc(1, 0x48)`, followed by `str w21, [x0]`; the recorded result is zero. Thus a **72-byte host object allocation failed** and was dereferenced unchecked. The footprint is approximately 1567 MiB with 3552 MiB available before Jetsam. This is not a Jetsam termination or a failing JIT self-test.

The log's successful, repeated 256 MiB Unity reservations consume ordinary allocation ranges. Address-space pressure is the leading explanation for host malloc failing with that physical headroom; the old guest-only mmap recovery cannot service host libc calloc. The crash log does not contain a full host allocator map, so exact libmalloc zone exhaustion is not independently proven on the iPad.

## Preserve the host allocation range

Large native aligned guest reservations (at least 64 MiB) now try kernel-approved delegated space **before** ordinary Mach placement. The existing `mach_vm_range_create` implementation is extended to honor the requested power-of-two alignment on both fresh registration and reuse. It registers exactly the requested size, leaves unrelated mappings intact and retains ordinary placement when delegation is unavailable/refused. The existing implementation still requires the kernel's reported void bounds and inaccessible padding; it does not treat padding as free memory, overwrite it from userspace, or alter RAM limits.

This follows [Apple's XNU range registration tests](https://github.com/apple-oss-distributions/xnu/blob/main/tests/vm_ranges.c), which distinguish fixed delegated ranges from the ordinary heap. Both fresh registration and aligned reuse are exercised with real Mach pages. On Mac hosts without the range API/bounds, the suite explicitly reports its registration-contract model; that cannot certify the iPad kernel's acceptance.

The JNI allocator checks object and field allocation results. A preallocated `OutOfMemoryError` is prepared after boot classes link, before app loading. The reported interpreter `new-instance` path propagates it through exception dispatch rather than dereferencing NULL or allocating another exception recursively. String/array factories also reject failed storage allocation. This is protection for these paths, not a claim that every existing host allocation in the translation layer is now OOM-safe. Avoiding the address-space exhaustion remains the primary repair.

## Keep unrelated GL errors out of image-import checks

This run switches to the CPU compositor after one generic import/wait failure, then remains at 8.6–14 fps with 63–78 ms readback/copy time per frame. The original diagnostic does not identify the exact GL error or fence stage.

The bridge checked `glGetError` after importing the image without first isolating errors from earlier UI drawing in the private consumer context. A stale GL error therefore disabled all subsequent GPU frames even when the import succeeded. A regression with an injected prior consumer error fails on the delivered source and passes with the fix. The real Windows ANGLE test also retains correct pixels and queued reuse when the private context has a prior error.

The consumer now clears/reports prior private UI errors before importing. Guest errors retain their existing separate preservation path. Context loss, genuine image import failures and fence timeouts still fail safely; no fake fence completion or retry over in-flight data is introduced. Stage-specific diagnostics identify image creation, copy, import and producer/consumer wait failures. Synchronization continues to follow [KHR_wait_sync](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_wait_sync.txt). The old log alone cannot prove that a stale error caused this particular iPad fallback; the new diagnostic distinguishes that from a driver failure.

## Audio and acceptance

The new device log confirms Unity explicitly requests **24000 Hz**. AAudio now preserves that value. The 48000 Hz host producer line is expected: the existing interpolation doubles the sample count and output rate together, preserving duration. This verifies the old ignored-rate bug's trigger; a physical listening confirmation is still pending.

This run never reaches the file picker, so it does not validate the prior OGG-import repair. The original APK, runtime resources and independent OpenSL effect-player rates remain unchanged. The next device checks are song speed, repeated create/import operations, sustained GPU presentation and editing/saving. Private APKs, songs and full device logs are excluded from the public repository.

## Validation status

Windows: the stale-error regression fails on the previous source and passes after the fix; real ANGLE/D3D11 image transfer, shader sampling, alpha/orientation and queued reuse pass. Mac sanitizer, aligned-delegation, real Metal and IPA validation are pending the new build.
