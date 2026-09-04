#pragma once
#include "include/core/SkTypeface.h"

namespace le
{
    /// @brief This process's one shared text typeface, memoized on first
    /// call - resolved via a platform font manager (CoreText on macOS, a
    /// bundled font directory on Linux) rather than the vendored Skia
    /// checkout's own default, which has no usable font on a machine with
    /// no system fonts installed. Defined in pipelines.cpp, the one TU
    /// allowed to include a platform font-manager header directly
    /// (SkFontMgr_mac_ct.h pulls in ApplicationServices.h, which defines
    /// legacy Carbon Rect/Point/Polygon typedefs that collide with
    /// le::Rect/le::Point/le::Polygon wherever a file does
    /// `using namespace le`) - every other stage just calls this function.
    /// Returns nullptr if no usable font could be found (degrades to
    /// blank text labels, not a crash - see pipelines.cpp's own comment).
    sk_sp<SkTypeface> default_typeface();
}
