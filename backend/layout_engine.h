
#ifndef LAYOUT_ENGINE_H
#define LAYOUT_ENGINE_H

// This header is now only for platform-specific rendering functions.
// The cross-platform FFI functions are in layout_engine_ffi.h
#include <CoreVideo/CoreVideo.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
    // Renders one frame of the pixel buffer
    EXPORT void render_pixel_buffer(void *layout_engine_ptr, CVPixelBufferRef pixel_buffer);
}
#endif // LAYOUT_ENGINE_H
