#pragma once
#include <blend2d/blend2d.h>

namespace le
{
    /// @brief This process's one shared BLFontFace, memoized on first
    /// call, for RasterizeBlend2DStage's own text draws. Blend2D has no
    /// font-manager abstraction to scan a directory with - there's
    /// nothing to match/fall back within, so this loads one specific
    /// bundled file (LE_FONT_DIR/DejaVuSansMono.ttf) directly via
    /// BLFontFace::create_from_file, with an executable-relative fallback
    /// path on Linux (pipelines.cpp's own comment) for a packaged release
    /// build, where LE_FONT_DIR's compile-time path is never valid.
    /// Monospace is load-bearing, not incidental: RasterizeBlend2DStage's
    /// own per-character glyph-bitmap cache (rasterize_blend2d_stage.hpp's
    /// own GlyphBitmapCacheKey/draw_monospace_label_blend2d) lays a label
    /// out as fixed-width cells rather than reproducing Blend2D's own
    /// shaped-run kerning/positioning, which only produces correct layout
    /// for a font whose every glyph shares one advance width by
    /// construction. Defined in pipelines.cpp, the one TU LE_FONT_DIR is
    /// injected into (backend/CMakeLists.txt). Returns an invalid/empty
    /// BLFontFace (`face.is_empty()`) if no usable font could be found -
    /// callers skip drawing text in that case rather than crashing.
    const BLFontFace &default_blend2d_font_face();
}
