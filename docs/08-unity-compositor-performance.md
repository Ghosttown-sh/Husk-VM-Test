# Unity native-window compositor performance

## Evidence and scope

After the native aligned VM build (6394582), the user reports that the app opens
fully, but the whole picture and animations are slow. This is physical launch
feedback, not a measured frame rate or confirmation of chart editing/saving.
The previous VM, asset, IL2CPP, JIT, and audio repairs remain unchanged.

The earlier device logs identify Unity's native window as a composited surface.
Tracing `w_eglSwapBuffers` shows a full `glReadPixels` from its pbuffer, a CPU
`memcpy` into E2, and a `glTexSubImage2D` upload on Husk's Java GL thread.
At 2420 x 1668 RGBA this reads 16,146,240 bytes per frame before the copy and
upload. GPU completion/readback and these transfers are avoidable. Source
inspection establishes this cost, not its proportion of the current iPad stall.
No new device performance log was available when implementing this change.

## Change

`husk-tl-gl-frame.c` copies the default framebuffer into a GPU texture and shares
its storage through EGLImage with the existing UI compositor. On Metal,
`husk-tl-metal-frame.m` creates a native texture on ANGLE's queried MTLDevice and
imports it into both independent GL namespaces. ANGLE Metal supports external
Metal image imports but cannot export ordinary GL texture images; the generic
`EGL_KHR_image` extension alone must not be mistaken for that capability.
Other backends with the explicit GL texture image extension can export directly.
Guest and host texture namespaces remain independent; Java overlays, touch
coordinates, resolution and the APK are unchanged. EGL producer/consumer fences
and real server waits protect reuse; a driver without the fence extensions uses
real `glFinish`, never a fabricated success. Unsupported image/copy/import/wait
operations fall back to the existing CPU compositor. The bridge uses raw host
ANGLE functions, bypassing the guest's existing fence compatibility shims.

One pending frame applies bounded compositor backpressure. State restoration
covers texture, read framebuffer and unpack buffer bindings. Guest GL errors
are retained for `glGetError`. Resize and hide/show keep image and fence counts
bounded; foreign-context resets retain the source name until its owner reuses
or destroys it. The CPU fallback avoids uploading an unchanged frame twice.

Logs report GPU copy/enqueue cost (excluding producer pacing), CPU readback/copy
cost, and five-second produced/presented frame rates. These distinguish slow
frame generation from slow delivery. `TL_GPU_COMPOSITOR_DISABLE=1` is a host
diagnostic switch for comparing the original path.

## Verification

- Windows MSVC production-core regression: 240 cross-context frames, ordering,
  binding/error preservation, resize, 32 foreign-context hide/show cycles,
  extension refusal, image/copy/import/wait failure, and real completion fallback.
- Real Windows ANGLE/NVIDIA D3D11: independent contexts, 30 transfers each at
  2420 x 1668 with zero and four requested MSAA samples, exact pixel checks and
  guest binding/error checks. This backend used the real `glFinish` fallback.
- The Mac pipeline runs the same production bridge against an official pinned
  CEF ANGLE Metal host build, separately from the iOS ANGLE shipped in the IPA.
  A successful pipeline is required before delivery; native tests cannot certify
  performance on the user's iPad or the complete editor workflow.
- Previous VM regressions and IPA identity/architecture validation remain gates.

The real GPU regression performs a one-pixel CPU read solely to verify output.
That read is not present in the production GPU compositor. Reported host timing
includes this test overhead and must not be represented as iPad performance.

## References

- [EGLImage base](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_image_base.txt)
- [Combined EGLImage](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_image.txt)
- [EGL fence sync](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_fence_sync.txt)
- [EGL server wait](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_wait_sync.txt)
- [ANGLE Metal capabilities](https://github.com/google/angle/blob/main/src/libANGLE/renderer/metal/DisplayMtl.mm)
- [Metal texture image import](https://github.com/google/angle/blob/main/extensions/EGL_ANGLE_metal_texture_client_buffer.txt)
- [Query ANGLE's Metal device](https://github.com/google/angle/blob/main/extensions/EGL_ANGLE_device_metal.txt)

Remaining physical check: update the existing Husk VM Test, launch the same app,
observe animation and touch responsiveness, and load/edit/save a chart. If slow,
the new produced/presented FPS and compositor timings locate the next bottleneck.
