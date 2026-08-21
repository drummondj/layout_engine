#include "lef_texture.h"

// Only present when CMakeLists.txt's LE_LINK_BACKEND option is ON (see its
// comment there for why that's off by default and what's actually blocking
// it - this isn't a "not implemented yet" stub, the real implementation
// below is complete, just not linkable against a working backend build on
// this dev machine yet). Falls back to copy_pixels always failing with a
// clear GError when off, so this target still links either way - Flutter's
// generated_plugins.cmake unconditionally expects a `layout_engine_plugin_plugin`
// target to exist and link successfully once pluginClass is declared in
// pubspec.yaml, unlike the ffiPlugin-only shared library, where missing
// backend symbols were fine until Dart actually looked them up.
#ifdef LE_LINK_BACKEND_ENABLED
#include "../src/layout_engine_plugin.h"
#endif

struct _FlLeTexture {
  FlPixelBufferTexture parent_instance;
  int64_t handle_address;
};

G_DEFINE_TYPE(FlLeTexture, fl_le_texture, fl_pixel_buffer_texture_get_type())

static gboolean fl_le_texture_copy_pixels(FlPixelBufferTexture* texture,
                                           const uint8_t** out_buffer, uint32_t* width,
                                           uint32_t* height, GError** error) {
#ifdef LE_LINK_BACKEND_ENABLED
  FlLeTexture* self = FL_LE_TEXTURE(texture);
  LeHandle* handle = reinterpret_cast<LeHandle*>(static_cast<uintptr_t>(self->handle_address));
  if (handle == nullptr) {
    g_set_error(error, g_quark_from_static_string("layout_engine_plugin"), 0, "null handle");
    return FALSE;
  }

  LePixelBuffer buf = le_render_pixel_buffer(handle);
  if (buf.data == nullptr || buf.width <= 0 || buf.height <= 0) {
    g_set_error(error, g_quark_from_static_string("layout_engine_plugin"), 0, "empty pixel buffer");
    return FALSE;
  }

  // FlPixelBufferTexture wants RGBA (see fl_pixel_buffer_texture.h's own
  // doc) - api.hpp's LePixelBuffer already is RGBA8888 (deliberately
  // platform-neutral, see its own comment), so unlike macOS's
  // kCVPixelFormatType_32BGRA-only CVPixelBuffer this needs no swizzle.
  //
  // buf.row_bytes may exceed buf.width * 4 (see LePixelBuffer's own
  // comment), but copy_pixels's contract is a tightly-packed RGBA buffer
  // with no stride of its own - if row_bytes isn't already tight, this
  // needs a real repack, not just a pointer handoff. Checked, not assumed:
  // Renderer::rasterize's kRGBA_8888_SkColorType raster surface is created
  // at exactly `width`, so its row_bytes today always equals width * 4 in
  // practice, but assert that rather than silently mis-rendering the day
  // that stops being true.
  if (buf.row_bytes != static_cast<int64_t>(buf.width) * 4) {
    g_set_error(error, g_quark_from_static_string("layout_engine_plugin"), 0,
                "row_bytes (%lld) isn't tightly packed for width %d - "
                "copy_pixels needs a real repack, not implemented",
                static_cast<long long>(buf.row_bytes), buf.width);
    return FALSE;
  }

  *out_buffer = buf.data;
  *width = static_cast<uint32_t>(buf.width);
  *height = static_cast<uint32_t>(buf.height);
  return TRUE;
#else
  g_set_error(error, g_quark_from_static_string("layout_engine_plugin"), 0,
              "built without LE_LINK_BACKEND - see this plugin's CLAUDE.md");
  return FALSE;
#endif
}

static void fl_le_texture_class_init(FlLeTextureClass* klass) {
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels = fl_le_texture_copy_pixels;
}

static void fl_le_texture_init(FlLeTexture* self) {}

FlLeTexture* fl_le_texture_new(int64_t handle_address) {
  FlLeTexture* self = FL_LE_TEXTURE(g_object_new(fl_le_texture_get_type(), nullptr));
  self->handle_address = handle_address;
  return self;
}
