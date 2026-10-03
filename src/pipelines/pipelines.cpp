#include "blend2d_font.hpp"
#include "../core/resource_path.hpp"

#include <blend2d/blend2d.h>
#include <spdlog/spdlog.h>

// The pipelines module's one compiled TU (the rest is header-only) - for
// default_blend2d_font_face() below, whose static function-local cache
// needs a single definition.
#ifndef LE_FONT_DIR
#error "LE_FONT_DIR must be set by CMakeLists.txt"
#endif

namespace le
{
    const BLFontFace &default_blend2d_font_face()
    {
        static const BLFontFace face = []() -> BLFontFace
        {
            const auto path = find_resource(LE_FONT_DIR "/DejaVuSansMono.ttf", "fonts/DejaVuSansMono.ttf");
            if (!path)
            {
                spdlog::error("Label font DejaVuSansMono.ttf not found (tried {}) - text labels will render blank.", quoted_paths(path.error()));
                return BLFontFace();
            }
            BLFontFace f;
            if (const BLResult err = f.create_from_file(path->c_str()); err != BL_SUCCESS)
            {
                spdlog::error("Couldn't load label font '{}' (BLResult {}) - text labels will render blank.", *path, static_cast<unsigned>(err));
                return BLFontFace();
            }
            return f;
        }();
        return face;
    }
}
