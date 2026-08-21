#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Objective-C++ wrapper around one `LeHandle*` (see backend/src/api/api.hpp)
/// - exists so LeTexture.swift never has to see api.hpp's raw C
/// declarations directly.
///
/// Swift files in a CocoaPods framework target do *not* automatically see
/// the target's own C headers the way a bridging header would provide (and
/// bridging headers are outright unsupported for framework targets - both
/// confirmed by trial while building this). A plain Objective-C interface
/// like this one, by contrast, is ordinary same-target ObjC<->Swift
/// interop, which Xcode has always supported with no extra plumbing - so
/// this class exists to move all direct api.hpp usage out of Swift's way,
/// not because the logic itself needs to be Objective-C.
@interface LeApiBridge : NSObject

- (instancetype)initWithHandleAddress:(int64_t)handleAddress NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Calls `le_render_pixel_buffer` and copies the result into a new
/// `kCVPixelFormatType_32BGRA` pixel buffer (see the .mm file for why BGRA,
/// not the RGBA `le_render_pixel_buffer` itself produces). Returns NULL if
/// the handle is invalid or the buffer is empty (e.g. no Design selected).
- (nullable CVPixelBufferRef)copyPixelBufferBGRA CF_RETURNS_RETAINED;

@end

NS_ASSUME_NONNULL_END
