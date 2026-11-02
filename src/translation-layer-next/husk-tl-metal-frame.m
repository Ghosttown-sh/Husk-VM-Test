/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>

/* EGL_ANGLE_device_metal + EGL_ANGLE_metal_texture_client_buffer. Allocate on
 * ANGLE's actual device, not an independently selected default GPU. EGLImage
 * retains the MTLTexture; ARC releases our temporary ownership on return. */
void *tl_gl_frame_metal_image(void *display, int width, int height,
                            void *(*resolve)(const char *))
{
    @autoreleasepool {
        unsigned (*query_display)(void *, int, intptr_t *) = resolve("eglQueryDisplayAttribEXT");
        unsigned (*query_device)(void *, int, intptr_t *) = resolve("eglQueryDeviceAttribEXT");
        void *(*create_image)(void *, void *, unsigned, void *, const int *) = resolve("eglCreateImageKHR");
        if (!query_display || !query_device || !create_image || width <= 0 || height <= 0) return NULL;
        intptr_t egl_device = 0, metal_device = 0;
        if (!query_display(display,0x322c,&egl_device) || !egl_device ||
            !query_device((void *)egl_device,0x34a6,&metal_device) || !metal_device) return NULL;
        id<MTLDevice> device = (__bridge id<MTLDevice>)(void *)metal_device;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
        if (!texture) return NULL;
        const int attributes[] = { 0x3038 };
        void *image = create_image(display,NULL,0x34a7,(__bridge void *)texture,attributes);
#if !__has_feature(objc_arc)
        [texture release];
#endif
        return image;
    }
}
