#ifndef LEF_TEXTURE_H_
#define LEF_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

// An FlPixelBufferTexture wrapping one `LeHandle*` (see
// backend/src/api/api.hpp) - pulls a frame via le_render_pixel_buffer()
// on demand (called on the render thread, only after
// layout_engine_plugin_plugin relays a markTextureFrameAvailable call - see
// layout_engine_plugin.cc's own comment for the full push/pull split, which
// mirrors ../macos/Classes/LeTexture.swift exactly). Does not own the
// handle - the Dart-side LeEditor that created it does, and must outlive
// every FlLeTexture built from it.
G_DECLARE_FINAL_TYPE(FlLeTexture, fl_le_texture, FL, LE_TEXTURE, FlPixelBufferTexture)

FlLeTexture* fl_le_texture_new(int64_t handle_address);

G_END_DECLS

#endif  // LEF_TEXTURE_H_
