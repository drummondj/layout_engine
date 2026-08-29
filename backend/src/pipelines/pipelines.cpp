#include "draw_helpers.hpp"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkString.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <string>

// See draw_helpers.hpp's comment on default_typeface() for why this is
// isolated to its own translation unit: ApplicationServices.h (pulled in by
// SkFontMgr_mac_ct.h) defines legacy Carbon Rect/Point/Polygon typedefs
// that collide with le::Rect/le::Point/le::Polygon wherever a file does
// `using namespace le`. SkFontMgr_directory.h doesn't have that problem, but
// it's included here anyway to keep both platforms' font-manager headers in
// the one .cpp that's allowed to know about them, rather than splitting the
// isolation rule across files.
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(__linux__)
// A bundled font directory (LE_FONT_DIR, injected by backend/CMakeLists.txt
// - defaults to backend/assets/fonts, committed to the repo), not
// SkFontMgr_New_FontConfig - deliberately, for portability to a locked-down
// target machine we can't assume has any working system fontconfig config
// or installed fonts at all (a rootless-Rocky-8-build concern, not just a
// Docker-CI one). A missing/unmatched font under fontconfig fails silently
// (matchFamilyStyle just returns null, every label renders blank, no
// error) - shipping the font file removes that runtime dependency
// entirely, at the cost of only ever having this one bundled family
// available. SkFontMgr_New_Custom_Directory scans LE_FONT_DIR itself for
// .ttf/.ttc/.otf/.pfb files and uses FreeType for rasterization internally
// - no separate SkFontScanner argument needed, unlike the fontconfig path
// this replaced.
#include "include/ports/SkFontMgr_directory.h"
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef LE_FONT_DIR
#error "LE_FONT_DIR must be set by backend/CMakeLists.txt"
#endif
#endif

namespace le
{
    sk_sp<SkTypeface> default_typeface()
    {
        static const sk_sp<SkTypeface> typeface = []() -> sk_sp<SkTypeface>
        {
#if defined(__APPLE__)
            sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_CoreText(nullptr);
            return font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
#elif defined(__linux__)
            // Deliberately *not* matchFamilyStyle(nullptr, ...) - unlike
            // CoreText/fontconfig, SkFontMgr_Custom's nullptr handling
            // (SkFontMgr_custom.cpp's onMatchFamilyStyle -> matchFamily ->
            // onMatchFamily) only matches by *name* (empty on no match), and
            // never falls back to the family it separately picks as
            // "default" in its own constructor - which only checks a
            // hardcoded name list ("Arial"/"Verdana"/.../"DejaVu Serif")
            // that doesn't include "DejaVu Sans", this project's own
            // bundled family. Confirmed via gdb: matchFamilyStyle(nullptr,
            // ...) returns null even though the font manager itself loaded
            // the bundled font successfully. createStyleSet(0) sidesteps
            // name-matching entirely - a candidate directory only ever has
            // the one bundled family, so "family at index 0" is unambiguous.
            // Every branch below logs *something* - success included, not
            // just failure. A previous version of this function only ever
            // logged on total failure, so a report of "text isn't
            // rendering" against silent (non-error) logs was genuinely
            // ambiguous: did font *loading* fail (this function's own
            // concern), or did it succeed but glyph *rasterization*
            // further down the pipeline fail for some other reason (a
            // different bug entirely)? Logging the success path closes
            // that ambiguity for the next report.
            auto try_font_dir = [](const std::string& dir) -> sk_sp<SkTypeface>
            {
                // Checked explicitly, before ever constructing a
                // SkFontMgr: confirmed via a real report that
                // SkFontMgr_New_Custom_Directory does NOT fail cleanly
                // (empty countFamilies()/null matchStyle()) when given a
                // directory that doesn't exist at all - it instead
                // produces a *non-null* SkTypeface with an empty family
                // name and no real glyph data, which this function used
                // to treat as success, short-circuiting before ever
                // trying the correct fallback directory. Since LE_FONT_DIR
                // is a compile-time path that's frequently wrong at
                // runtime (that's the whole reason the fallback below
                // exists), this isn't a hypothetical edge case - it's the
                // expected common case on a machine other than the one
                // that built the binary.
                struct stat st{};
                if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                {
                    spdlog::warn("default_typeface(): '{}' does not exist or is not a directory", dir);
                    return nullptr;
                }
                sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_Custom_Directory(dir.c_str());
                int family_count = font_mgr->countFamilies();
                if (family_count == 0)
                {
                    spdlog::warn("default_typeface(): no font families found in '{}'", dir);
                    return nullptr;
                }
                sk_sp<SkFontStyleSet> style_set = font_mgr->createStyleSet(0);
                if (style_set == nullptr)
                {
                    spdlog::warn("default_typeface(): '{}' has {} font famil{} but createStyleSet(0) returned null",
                                 dir, family_count, family_count == 1 ? "y" : "ies");
                    return nullptr;
                }
                sk_sp<SkTypeface> face = style_set->matchStyle(SkFontStyle());
                if (face == nullptr)
                {
                    spdlog::warn("default_typeface(): '{}' has {} font famil{} but matchStyle() returned null "
                                 "(the .ttf/.otf file(s) there may have failed to parse)",
                                 dir, family_count, family_count == 1 ? "y" : "ies");
                    return nullptr;
                }
                SkString family_name;
                face->getFamilyName(&family_name);
                if (family_name.isEmpty())
                {
                    // Belt-and-braces alongside the stat() check above -
                    // this is the exact symptom that check was added to
                    // catch (a "successful" but degenerate typeface with
                    // no real family name), kept as a second guard in
                    // case some other, not-yet-seen input shape produces
                    // the same degenerate result a missing directory did.
                    spdlog::warn("default_typeface(): '{}' produced a typeface with no family name "
                                 "(family_count={}) - treating as not found", dir, family_count);
                    return nullptr;
                }
                spdlog::info("default_typeface(): loaded '{}' from '{}'", family_name.c_str(), dir);
                return face;
            };

            // LE_FONT_DIR is a compile-time path (this build machine's own
            // backend/assets/fonts) - correct for ctest/render_preview/local
            // dev, where that source tree genuinely exists, but never valid
            // on a machine a packaged release bundle gets copied to. Fall
            // back to the font bundled next to the running executable
            // instead (flutter_plugin/linux/CMakeLists.txt's own
            // LE_RUNTIME_BUNDLED_FILES bundles backend/assets/fonts/*.ttf
            // into the app's lib/ directory, same place le_tcl.so/
            // le_tcl_procs.tcl land - see layout_engine_plugin.cc's
            // ExecutableDir() for the equivalent Tcl-console fix this
            // mirrors) - confirmed necessary by a real report of every text
            // label rendering blank on a release build.
            sk_sp<SkTypeface> face = try_font_dir(LE_FONT_DIR);
            if (face == nullptr)
            {
                char buf[PATH_MAX];
                ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
                std::string exe_dir;
                if (len > 0)
                {
                    buf[len] = '\0';
                    std::string exe_path(buf);
                    size_t slash = exe_path.find_last_of('/');
                    if (slash == std::string::npos)
                    {
                        exe_dir = ".";
                    }
                    else if (slash == 0)
                    {
                        // The executable lives directly under / (e.g.
                        // "/layout_engine", exe_path[0..slash] would
                        // otherwise be an empty substring - confirmed by
                        // hitting exactly this in a throwaway test
                        // harness placed at filesystem root: the fallback
                        // was silently skipped entirely (empty string was
                        // indistinguishable from readlink() itself
                        // failing, both handled by the `if
                        // (!exe_dir.empty())` check below). Unlikely for a
                        // real install, but cheap to get right.
                        exe_dir = "/";
                    }
                    else
                    {
                        exe_dir = exe_path.substr(0, slash);
                    }
                }
                else
                {
                    spdlog::warn("default_typeface(): readlink(\"/proc/self/exe\") failed (errno {}) - "
                                 "can't compute the executable-relative font fallback path", errno);
                }
                if (!exe_dir.empty())
                {
                    face = try_font_dir(exe_dir + "/lib");
                }
            }
            if (face == nullptr)
            {
                // Not a hard failure (matches this function's existing
                // "degrade rather than throw" contract - see draw_helpers.hpp)
                // but every terminal/pin/ruler label silently renders blank
                // if this ever fires, which is otherwise very hard to
                // diagnose on a machine we can't reproduce against directly -
                // see backend/CLAUDE.md's Build section for LE_FONT_DIR. The
                // two warn() lines above (one per candidate directory) carry
                // the actual "why" (missing directory vs. empty vs. a file
                // present but unparseable); this line is deliberately just a
                // loud, unmissable summary that every label is about to
                // render blank, not a duplicate of that detail.
                spdlog::error("default_typeface(): FAILED - no usable font found, every text label will render blank. "
                              "See the warn() line(s) immediately above for which candidate directories failed and why.");
            }
            return face;
#else
            return nullptr;
#endif
        }();
        return typeface;
    }
}
