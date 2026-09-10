#pragma once
#include <blend2d/blend2d.h>

namespace le
{
    /// @brief Blend2D analog of default_typeface() (default_typeface.hpp) -
    /// this process's one shared BLFontFace, memoized on first call, for
    /// RasterizeBlend2DStage's own text draws. Unlike Skia, Blend2D has no
    /// font-manager abstraction to scan a directory with
    /// (SkFontMgr_New_Custom_Directory's own job) - there's nothing to
    /// match/fall back within, so this loads one specific bundled file
    /// (LE_FONT_DIR/DejaVuSansMono.ttf) directly via BLFontFace::create_from_file,
    /// on every platform (not just Linux - default_typeface() only needs
    /// LE_FONT_DIR there since CoreText already has system fonts on macOS,
    /// but Blend2D has no CoreText-equivalent fallback on any platform).
    /// Deliberately monospace (unlike default_typeface()'s own proportional
    /// DejaVu Sans) - RasterizeBlend2DStage's own per-character
    /// glyph-bitmap cache (rasterize_blend2d_stage.hpp's own
    /// GlyphBitmapCacheKey/draw_monospace_label_blend2d) lays a label out
    /// as fixed-width cells rather than reproducing Blend2D's own shaped-
    /// run kerning/positioning, which only produces correct layout for a
    /// font whose every glyph shares one advance width by construction.
    /// Defined in pipelines.cpp, the one TU LE_FONT_DIR is injected into
    /// (backend/CMakeLists.txt). Returns an invalid/empty BLFontFace
    /// (`face.is_empty()`) if no usable font could be found - callers skip
    /// drawing text in that case rather than crashing, the same degrade
    /// default_typeface()'s own nullptr return gets on the Skia side.
    const BLFontFace &default_blend2d_font_face();
}
