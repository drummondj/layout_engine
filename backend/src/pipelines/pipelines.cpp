#include "blend2d_font.hpp"

#include <blend2d/blend2d.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <string>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

// One compiled TU (PIPELINE_REFACTOR.md's restart otherwise being
// header-only) - kept around purely for default_blend2d_font_face()
// below, now that RasterizeStage (Skia) and its own default_typeface()
// are gone (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md - RasterizeBlend2DStage
// is the only Rasterize backend); nothing here needs Skia's own
// SkFontMgr_mac_ct.h/ApplicationServices.h Carbon-typedef-collision
// isolation anymore, but a single compiled TU (rather than reverting this
// target back to INTERFACE) is still convenient for a static function-
// local cache like this one.
#ifndef LE_FONT_DIR
#error "LE_FONT_DIR must be set by backend/CMakeLists.txt"
#endif

namespace le
{
    const BLFontFace &default_blend2d_font_face()
    {
        static const BLFontFace face = []() -> BLFontFace
        {
            BLFontFace f;
            const std::string primary_path = std::string(LE_FONT_DIR) + "/DejaVuSansMono.ttf";
            BLResult err = f.create_from_file(primary_path.c_str());
            if (err == BL_SUCCESS)
            {
                spdlog::info("default_blend2d_font_face(): loaded '{}'", primary_path);
                return f;
            }
            spdlog::warn("default_blend2d_font_face(): failed to load '{}' (BLResult {})", primary_path, static_cast<unsigned>(err));

#if defined(__linux__)
            // Executable-relative fallback: LE_FONT_DIR is a compile-time
            // path (this build machine's own backend/assets/fonts), never
            // valid on a machine a packaged release bundle gets copied to;
            // Dockerfile.linux-release's own bundle stage copies
            // assets/fonts/ next to the running executable specifically
            // for this fallback to find.
            char buf[PATH_MAX];
            const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (len > 0)
            {
                buf[len] = '\0';
                const std::string exe_path(buf);
                const size_t slash = exe_path.find_last_of('/');
                const std::string exe_dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : exe_path.substr(0, slash));
                const std::string fallback_path = exe_dir + "/fonts/DejaVuSansMono.ttf";
                BLFontFace f2;
                err = f2.create_from_file(fallback_path.c_str());
                if (err == BL_SUCCESS)
                {
                    spdlog::info("default_blend2d_font_face(): loaded '{}'", fallback_path);
                    return f2;
                }
                spdlog::warn("default_blend2d_font_face(): failed to load '{}' (BLResult {})", fallback_path, static_cast<unsigned>(err));
            }
            else
            {
                spdlog::warn("default_blend2d_font_face(): readlink(\"/proc/self/exe\") failed (errno {}) - "
                              "can't compute the executable-relative font fallback path", errno);
            }
#endif
            spdlog::error("default_blend2d_font_face(): FAILED - no usable font found, every Blend2D-backed text label will render blank.");
            return BLFontFace();
        }();
        return face;
    }
}
