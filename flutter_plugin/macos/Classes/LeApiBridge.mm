#import "LeApiBridge.h"

// Plain relative include out of the pod's module root - fine here since
// this is a normal (non-modular) Objective-C++ translation unit, exactly
// like layout_engine_plugin.c's own forwarder to ../../src/layout_engine_plugin.c.
// This is also this target's only .mm/.cpp source (besides layout_engine_plugin.c,
// a plain C forwarder) - its mere presence is what makes Xcode select the
// C++ linker driver for the whole target, which the backend archives this
// plugin links (see ../layout_engine_plugin.podspec) need regardless of
// whether this file did anything else.
#include "../../src/layout_engine_plugin.h"

@implementation LeApiBridge {
  LeHandle *_handle;
}

- (instancetype)initWithHandleAddress:(int64_t)handleAddress {
  self = [super init];
  if (self) {
    _handle = (LeHandle *)(uintptr_t)handleAddress;
  }
  return self;
}

- (nullable CVPixelBufferRef)copyPixelBufferBGRA {
  if (_handle == NULL) {
    return NULL;
  }

  LePixelBuffer buf = le_render_pixel_buffer(_handle);
  if (buf.data == NULL || buf.width <= 0 || buf.height <= 0) {
    return NULL;
  }

  CVPixelBufferRef pixelBuffer = NULL;
  // api.hpp's LePixelBuffer is deliberately platform-neutral RGBA8888 (see
  // its own comment); FlutterTexture.copyPixelBuffer's doc restricts the
  // returned CVPixelBuffer to a fixed set of formats that doesn't include
  // 32RGBA - only kCVPixelFormatType_32BGRA fits premultiplied RGBA data
  // without a color-space reinterpretation, so swizzle R/B per pixel below
  // rather than assume the byte order already lines up.
  //
  // kCVPixelBufferIOSurfacePropertiesKey is required, not optional: without
  // it CVPixelBufferCreate makes a plain (non-IOSurface-backed) buffer,
  // which the engine's Metal compositor can't wrap into a texture at all -
  // it fails silently from this call's own point of view (CVReturn success
  // here) and only surfaces later as "Could not create Metal texture from
  // pixel buffer: CVReturn -6660" in the system log, with the Texture
  // widget just staying blank forever (confirmed by trial - found exactly
  // this way, not from documentation).
  NSDictionary *attributes = @{(NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}};
  CVReturn status = CVPixelBufferCreate(kCFAllocatorDefault, buf.width, buf.height,
                                         kCVPixelFormatType_32BGRA,
                                         (__bridge CFDictionaryRef)attributes, &pixelBuffer);
  if (status != kCVReturnSuccess || pixelBuffer == NULL) {
    return NULL;
  }

  CVPixelBufferLockBaseAddress(pixelBuffer, 0);
  uint8_t *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pixelBuffer));
  size_t dstBytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);

  for (int32_t y = 0; y < buf.height; y++) {
    const uint8_t *srcRow = buf.data + static_cast<size_t>(y) * buf.row_bytes;
    uint8_t *dstRow = dst + static_cast<size_t>(y) * dstBytesPerRow;
    for (int32_t x = 0; x < buf.width; x++) {
      const uint8_t *s = srcRow + x * 4;
      uint8_t *d = dstRow + x * 4;
      d[0] = s[2];  // B
      d[1] = s[1];  // G
      d[2] = s[0];  // R
      d[3] = s[3];  // A
    }
  }

  CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
  return pixelBuffer;
}

@end
