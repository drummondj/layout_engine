#import "LayoutEngineTexture.h"

extern "C" {
void render_pixel_buffer(void *layout_engine_handle,
                         CVPixelBufferRef pixel_buffer);
void destroy(void *layout_engine_handle);
}

@implementation LayoutEngineTexture {
  id<FlutterTextureRegistry> _registry;
  int64_t _textureId;
  CVPixelBufferRef _pixelBuffer;
  void *_layoutEngineHandle; // Holds our C++ instance pointer securely
}

- (instancetype)initTextureWithRegistry:(id<FlutterTextureRegistry>)registry
                                 handle:(void *)handle
                                  width:(int)width
                                 height:(int)height {
  self = [super init];
  if (self) {
    _registry = registry;

    // 2. Use the existing cross-platform C++ engine instance
    _layoutEngineHandle = handle;

    // 3. Allocate a high-performance macOS pixel buffer canvas
    NSDictionary *options = @{
      (id)kCVPixelBufferCGImageCompatibilityKey : @YES,
      (id)kCVPixelBufferCGBitmapContextCompatibilityKey : @YES,
      (id)kCVPixelBufferMetalCompatibilityKey : @YES
    };

    CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                        kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)options, &_pixelBuffer);

    // 4. Register this surface with Flutter and capture the ID token
    _textureId = [_registry registerTexture:self];
  }
  return self;
}

- (int64_t)textureId {
  return _textureId;
}

- (void)resizeWithWidth:(int)width height:(int)height {
  // Do nothing if dimensions are invalid or haven't changed.
  if (width <= 0 || height <= 0 ||
      (CVPixelBufferGetWidth(_pixelBuffer) == width &&
       CVPixelBufferGetHeight(_pixelBuffer) == height)) {
    return;
  }

  // Release the old buffer
  if (_pixelBuffer) {
    CVBufferRelease(_pixelBuffer);
    _pixelBuffer = nil;
  }

  // Create a new pixel buffer with the new dimensions
  NSDictionary *options = @{
    (id)kCVPixelBufferCGImageCompatibilityKey : @YES,
    (id)kCVPixelBufferCGBitmapContextCompatibilityKey : @YES,
    (id)kCVPixelBufferMetalCompatibilityKey : @YES
  };
  CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                      kCVPixelFormatType_32BGRA,
                      (__bridge CFDictionaryRef)options, &_pixelBuffer);
}

// 5. This method is called automatically by Flutter when a new frame is
// requested
- (CVPixelBufferRef)copyPixelBuffer {
  if (_pixelBuffer) {
    CVBufferRetain(_pixelBuffer);
  }
  return _pixelBuffer;
}

// 6. The 60FPS tick target triggered by your timer loop
- (void)tickAnimationFrame {
  if (!_pixelBuffer || !_layoutEngineHandle)
    return;

  // Call your shared C++ adapter function (handles locking and drawing
  // internally)
  render_pixel_buffer(_layoutEngineHandle, _pixelBuffer);

  // Signal Flutter's engine pipeline that the pixels changed and need a redraw
  [_registry textureFrameAvailable:_textureId];
}

- (void)dealloc {
  if (_pixelBuffer) {
    CVBufferRelease(_pixelBuffer);
  }
  if (_layoutEngineHandle) {
    // We don't destroy the handle here, as its lifecycle is managed from Dart.
    _layoutEngineHandle = nil;
  }
}

@end
