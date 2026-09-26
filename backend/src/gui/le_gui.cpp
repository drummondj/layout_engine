#include "le_gui.hpp"

#include "api.hpp"
#include "gui_provider.hpp"
#include "components/status_bar.hpp"
#include "components/library_browser.hpp"
#include "components/property_viewer.hpp"
#include "components/layer_manager.hpp"
#include "components/mode_selector.hpp"
#include "components/mode_toolbar.hpp"
#include "components/secondary_toolbar.hpp"
#include "components/settings_panel.hpp"
#include "components/info_panel.hpp"
#include "components/icon_font.hpp"

// Apple deprecated the whole OpenGL framework in favor of Metal (10.14+)
// but still fully implements it - every desktop-GL ImGui backend still
// targets it the same way, this is a purely informational warning.
#define GL_SILENCE_DEPRECATION

#include "imgui.h"
// DockBuilder* (imgui_internal.h, "internal" API - not exported from
// imgui.h) is the standard, documented way to script a *default* dock
// layout the first time a window opens (left/center/right, mirroring
// the Flutter frontend's own default docking layout in home.dart's
// _buildDefaultLayout) - see setup_default_dock_layout below. Everything
// else this file uses comes from imgui.h alone.
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
// ICON_LC_*/ICON_MIN_LC/ICON_MAX_LC - see this file's own font-merge
// setup below (open_and_run_window) and CMakeLists.txt's own comment on
// why this exact header is pinned to the Lucide release it was
// generated from.
#include "IconsLucide.h"
// Declares every GL function this file calls (glGenTextures/glTexImage2D/
// etc - all part of OpenGL 1.1's core spec, so no external loader library
// is needed) via its own bundled minimal loader on platforms that need
// one (Linux) or the system GL headers directly (macOS) - no separate
// <OpenGL/gl3.h>/<GL/gl.h> include of our own needed (and actively wrong
// to add one: it collides with this header's own, tripping a real
// "gl.h and gl3.h both included" warning on macOS).
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace le::gui
{
    namespace
    {
        // How often the idle loop polls le_take_show_gui_request() while
        // no window is open - `show_gui` itself returns immediately
        // either way, this only bounds how long a caller might have to
        // wait to actually *see* the window appear. Cheap enough (a
        // single atomic exchange) that a short interval costs nothing
        // measurable while idle.
        constexpr auto kIdlePollInterval = std::chrono::milliseconds(30);

        // LE_LUCIDE_FONT_PATH (CMakeLists.txt) is this build machine's
        // own absolute FetchContent cache path - correct for a local
        // dev/ctest run where that cache dir genuinely still exists, but
        // never valid once le_shell is copied to another machine (the
        // exact bug pipelines.cpp's default_blend2d_font_face() already
        // has this same two-step fallback for, on Linux, for the exact
        // same reason - a real report of AddFontFromFileTTF failing
        // outright in a Linux release build, both icon and body text).
        // Checked via stat() first, on both platforms, so a missing file
        // is diagnosed by us (a clean spdlog::warn + graceful skip - icons
        // just don't render, matching draw_helpers.hpp's own "degrade
        // rather than throw" contract) instead of reaching
        // AddFontFromFileTTF at all, which logs its own ImGui-internal
        // "Could not load font file!" error/assert and returns null
        // either way - our own check is strictly more informative (names
        // every candidate path actually tried, mirroring
        // default_blend2d_font_face()'s own fallback).
        //
        // The second candidate - right next to the running executable -
        // only exists on Linux: CMakeLists.txt's own file(COPY ...) right
        // after FetchContent_MakeAvailable(lucide_font) puts a real copy
        // of lucide.ttf in the build tree alongside le_shell specifically
        // so this works, and Dockerfile.linux-release's own bundle stage
        // copies that same file into the shipped release bundle
        // alongside le_shell too. macOS never needs this second
        // candidate - a real report would be needed before adding
        // platform-specific bundling for it, matching this project's own
        // "fix confirmed bugs, don't speculatively harden" convention.
        std::string resolve_lucide_font_path()
        {
            struct stat st{};
            if (stat(LE_LUCIDE_FONT_PATH, &st) == 0)
                return LE_LUCIDE_FONT_PATH;
            spdlog::warn("resolve_lucide_font_path(): '{}' does not exist", LE_LUCIDE_FONT_PATH);

#if defined(__linux__)
            char buf[PATH_MAX];
            const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (len <= 0)
            {
                spdlog::warn("resolve_lucide_font_path(): readlink(\"/proc/self/exe\") failed (errno {}) - "
                             "can't compute the executable-relative icon-font fallback path",
                             errno);
                return {};
            }
            buf[len] = '\0';
            const std::string exe_path(buf);
            const size_t slash = exe_path.find_last_of('/');
            const std::string exe_dir = slash == std::string::npos ? "." : slash == 0 ? "/"
                                                                                        : exe_path.substr(0, slash);
            const std::string candidate = exe_dir + "/lucide.ttf";
            if (stat(candidate.c_str(), &st) == 0)
                return candidate;
            spdlog::warn("resolve_lucide_font_path(): '{}' does not exist either", candidate);
#endif
            return {};
        }

        // A dark, low-saturation ("pastel") ImGui theme - applied once,
        // right after ImGui::CreateContext() below, in place of Dear
        // ImGui's own built-in default style.
        void set_dark_pastel_imgui_style()
        {
            ImGuiStyle &style = ImGui::GetStyle();
            ImVec4 *colors = style.Colors;

            // Backgrounds
            colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.13f, 0.15f, 1.00f); // Dark grey base
            colors[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 0.95f);
            colors[ImGuiCol_Border] = ImVec4(0.30f, 0.33f, 0.42f, 0.40f);

            // Text
            colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
            colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.65f, 0.70f, 1.00f);

            // Headers
            colors[ImGuiCol_Header] = ImVec4(0.36f, 0.42f, 0.55f, 0.60f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.44f, 0.50f, 0.68f, 0.80f);
            colors[ImGuiCol_HeaderActive] = ImVec4(0.46f, 0.55f, 0.75f, 1.00f);

            // Buttons
            colors[ImGuiCol_Button] = ImVec4(0.28f, 0.34f, 0.48f, 0.70f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.36f, 0.45f, 0.65f, 0.85f);
            colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.50f, 0.70f, 1.00f);

            // Frames
            colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.32f, 0.42f, 1.00f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.38f, 0.50f, 1.00f);

            // Tabs
            colors[ImGuiCol_Tab] = ImVec4(0.26f, 0.30f, 0.42f, 0.80f);
            colors[ImGuiCol_TabHovered] = ImVec4(0.36f, 0.42f, 0.58f, 1.00f);
            colors[ImGuiCol_TabActive] = ImVec4(0.42f, 0.50f, 0.68f, 1.00f);
            colors[ImGuiCol_TabUnfocused] = ImVec4(0.20f, 0.24f, 0.32f, 0.80f);
            colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.30f, 0.36f, 0.50f, 1.00f);

            // Titles
            colors[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.25f, 0.30f, 1.00f);
            colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.30f, 0.40f, 1.00f);
            colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.12f, 0.15f, 0.75f);

            // Scrollbars
            colors[ImGuiCol_ScrollbarBg] = ImVec4(0.13f, 0.14f, 0.18f, 1.00f);
            colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.30f, 0.38f, 0.60f);
            colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.40f, 0.50f, 0.80f);
            colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.50f, 0.65f, 1.00f);

            // Checkboxes / Radios
            colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.85f, 1.00f, 1.00f);

            // Sliders
            colors[ImGuiCol_SliderGrab] = ImVec4(0.50f, 0.65f, 0.90f, 1.00f);
            colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.75f, 1.00f, 1.00f);

            // Resize grip
            colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.40f, 0.50f, 0.60f);
            colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.40f, 0.50f, 0.60f, 0.80f);
            colors[ImGuiCol_ResizeGripActive] = ImVec4(0.50f, 0.60f, 0.80f, 1.00f);

            // Separators
            colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.40f, 0.48f, 0.70f);
            colors[ImGuiCol_SeparatorHovered] = ImVec4(0.50f, 0.60f, 0.72f, 0.90f);
            colors[ImGuiCol_SeparatorActive] = ImVec4(0.65f, 0.70f, 0.85f, 1.00f);

            // Menu bar
            colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);

            // Drag & drop
            colors[ImGuiCol_DragDropTarget] = ImVec4(0.50f, 0.85f, 1.00f, 0.90f);

            // Shape metrics
            style.WindowRounding = 8.0f;
            style.ChildRounding = 6.0f;
            style.FrameRounding = 5.0f;
            style.PopupRounding = 6.0f;
            style.ScrollbarRounding = 5.0f;
            style.GrabRounding = 4.0f;
            style.TabRounding = 5.0f;

            style.WindowBorderSize = 0.0f;
            style.FrameBorderSize = 0.0f;
            style.PopupBorderSize = 1.0f;

            style.WindowPadding = ImVec2(16, 16);
            style.FramePadding = ImVec2(10, 6);
            style.ItemSpacing = ImVec2(10, 10);
            style.ItemInnerSpacing = ImVec2(6, 4);
            style.IndentSpacing = 20.0f;
        }

        // The loading spinner - a horizontal row of 12 rectangle segments
        // (a segmented loading bar / equalizer look), used while a render
        // is in flight (open_and_run_window's own main loop, below).
        // Replaces an earlier design (draw_rotated_glyph, since removed -
        // see git history) that continuously rotated a single icon-font
        // glyph via a rotated textured quad: that approach visibly
        // shimmered/wobbled along the glyph's own thin ring stroke no
        // matter how it was supersampled, a standard raster-rotation
        // aliasing artifact. A second design (also since removed - a
        // circular ring of 12 bars at fixed angles) avoided the rotation
        // aliasing but looked low-quality at this size (thin radiating
        // segments read poorly at a small on-screen footprint).
        //
        // This design keeps the same "nothing ever rotates, only opacity
        // cycles" principle (a plain alpha lerp can't alias) but as a
        // plain horizontal row of axis-aligned rectangles - each simpler
        // to draw (AddRectFilled, no per-segment trigonometry needed for
        // its own position/orientation the way the ring's radiating bars
        // needed) and, at a glance, a more familiar "loading bar" shape.
        // The "loading" look comes from a highlight that sweeps left to
        // right across the 12 segments and wraps back to the start.
        void draw_loading_spinner(ImDrawList *draw_list, ImVec2 center, float time_seconds)
        {
            constexpr int kSegmentCount = 12;
            constexpr float kSegmentWidth = 6.0f;
            constexpr float kSegmentHeight = 16.0f;
            constexpr float kSegmentSpacing = 3.0f;
            constexpr float kCyclesPerSecond = 1.2f;
            // Never fully transparent even at the tail, so all 12
            // segments stay individually visible, matching a standard
            // loading-bar look rather than fading to nothing.
            constexpr float kMinAlpha = 0.15f;

            const float total_width = kSegmentCount * kSegmentWidth + (kSegmentCount - 1) * kSegmentSpacing;
            const float start_x = center.x - total_width * 0.5f;
            const float y0 = center.y - kSegmentHeight * 0.5f;
            const float y1 = center.y + kSegmentHeight * 0.5f;

            const float lead = std::fmod(time_seconds * kCyclesPerSecond, 1.0f);
            for (int i = 0; i < kSegmentCount; ++i)
            {
                const float segment_fraction = static_cast<float>(i) / static_cast<float>(kSegmentCount);

                // How far *behind* the lead position this segment is,
                // wrapped into [0, 1) - 0 right at the lead, approaching 1
                // all the way back around to it (left edge wraps to the
                // right edge, a continuous sweep rather than a bounce).
                float behind = segment_fraction - lead;
                behind -= std::floor(behind);
                const float brightness = 1.0f - behind;
                const float alpha = kMinAlpha + (1.0f - kMinAlpha) * brightness * brightness;

                const float x0 = start_x + static_cast<float>(i) * (kSegmentWidth + kSegmentSpacing);
                const float x1 = x0 + kSegmentWidth;
                const ImU32 color = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f));
                draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);
            }
        }

        // Every physical key this prototype forwards to the backend,
        // mapped to its own LeKeyCode - see api.hpp's own LeKeyCode/
        // le_key_down doc comments for the full per-code modifier-gating
        // table (checked internally via le_is_key_held(LE_KEY_CTRL/
        // LE_KEY_SHIFT), not by this table - this loop's only job is
        // keeping LE_KEY_CTRL/LE_KEY_SHIFT's own held-state in sync
        // alongside every other key, not deciding what a modifier means).
        struct KeyMapping
        {
            ImGuiKey imgui_key;
            int32_t le_key_code;
        };
        constexpr KeyMapping kKeyMappings[] = {
            {ImGuiKey_Z, LE_KEY_ZOOM},
            {ImGuiKey_F, LE_KEY_FIT},
            {ImGuiKey_LeftArrow, LE_KEY_PAN_LEFT},
            {ImGuiKey_RightArrow, LE_KEY_PAN_RIGHT},
            {ImGuiKey_UpArrow, LE_KEY_PAN_UP},
            {ImGuiKey_DownArrow, LE_KEY_PAN_DOWN},
            {ImGuiKey_A, LE_KEY_SELECT_ALL},
            {ImGuiKey_D, LE_KEY_DESELECT_ALL},
            {ImGuiKey_1, LE_KEY_1},
            {ImGuiKey_2, LE_KEY_2},
            {ImGuiKey_3, LE_KEY_3},
            {ImGuiKey_4, LE_KEY_4},
            {ImGuiKey_5, LE_KEY_5},
            {ImGuiKey_6, LE_KEY_6},
            {ImGuiKey_7, LE_KEY_7},
            {ImGuiKey_8, LE_KEY_8},
            {ImGuiKey_9, LE_KEY_9},
            {ImGuiKey_0, LE_KEY_0},
            {ImGuiKey_S, LE_KEY_SELECT_MODE},
            {ImGuiKey_E, LE_KEY_EDIT_MODE},
            {ImGuiKey_R, LE_KEY_RULER_MODE},
            {ImGuiKey_Escape, LE_KEY_FINISH_RULER},
            {ImGuiKey_M, LE_KEY_MOVE},
        };

        // Forwards every currently-pressed/released/held key this frame
        // to the backend - modifiers (Ctrl/Shift) first, since every
        // action code's own gating (le_key_down's doc comment) reads
        // their held-state at the moment it's called. `repeat=true`
        // (ImGui's own default) matches le_key_down's own "call on
        // key-down *and* key-repeat" contract for the canvas-navigation
        // codes (zoom/fit/pan keep re-triggering while held, same as a
        // real keyboard's own repeat).
        //
        // `active` (BUGS_AND_ENHANCEMENTS.md B7) - the caller passes
        // `layout view hovered && !io.WantTextInput`, so this only
        // forwards while the mouse is actually over the layout/design
        // view AND no ImGui text-editing widget currently has focus.
        // `io.WantCaptureKeyboard` (tried first) is the wrong flag here
        // despite the similar name - its own doc comment says it's also
        // set whenever "an imgui window is focused and navigation is
        // enabled", true for this whole docked app almost all the time,
        // not just while actively typing - gating on it broke every
        // shortcut outright. `io.WantTextInput` is the narrower one,
        // "set by Dear ImGui when it wants textual keyboard input to
        // happen (e.g. when an InputText widget is active)" - exactly
        // "is some text field actively capturing keystrokes right now".
        // Without this check at all, typing "1"/"2" into
        // layer_manager.hpp's own "Hierarchy Depth" InputInt field also
        // reached this function unconditionally - LE_KEY_1/LE_KEY_2 mean
        // "toggle a ROUTING layer's visibility" to the backend
        // (LeKeyCode's own doc comment), so every digit typed into that
        // field silently toggled a layer too, the original bug reported.
        // When `active` transitions to false, `le_clear_all_keys` fires
        // once - the matching key-up for whatever was held at that
        // moment (e.g. Shift, mid-multi-select) isn't guaranteed to
        // still reach this function once forwarding stops, so without
        // this a modifier could stay "held" from the backend's own point
        // of view indefinitely (le_clear_all_keys's own doc comment).
        void forward_keyboard_input(GuiProvider &provider, bool active)
        {
            static bool was_active = false;
            static bool ctrl_was_held = false;
            static bool shift_was_held = false;

            if (!active)
            {
                if (was_active)
                {
                    provider.clear_all_keys();
                    ctrl_was_held = false;
                    shift_was_held = false;
                }
                was_active = false;
                return;
            }
            was_active = true;

            ImGuiIO &io = ImGui::GetIO();
            if (io.KeyCtrl != ctrl_was_held)
            {
                if (io.KeyCtrl)
                    provider.key_down(LE_KEY_CTRL);
                else
                    provider.key_up(LE_KEY_CTRL);
                ctrl_was_held = io.KeyCtrl;
            }
            if (io.KeyShift != shift_was_held)
            {
                if (io.KeyShift)
                    provider.key_down(LE_KEY_SHIFT);
                else
                    provider.key_up(LE_KEY_SHIFT);
                shift_was_held = io.KeyShift;
            }

            for (const KeyMapping &mapping : kKeyMappings)
            {
                if (ImGui::IsKeyPressed(mapping.imgui_key))
                    provider.key_down(mapping.le_key_code);
                if (ImGui::IsKeyReleased(mapping.imgui_key))
                    provider.key_up(mapping.le_key_code);
            }
        }

        // Mouse gesture state, local to one open_and_run_window() call -
        // which button (if any) started the currently-in-progress
        // gesture, so the matching release calls le_mouse_up() rather
        // than a second unmatched le_mouse_down()/le_zoom_drag_down().
        enum class ActiveGesture
        {
            kNone,
            kSelect,
            kZoomDrag,
        };

        // Forwards this frame's mouse position/buttons/wheel to the
        // backend, but only while the image widget just drawn is
        // actually hovered - the image fills the whole window for now,
        // so this is mostly moot today, but keeps this correct once
        // menus/toolbars (mentioned as later work) start sharing the
        // window with the design view. `scale_x`/`scale_y` convert from
        // ImGui's own logical/window coordinate space into the backend's
        // own pixel space (matching le_set_viewport_size's own
        // framebuffer-pixel units) - they differ on a HiDPI/Retina
        // display, where the framebuffer has more real pixels than
        // logical points. Returns `hovered` - the caller also gates
        // forward_keyboard_input on it (BUGS_AND_ENHANCEMENTS.md B7).
        bool forward_mouse_input(GuiProvider &provider, ActiveGesture &gesture, float scale_x, float scale_y)
        {
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 origin = ImGui::GetItemRectMin();
            const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
            const int32_t px = static_cast<int32_t>((mouse_pos.x - origin.x) * scale_x);
            const int32_t py = static_cast<int32_t>((mouse_pos.y - origin.y) * scale_y);

            static bool was_hovered = false;
            if (hovered)
            {
                provider.set_mouse_position(px, py);
            }
            else if (was_hovered)
            {
                provider.clear_mouse_position();
            }
            was_hovered = hovered;

            if (gesture == ActiveGesture::kNone && hovered)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    provider.mouse_down(px, py);
                    gesture = ActiveGesture::kSelect;
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    provider.zoom_drag_down(px, py);
                    gesture = ActiveGesture::kZoomDrag;
                }
            }
            else if (gesture == ActiveGesture::kSelect && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                provider.mouse_up(px, py);
                gesture = ActiveGesture::kNone;
            }
            else if (gesture == ActiveGesture::kZoomDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                provider.mouse_up(px, py);
                gesture = ActiveGesture::kNone;
            }

            // A signed fractional step per wheel tick, matching le_zoom's
            // own "positive zooms in, negative zooms out" convention -
            // smaller than the GUI toolbar's own fixed 0.3-per-keypress
            // step (UPDATES.md's zoom command doc), since a scroll
            // gesture delivers many ticks in quick succession.
            constexpr float kZoomStepPerWheelTick = 0.15f;
            const float wheel = ImGui::GetIO().MouseWheel;
            if (hovered && wheel != 0.0f)
            {
                provider.zoom(wheel * kZoomStepPerWheelTick, px, py);
            }

            return hovered;
        }

        // Upper bound (seconds) on how long the main loop's own
        // glfwWaitEventsTimeout() below blocks before redrawing anyway -
        // a real, reported bug: the main loop used to call the
        // non-blocking glfwPollEvents() and then unconditionally redraw
        // the whole ImGui frame + do a full GL render every single
        // iteration, with no idle throttling at all. glfwSwapInterval(1)
        // (vsync) doesn't reliably cap this on a machine with no real
        // GPU (confirmed via `top -H` on a live le_shell: dozens of Mesa
        // "llvmpipe" software-rasterizer threads, one per CPU core, each
        // sitting at ~18-27% CPU continuously, even with the mouse
        // untouched and nothing on screen changing) - llvmpipe's own
        // software swap path has no real display refresh signal to sync
        // to, so the loop just free-spins, re-rasterizing the entire
        // (unchanged) UI in software as fast as it possibly can.
        // glfwWaitEventsTimeout() blocks (genuinely sleeping, not
        // polling) until either a real input event arrives - identical
        // responsiveness to glfwPollEvents() for actual interaction,
        // since any real event wakes it immediately - or this timeout
        // elapses, which is what now bounds the idle redraw rate instead
        // of leaving it unbounded. 33ms (~30Hz) matches
        // kRenderThreadIdleInterval above, both existing for the same
        // reason: cheap enough to never feel laggy, short enough that a
        // background render-thread frame (le_is_rendering()'s own
        // spinner, or a newly-published mailbox frame) still gets picked
        // up and drawn promptly rather than sitting unseen until the
        // next real input event.
        constexpr double kMainLoopIdleWaitSeconds = 0.033;

        // Logical (window/point, not framebuffer-pixel) height reserved
        // at the bottom of the window for draw_status_bar
        // (components/status_bar.hpp) - the ImGui port of
        // frontend/lib/components/status_bar.dart, which sits directly
        // below the design view the same way in home.dart's own layout.
        // A fixed constant, not measured, since this prototype's status
        // bar is one fixed-height row (Separator + one line of text) -
        // ImGui itself only reports an item's actual size *after*
        // drawing it, so getting this exactly without a fixed guess
        // would need drawing the whole frame twice.
        constexpr float kStatusBarHeight = 36.0f;

        // The one slot a background render thread publishes into and the
        // main/GUI thread reads from - decouples le_render_pixel_buffer()
        // (which can take anywhere from microseconds to over a second for
        // a real design on a scale change - see BENCHMARKS.md's own
        // RenderLayoutFrame entries) from GLFW's own event loop and
        // window repaint, mirroring the same raster-thread/platform-
        // thread split Flutter's own texture pull already used (see
        // is_rendering_'s own doc comment, api.cpp) - without this, a
        // single slow render call blocks *everything* on the thread that
        // also owns polling input and drawing the window, freezing the
        // whole app for its own full duration instead of just delaying
        // the next visible frame while input/repaint keep working.
        //
        // `pixels` is a plain byte copy of the handle's own returned
        // buffer, made because LePixelBuffer.data is only valid until the
        // *next* le_render_pixel_buffer() call on the same handle (its
        // own doc comment) - and this same background thread is about to
        // make exactly that next call, on its own next loop iteration,
        // arbitrarily soon. `generation` lets the main thread notice a
        // new frame arrived (and skip a redundant GL upload when nothing
        // has, which is the common case at idle) without needing its own
        // copy of the pixels to compare byte-for-byte.
        struct RenderMailbox
        {
            std::mutex mutex;
            std::vector<uint8_t> pixels;
            int width = 0;
            int height = 0;
            int64_t row_bytes = 0;
            uint64_t generation = 0;
        };

        // Event-driven, not a fixed-interval poll: le_wait_for_render_needed
        // (api.hpp) blocks with zero CPU cost until a real mutation has
        // been made to `handle` since the last time it returned, or until
        // le_cancel_render_wait wakes it for shutdown (open_and_run_window's
        // own teardown, below) - replacing a former "call
        // le_render_pixel_buffer back-to-back forever, sleeping a fixed
        // interval between calls" loop that burned continuous CPU even at
        // total idle and depended on tuning that sleep interval to some
        // machine's own load (a real, reported problem - direct
        // instrumentation once measured over 6500 calls in 244 seconds at
        // total idle, almost all of them no-ops). Cause and effect instead
        // of polling: nothing runs here until something actually changed.
        void render_thread_loop(LeHandle *handle, RenderMailbox &mailbox, std::atomic<bool> &stop)
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                le_wait_for_render_needed(handle);
                if (stop.load(std::memory_order_relaxed))
                    break;

                const LePixelBuffer buffer = le_render_pixel_buffer(handle);
                if (buffer.data != nullptr && buffer.width > 0 && buffer.height > 0)
                {
                    const size_t byte_count = static_cast<size_t>(buffer.row_bytes) * static_cast<size_t>(buffer.height);
                    std::lock_guard<std::mutex> lock(mailbox.mutex);
                    mailbox.pixels.resize(byte_count);
                    std::memcpy(mailbox.pixels.data(), buffer.data, byte_count);
                    mailbox.width = buffer.width;
                    mailbox.height = buffer.height;
                    mailbox.row_bytes = buffer.row_bytes;
                    ++mailbox.generation;
                }
            }
        }

        // Window titles used both as the ImGui window label (must match
        // exactly what DockBuilderDockWindow below targets) and as the
        // panel's own on-screen tab text.
        constexpr const char *kBrowserWindowTitle = "Browser";
        constexpr const char *kPropertiesWindowTitle = "Properties";
        constexpr const char *kLayersWindowTitle = "Layers";
        constexpr const char *kSettingsWindowTitle = "Settings";
        constexpr const char *kInfoWindowTitle = "Info";
        constexpr const char *kLayoutWindowTitle = "Layout";

        // NEW_FEATURES_SEPT_2026.md item 16 - the dark gray line separating
        // the mode selector (right edge) and the mode/secondary toolbars
        // (bottom edge) from the design view. Call from inside the child,
        // before EndChild: drawn on the child's own draw list, since the
        // parent's would be painted over by the child's opaque background.
        // The child's default clip rect stops short of its own edges (by
        // half its WindowPadding), so the whole window rect is pushed first.
        void draw_child_edge(ImGuiDir side)
        {
            constexpr ImU32 kEdgeColor = IM_COL32(80, 80, 80, 255);
            ImDrawList *draw_list = ImGui::GetWindowDrawList();
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
            draw_list->PushClipRect(min, max, false);
            if (side == ImGuiDir_Right)
                draw_list->AddLine(ImVec2(max.x - 0.5f, min.y), ImVec2(max.x - 0.5f, max.y), kEdgeColor);
            else
                draw_list->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), kEdgeColor);
            draw_list->PopClipRect();
        }

        // Draws the always-present, fullscreen invisible host window +
        // dockspace every frame (cheap - ImGui's own recommended
        // "DockSpace over main viewport" pattern, see imgui_demo.cpp's
        // ShowExampleAppDockSpace), and - the first time only, since
        // there's no persisted layout to restore (io.IniFilename is null,
        // see its own comment above) - programmatically splits it into a
        // left/center/right layout mirroring the Flutter frontend's own
        // default docking layout (home.dart's _buildDefaultLayout:
        // browser/file on the left, layout+console in the center,
        // layers/properties on the right) - minus the console (this
        // prototype's Tcl console is le_shell's own terminal now, not a
        // panel of its own - see this file's own header comment) and
        // collapsed to one placeholder tab per side rather than
        // per-panel tabs, since there's no real content to split between
        // multiple tabs yet. `dockspace_built` is owned by (and reset
        // once per) open_and_run_window's own window-open/close cycle,
        // not a function-static - a fresh ImGui context (and so a fresh,
        // empty dock layout) is created every time show_gui reopens the
        // window, so the split has to be rebuilt every time too.
        // Returns true on exactly the one frame that (re)built the split
        // (dockspace_built's own false->true transition) - see the
        // "Layout" panel's own viewport-size-setting code below (its
        // caller) for why that one frame's own ImGui::GetContentRegionAvail()
        // reading needs to be distrusted rather than acted on: a
        // freshly-split dock node's own child windows don't actually
        // report their final, corrected size until the *following*
        // frame (a well-known one-frame lag in ImGui's own docking
        // system for newly created nodes) - "Layout" still reports the
        // *whole* dockspace's own width on this frame, not yet the
        // ~50% split width, since "Browser"/"Properties" haven't been
        // drawn (and so haven't claimed their own share of it) yet
        // either.
        bool draw_dockspace_and_default_layout(bool &dockspace_built)
        {
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin(
                "DockSpaceHost", nullptr,
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
            ImGui::PopStyleVar(3);

            const ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
            const bool just_built = !dockspace_built;
            if (just_built)
            {
                dockspace_built = true;
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

                ImGuiID center_id = dockspace_id;
                const ImGuiID left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.22f, nullptr, &center_id);
                ImGuiID right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.28f, nullptr, &center_id);
                // NEW_FEATURES_SEPT_2026.md item 15 - the Info panel gets
                // its own strip along the bottom of the right sidebar,
                // below the Properties/Layers/Settings tabs.
                const ImGuiID info_id = ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.15f, nullptr, &right_id);

                ImGui::DockBuilderDockWindow(kBrowserWindowTitle, left_id);
                // Docked into the same node as Properties, not a
                // BeginTabBar/BeginTabItem pair inside one shared window
                // - a real ImGui tab bar can't be dragged apart, but two
                // separate windows docked into the same node still show
                // as tabs of one panel by default while staying fully
                // dockable - the user can drag "Layers" out to its own
                // split/area, matching home.dart's own DockingTabs
                // grouping (a real docking construct there too, not a
                // fixed in-panel tab strip).
                ImGui::DockBuilderDockWindow(kPropertiesWindowTitle, right_id);
                ImGui::DockBuilderDockWindow(kLayersWindowTitle, right_id);
                ImGui::DockBuilderDockWindow(kSettingsWindowTitle, right_id);
                ImGui::DockBuilderDockWindow(kInfoWindowTitle, info_id);
                ImGui::DockBuilderDockWindow(kLayoutWindowTitle, center_id);
                ImGui::DockBuilderFinish(dockspace_id);
            }
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::End();
            return just_built;
        }

        // Creates one window, runs its own frame loop until closed, then
        // tears everything back down - GLFW's own top-level state
        // (glfwInit, called once by run_main_thread_loop) stays alive
        // across repeated open/close cycles, only this window's own
        // GLFWwindow/GL context/ImGui context/texture are per-cycle.
        void open_and_run_window(LeHandle *handle)
        {
            // The single point of contact between this whole module and
            // the API/LeHandle for the rest of this window's own session -
            // see gui_provider.hpp's own doc comment. Constructed once
            // here, refreshed once per frame in the main loop below
            // (right where is_rendering is read), passed to every
            // component as GuiProvider& instead of the raw handle.
            GuiProvider provider(handle);

            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required on macOS
            GLFWwindow *window = glfwCreateWindow(1280, 800, "Layout Engine", nullptr, nullptr);
            if (!window)
            {
                std::fprintf(stderr, "gui: glfwCreateWindow failed\n");
                return;
            }
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            set_dark_pastel_imgui_style();
            ImGuiIO &io = ImGui::GetIO();
            // Deliberately NOT ImGuiConfigFlags_NavEnableKeyboard - this
            // app has its own complete keyboard-shortcut system
            // (forward_keyboard_input/kKeyMappings below, arrow keys
            // included - LE_KEY_PAN_LEFT/_RIGHT/_UP/_DOWN), and nothing
            // here is built to be Tab/arrow-navigated as ImGui widgets.
            // With that flag on, Dear ImGui's own keyboard nav consumes
            // the same arrow keys to move focus between windows/child
            // panels instead (drawing its own blue nav-highlight border
            // around whichever one it just focused) - a real reported
            // bug (arrow-key panning in the Layout view kept stealing
            // focus to the mode_toolbar_row child), not a hypothetical
            // conflict. Text fields (InputInt, etc.) still get normal
            // arrow-key cursor movement regardless of this flag - that's
            // ImGui's ordinary text-editing behavior, unrelated to Nav.
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            // Nothing worth persisting yet - every open_and_run_window()
            // call rebuilds the same default left/center/right split from
            // scratch (setup_default_dock_layout below) rather than
            // restoring a user's own rearranged layout, the ImGui-side
            // equivalent of Flutter's own docking_layout_v1 SharedPreferences
            // persistence (home.dart) - without this, ImGui writes an
            // "imgui.ini" into whatever directory le_shell happens to be
            // run from by default. Worth revisiting once real panel
            // content (not placeholders) makes a stable layout worth
            // keeping across window close/reopen.
            io.IniFilename = nullptr;

            // Icon font (components/mode_selector.cpp, mode_toolbar.cpp,
            // and any later toolbar button) - Dear ImGui draws an icon as
            // plain text via its own Unicode codepoint, so the icon
            // font's own glyphs need to be merged into the same atlas as
            // the regular text font first (ImFontConfig::MergeMode) -
            // AddFontDefault() has to run first to give the merge
            // something to merge *into* (an empty atlas with nothing
            // added yet can't merge). Both calls have to happen before
            // ImGui_ImplOpenGL3_Init below, which builds/uploads the
            // atlas texture from whatever's in it at that point - a font
            // added afterward would never make it into the uploaded
            // texture this session.
            // An explicit SizePixels, not a bare AddFontDefault() - this
            // pinned ImGui commit asserts when merging a font with an
            // explicit reference size (AddFontFromFileTTF always needs
            // one, a scalable TTF has no size of its own) into a
            // destination font that used an *implicit* one
            // (AddFontDefault()'s own default when given no config at
            // all) - 13.0f is ProggyClean.ttf's own established default
            // size in Dear ImGui, unchanged from every prior version.
            ImFontConfig default_font_config;
            default_font_config.SizePixels = 13.0f;
            io.Fonts->AddFontDefault(&default_font_config);
            ImFontConfig icon_font_config;
            icon_font_config.MergeMode = true;
            icon_font_config.PixelSnapH = true;
            icon_font_config.GlyphMinAdvanceX = 16.0f;
            static const ImWchar icon_ranges[] = {ICON_MIN_LC, ICON_MAX_LC, 0};
            const std::string lucide_font_path = resolve_lucide_font_path();
            if (!lucide_font_path.empty())
                io.Fonts->AddFontFromFileTTF(lucide_font_path.c_str(), 16.0f, &icon_font_config, icon_ranges);
            else
                spdlog::error("resolve_lucide_font_path(): FAILED - no usable icon font found, every toolbar icon will render blank. "
                               "See the warn() line(s) immediately above for which candidate paths failed and why.");

            // A second, standalone (not MergeMode) copy of the same
            // Lucide font at 32px - components/icon_font.hpp's own
            // large_icon_font(), used by mode_selector.cpp/
            // mode_toolbar.cpp's icon-only buttons (labels removed,
            // tooltip-only now). Deliberately not merged into the base
            // 13px text font the way the 16px copy above is - these
            // buttons render an icon glyph alone, with no label text on
            // the same line that would need to share its font run.
            if (!lucide_font_path.empty())
            {
                // Plain default ImFontConfig (nullptr) - no
                // GlyphMinAdvanceX, unlike the merged 16px copy above
                // (that pads a narrow glyph's own advance box out to a
                // uniform minimum width, useful when text follows the
                // icon on the same line, but otherwise just adds blank
                // space onto the glyph's own natural bounds that
                // Button/RenderTextClipped then centers along with the
                // ink, visibly shifting it off-center - reported), and
                // no PixelSnapH either (quantizes each glyph's own
                // AdvanceX to a whole pixel at bake time - useful for
                // keeping a run of *several* characters aligned to a
                // pixel grid, but for a single icon-only glyph with
                // nothing else on the same line, it only ever throws
                // away sub-pixel centering accuracy and is a likely
                // source of the reported ~1px residual right-bias).
                large_icon_font() = io.Fonts->AddFontFromFileTTF(lucide_font_path.c_str(), 32.0f, nullptr, icon_ranges);
                // components/icon_font.hpp's small_icon_font() - the
                // secondary toolbar's icons (NEW_FEATURES_SEPT_2026.md item 16).
                small_icon_font() = io.Fonts->AddFontFromFileTTF(lucide_font_path.c_str(), 20.0f, nullptr, icon_ranges);
            }

            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 150");

            GLuint texture_id = 0;
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            int uploaded_width = 0;
            int uploaded_height = 0;

            // The render thread below is deliberately *not* started here
            // (unlike before docking existed, when the whole window was
            // the design view and its size was known immediately) - it
            // isn't spawned until the main loop has computed a real,
            // dock-panel-aware viewport size (see dock_layout_just_built
            // below), not a guess. A guessed bootstrap size used to be
            // needed here (le_render_pixel_buffer() degrades gracefully
            // to an empty buffer for an unset/0x0 viewport, but at least
            // one rasterize stage caches a "nothing to rasterize" result
            // keyed on a constant that never changes for a Layout view,
            // so a 0x0 *first-ever* call would cache that forever - see
            // BUGS_AND_ENHANCEMENTS.md's own history of this bug) only
            // because the render thread used to start immediately, before
            // anything else had a chance to set a real size; deferring
            // its start instead sidesteps that bug more directly (the
            // very first call this thread ever makes now already has a
            // correct size) and also avoids wasting a real, potentially
            // multi-second synchronous cold rasterize (BENCHMARKS.md) on
            // a guessed size about to be immediately superseded once
            // docking's own geometry settles a frame or two later.
            int last_viewport_width = 0;
            int last_viewport_height = 0;
            // BUGS_AND_ENHANCEMENTS.md B5 - dragging a dock splitter
            // (resizing a sidebar) changes this panel's own content
            // region *every single frame* for the whole drag, and acting
            // on each one immediately would mean a full synchronous
            // rasterize per frame (le_set_viewport_size's own cost - a
            // real design can take seconds cold, see BENCHMARKS.md),
            // stalling the drag itself rather than just following it.
            // pending_viewport_width/height/pending_viewport_change_time
            // debounce this: a still-changing size keeps resetting the
            // timer (see the main loop's own viewport-size block below)
            // and is never applied until it's held steady for
            // kResizeDebounceSeconds - i.e. "wait until the resize is
            // finished", not react to every intermediate frame of it.
            int pending_viewport_width = 0;
            int pending_viewport_height = 0;
            double pending_viewport_change_time = 0.0;
            bool dockspace_built = false;
            ActiveGesture gesture = ActiveGesture::kNone;

            RenderMailbox mailbox;
            std::atomic<bool> stop_render_thread{false};
            std::thread render_thread;
            uint64_t displayed_generation = 0;
            bool have_content = false;

            while (!glfwWindowShouldClose(window))
            {
                // glfwWaitEventsTimeout, not glfwPollEvents (see
                // kMainLoopIdleWaitSeconds's own doc comment) - blocks
                // until a real input event wakes it (same responsiveness
                // as PollEvents for actual interaction) instead of
                // returning immediately and letting the loop below
                // free-spin a full redraw with nothing to actually
                // redraw.
                glfwWaitEventsTimeout(kMainLoopIdleWaitSeconds);

                int fb_width = 0;
                int fb_height = 0;
                glfwGetFramebufferSize(window, &fb_width, &fb_height);
                int win_width = 0;
                int win_height = 0;
                glfwGetWindowSize(window, &win_width, &win_height);
                if (win_width < 1)
                    win_width = 1;
                if (win_height < 1)
                    win_height = 1;
                // Framebuffer pixels per logical/window point (a HiDPI/
                // Retina ratio) - uniform across the whole window
                // regardless of how docking splits it into panels, so
                // this stays derived from the *whole* window's own
                // logical/framebuffer size, unlike the design view's own
                // pixel dimensions below (which now depend on however
                // large the user has left the "Layout" dock panel).
                const float scale_x = static_cast<float>(fb_width) / static_cast<float>(win_width);
                const float scale_y = static_cast<float>(fb_height) / static_cast<float>(win_height);

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                // le_is_rendering() is the one handle call safe to make
                // unconditionally every frame - lock-free/atomic
                // (le_handle.hpp's own is_rendering_ doc comment).
                // Checked FIRST, before anything else below touches the
                // handle this frame, and used *raw* - deliberately not
                // debounced/delayed in any way - for every gating
                // decision below: every other le_* function takes
                // handle->mutex_, the very same mutex a slow render
                // holds for its own *entire* duration (that same doc
                // comment), so calling any of them while this reads
                // true blocks this whole thread until the render
                // finishes. Every panel below (draw_library_browser/
                // _property_viewer/_layer_manager/_mode_selector/
                // _mode_toolbar, forward_mouse_input,
                // forward_keyboard_input, draw_status_bar,
                // le_set_viewport_size) makes at least one such call
                // every frame - a real reported bug, not hypothetical:
                // without gating every one of them on is_rendering, the
                // *whole* window froze solid (not just the design view)
                // for however long a slow render took, including window
                // drag/resize.
                //
                // A *debounced* version of this flag (only trust it
                // after reading true continuously for some threshold,
                // to smooth over brief steady-state cache-recheck
                // blips - CLAUDE.md's own HierarchyResolver bullet:
                // run_pending()'s own wait_for_all() runs
                // unconditionally on *every* top-level call, so even a
                // full cache hit isn't free at scale) was tried and
                // reverted - a real, confirmed-by-instrumentation bug,
                // not just a theoretical concern: on the very first
                // frame after a real slow render starts, the debounced
                // value is *still* false (the threshold hasn't elapsed
                // yet), so gating on it let that same frame go ahead
                // and make a locked call anyway - which then blocked
                // for the render's entire remaining duration, since the
                // lock was already held. The loop never got to run
                // again long enough for the debounce to ever resolve,
                // so the "busy" UI (and the spinner) never appeared at
                // all for a genuinely long render - worse than the
                // flicker it was meant to fix. Whether a call is safe
                // to make can only ever be judged from the *current*
                // instant, never a delayed/smoothed view of it -
                // there's no gap in which it's fine to guess.
                //
                // provider.refresh() populates GuiProvider::State fresh
                // for this frame - called here, first thing, so
                // is_rendering below (and every other state() field every
                // component reads this frame) gets exactly the "current
                // instant" freshness guarantee this whole comment block
                // already established: it's a lock-free/atomic read
                // (le_is_rendering) forwarded straight through, not
                // debounced or delayed by refresh() in any way.
                provider.refresh();
                const bool is_rendering = provider.state().is_rendering;

                // show_loading_overlay - whether to draw the spinner/
                // "Loading design..." text. Previously a debounced,
                // hysteresis-smoothed view of is_rendering, needed back
                // when render_thread_loop called le_render_pixel_buffer
                // back-to-back forever on a fixed poll interval - is_rendering
                // could flip true/false many times a second even at
                // idle, so showing it raw would have flickered
                // constantly. Now that render_thread_loop only calls
                // le_render_pixel_buffer once per real
                // le_wait_for_render_needed() wake, and is_rendering_ is
                // itself bracketed precisely around the pipeline's own
                // real recompute (api.cpp's own le_render_pixel_buffer
                // comment), a render is already a clean, one-shot
                // true/false pulse - nothing left to smooth.
                const bool show_loading_overlay = is_rendering;

                // BUGS_AND_ENHANCEMENTS.md B7 - set once the layout
                // view's own hover state is known (forward_mouse_input,
                // below, only runs once the image is actually drawn);
                // forward_keyboard_input is called unconditionally after
                // that, once per frame, using whatever this ends up as.
                bool layout_view_hovered = false;

                const bool dock_layout_just_built = draw_dockspace_and_default_layout(dockspace_built);

                // Left sidebar - components/library_browser.hpp, the
                // ImGui port of frontend/lib/components/library_browser.dart.
                // Called unconditionally, even while is_rendering - every
                // le_* function it calls (le_library_count/_at/
                // _design_count/_at) takes only a std::shared_lock now
                // (le_handle.hpp's own mutex_ doc comment), so it runs
                // concurrently with an in-progress render instead of
                // blocking behind it. This panel (like Properties/Layers/
                // the mode selector+toolbar/status bar below) used to be
                // skipped outright while rendering, a real, reported
                // regression in its own right - the user wanted these
                // panels to never change at all during a render, not show
                // a placeholder or go blank, which a client-side skip
                // could never actually deliver alongside "and never
                // block either" at the same time. Fixed at the actual
                // source of the conflict instead: handle->mutex_ itself.
                ImGui::Begin(kBrowserWindowTitle);
                draw_library_browser(provider);
                ImGui::End();

                // Right sidebar - two separate dockable panels docked
                // into the same node by default (see
                // draw_dockspace_and_default_layout's own comment on
                // why not a single BeginTabBar/BeginTabItem pair),
                // mirroring home.dart's own DockingTabs([layers,
                // properties]) grouping: property_viewer.hpp
                // (frontend/lib/components/property_viewer.dart) and
                // layer_manager.hpp (frontend/lib/components/layer_manager.dart).
                // Called unconditionally - kBrowserWindowTitle's own
                // comment above. GuiProvider::object_children
                // (gui_provider.cpp) has one narrow, documented
                // exception (a Design's own children specifically)
                // still gated on state().is_rendering internally, for
                // the one case with no shared-lock-safe accessor to
                // switch to - see its own comment.
                ImGui::Begin(kPropertiesWindowTitle);
                draw_property_viewer(provider);
                ImGui::End();

                ImGui::Begin(kLayersWindowTitle);
                draw_layer_manager(provider);
                ImGui::End();

                // NEW_FEATURES_SEPT_2026.md item 9 - a third tab in the
                // same right-hand dock node.
                ImGui::Begin(kSettingsWindowTitle);
                draw_settings_panel(provider);
                ImGui::End();

                // NEW_FEATURES_SEPT_2026.md item 15 - the current mode's
                // instructions, below the tabs above (it replaced the
                // status bar's middle column).
                ImGui::Begin(kInfoWindowTitle);
                draw_info_panel(provider);
                ImGui::End();

                // Zero window padding - the design view/status bar sizing
                // below budgets against its own content region's *full*
                // width/height, not that region minus whatever the
                // default ~8px WindowPadding would otherwise eat into on
                // every edge; without this, the image + status bar
                // together overflow their own true bottom/right edges by
                // exactly that padding amount, clipping the status bar
                // row's own text there. ModeSelector/ModeToolbar below
                // push their own, real padding back in just for
                // themselves (BeginChild captures whatever WindowPadding
                // is active *at the moment it's called*, not
                // retroactively - safe to toggle around each one).
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                // The Layout/Abstract design view always renders on pure
                // black, regardless of the current ImGui theme (unlike
                // every other dock panel, which follows
                // set_dark_pastel_imgui_style()'s own WindowBg/ChildBg) -
                // an explicit user request, not derived from the
                // rendered design content itself (which already draws
                // its own background via the pipelines module,
                // independent of this). Pushed once here, popped once
                // after this panel's own End() below - both
                // ModeSelector's/ModeToolbar's BeginChild calls and the
                // "no design loaded yet" fallback area all read the same
                // pushed color, since ImGui resolves style colors
                // dynamically against whatever's on top of this stack at
                // the moment each is drawn, not a value captured once at
                // push time.
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::Begin(kLayoutWindowTitle, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                const ImVec2 full_panel_avail = ImGui::GetContentRegionAvail();
                const float full_panel_height = full_panel_avail.y > 1.0f ? full_panel_avail.y : 1.0f;

                // ModeSelector (mode_selector.hpp) - a fixed-width column
                // to the left of everything else, matching home.dart's
                // own Row(ModeSelector, Column(ModeToolbar, LayoutEngine,
                // StatusBar)) layout: it's a plain child of this same
                // "Layout" panel, not a separate dock panel of its own,
                // so it moves/resizes with the design view rather than
                // being independently dockable like Browser/Properties/
                // Layers.
                // 64 = mode_selector.cpp's own 48px icon-only button plus
                // this child's 8px WindowPadding on each side.
                constexpr float kModeSelectorWidth = 64.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
                // ImGuiChildFlags_AlwaysUseWindowPadding - a borderless
                // BeginChild ignores the pushed WindowPadding entirely by
                // default (Dear ImGui's own documented behavior: "no
                // padding by default for non-bordered child windows"),
                // so without this flag the pushed (8,8) above was a
                // no-op - content region was the full 64px column width,
                // and the 48px button (flush against the child's own
                // top-left origin) left all 16px of slack on the right
                // instead of split 8/8 either side. A real reported bug
                // (asymmetric padding), not cosmetic preference.
                ImGui::BeginChild("mode_selector_column", ImVec2(kModeSelectorWidth, full_panel_height), ImGuiChildFlags_AlwaysUseWindowPadding);
                // Called unconditionally - draw_mode_selector's own
                // le_get_mode call is std::shared_lock now (kBrowserWindowTitle's
                // own comment further up).
                draw_mode_selector(provider);
                draw_child_edge(ImGuiDir_Right);
                ImGui::EndChild();
                ImGui::PopStyleVar();

                ImGui::SameLine();

                // Everything else - ModeToolbar, the design view, and
                // the status bar - shares the remaining width, stacked
                // in one child so panel_width/panel_height below are
                // this child's own content region, not the whole
                // "Layout" panel's (which still includes ModeSelector's
                // own column).
                ImGui::BeginChild("layout_content_column", ImVec2(0.0f, full_panel_height));

                // ModeToolbar (mode_toolbar.hpp) - a fixed-height row
                // above the design view, same "plain child, not its own
                // dock panel" reasoning as ModeSelector above.
                // 64 = mode_toolbar.cpp's own 48px icon-only buttons plus
                // this child's 8px WindowPadding on each side.
                constexpr float kModeToolbarHeight = 64.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
                // See mode_selector_column's own comment above -
                // ImGuiChildFlags_AlwaysUseWindowPadding is needed here
                // for the same reason (the pushed (8,8) is otherwise a
                // no-op for this borderless child too, leaving its
                // buttons flush against the top edge instead of centered
                // top/bottom).
                ImGui::BeginChild("mode_toolbar_row", ImVec2(0.0f, kModeToolbarHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
                // Called unconditionally - draw_mode_toolbar's own
                // le_get_mode/le_is_move_armed calls are std::shared_lock
                // now (kBrowserWindowTitle's own comment further up).
                draw_mode_toolbar(provider);
                draw_child_edge(ImGuiDir_Down);
                ImGui::EndChild();
                ImGui::PopStyleVar();

                // This panel's own content area, in logical/window
                // points - however large docking has left it (the user
                // can resize/rearrange the left/right sidebars freely)
                // and now also ModeSelector/ModeToolbar's own fixed
                // sizes above - not the whole GLFW window - converted to
                // framebuffer pixels via scale_x/scale_y for
                // le_set_viewport_size, which (like every other le_*
                // pixel-space call) works in real framebuffer pixels,
                // not logical points.
                const ImVec2 panel_avail = ImGui::GetContentRegionAvail();
                const float panel_width = panel_avail.x > 1.0f ? panel_avail.x : 1.0f;
                const float panel_height = panel_avail.y > 1.0f ? panel_avail.y : 1.0f;
                const float image_win_height =
                    (panel_height - kStatusBarHeight) > 1.0f ? panel_height - kStatusBarHeight : 1.0f;
                const int viewport_width =
                    static_cast<int>(panel_width * scale_x + 0.5f) > 0
                        ? static_cast<int>(panel_width * scale_x + 0.5f)
                        : 1;
                const int viewport_height =
                    static_cast<int>(image_win_height * scale_y + 0.5f) > 0
                        ? static_cast<int>(image_win_height * scale_y + 0.5f)
                        : 1;

                // dock_layout_just_built: skip acting on this frame's own
                // size (draw_dockspace_and_default_layout's own doc
                // comment on why panel_avail can't be trusted yet on
                // this one frame) - real cost, not just cosmetic: this
                // handle's render pipeline does a full synchronous
                // rasterize of whatever's currently loaded on every
                // viewport-size change (a real design can take seconds
                // cold, see BENCHMARKS.md), so acting on a known-wrong
                // width here would burn a real render on a size that's
                // about to be thrown away one frame later anyway.
                if (!dock_layout_just_built &&
                    (viewport_width != last_viewport_width || viewport_height != last_viewport_height))
                {
                    // BUGS_AND_ENHANCEMENTS.md B5 - debounced (see
                    // pending_viewport_width's own declaration comment
                    // above), except for the very first-ever apply (the
                    // render thread hasn't started yet - this is initial
                    // sizing right after the dock layout settled, not a
                    // user drag, and the render thread's own startup is
                    // itself gated on a real size being applied at least
                    // once - see its own declaration comment).
                    const bool is_first_ever_apply = !render_thread.joinable();
                    if (viewport_width != pending_viewport_width || viewport_height != pending_viewport_height)
                    {
                        pending_viewport_width = viewport_width;
                        pending_viewport_height = viewport_height;
                        pending_viewport_change_time = glfwGetTime();
                    }
                    constexpr double kResizeDebounceSeconds = 0.15;
                    // !is_rendering - le_set_viewport_size is
                    // handle->mutex_-locked (see this frame's own
                    // is_rendering doc comment further up); guaranteed
                    // false here on the very first-ever apply (the
                    // render thread hasn't started yet, and nothing
                    // else on this handle calls le_render_pixel_buffer),
                    // so this only ever actually defers a *later*
                    // resize that happens to land while an existing
                    // render is still in flight - viewport_width/height
                    // stay != last_viewport_width/height, so this whole
                    // block is simply retried next frame until the
                    // render finishes.
                    if (!is_rendering &&
                        (is_first_ever_apply || (glfwGetTime() - pending_viewport_change_time) >= kResizeDebounceSeconds))
                    {
                        provider.set_viewport_size(viewport_width, viewport_height);
                        last_viewport_width = viewport_width;
                        last_viewport_height = viewport_height;
                        if (is_first_ever_apply)
                        {
                            render_thread = std::thread(render_thread_loop, handle, std::ref(mailbox), std::ref(stop_render_thread));
                        }
                    }
                }

                // Only upload a new GL texture when the background render
                // thread has actually published something newer than what
                // this thread last showed - the common case at idle (mouse
                // not moving, nothing changed) is "nothing new", so this
                // just keeps redrawing the already-uploaded texture rather
                // than re-uploading identical bytes every frame. Holding
                // mailbox.mutex for the GL upload itself (not just the
                // copy) is deliberate and cheap - a few MB at most, and it
                // keeps this simple (no extra local copy) since the render
                // thread only re-takes the lock briefly, once per its own
                // iteration, not for the whole render.
                {
                    std::lock_guard<std::mutex> lock(mailbox.mutex);
                    if (mailbox.generation != displayed_generation && !mailbox.pixels.empty())
                    {
                        glBindTexture(GL_TEXTURE_2D, texture_id);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(mailbox.row_bytes / 4));
                        if (mailbox.width != uploaded_width || mailbox.height != uploaded_height)
                        {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mailbox.width, mailbox.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, mailbox.pixels.data());
                            uploaded_width = mailbox.width;
                            uploaded_height = mailbox.height;
                        }
                        else
                        {
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mailbox.width, mailbox.height, GL_RGBA, GL_UNSIGNED_BYTE, mailbox.pixels.data());
                        }
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        displayed_generation = mailbox.generation;
                        have_content = true;
                    }
                }

                // is_rendering (E17's own spinner signal, le_is_rendering)
                // covers the very first, potentially multi-second cold
                // render just as much as any later one - computed once,
                // up at the top of this frame (this function's own
                // is_rendering doc comment there), so both branches
                // below (and the cursor override further down) agree on
                // it instead of only the have_content branch ever
                // noticing it, which was the original reported bug: a
                // large DEF's own first render gave zero feedback (no
                // "rendering..." text - that only ever fired once
                // have_content was already true, which is exactly what
                // the first cold render hasn't finished yet) and the
                // static "No design loaded yet" text stayed up the
                // whole time, actively suggesting nothing was happening.
                bool over_layout_content = false;
                // Captured before drawing the Image/Dummy below, not a
                // fixed (8,8) window-relative offset - this panel can
                // now sit anywhere within the GLFW window (docking, not
                // always the top-left corner). Used both for the corner
                // "rendering..." text (image_screen_pos-relative) and,
                // further down, to compute the spinner's own fixed
                // center position - see that block's own comment for why
                // it no longer follows the mouse.
                const ImVec2 content_screen_pos = ImGui::GetCursorScreenPos();

                if (have_content)
                {
                    const ImVec2 &image_screen_pos = content_screen_pos;
                    ImGui::Image(
                        static_cast<ImTextureID>(static_cast<intptr_t>(texture_id)),
                        ImVec2(panel_width, image_win_height));
                    // forward_mouse_input is handle->mutex_-locked
                    // (le_set_mouse_position/le_mouse_down/up/...) - see
                    // this frame's own is_rendering doc comment further
                    // up. IsItemHovered() alone needs no handle call, so
                    // hover (and therefore the spinner/hidden-cursor
                    // block further down) still works correctly while a
                    // render is in flight, just without forwarding to
                    // the backend that frame.
                    if (is_rendering)
                    {
                        over_layout_content = ImGui::IsItemHovered();
                    }
                    else
                    {
                        layout_view_hovered = forward_mouse_input(provider, gesture, scale_x, scale_y);
                        over_layout_content = layout_view_hovered;

                        // NEW_FEATURES_SEPT_2026.md item 3 - with Resize
                        // armed, a resize cursor over a selected shape's
                        // grabbable edge/segment, pointing the way it moves
                        // (state is refreshed at the top of each frame, so
                        // this trails the mouse by one frame - unnoticeable).
                        if (layout_view_hovered)
                        {
                            switch (provider.state().resize.hover_axis)
                            {
                            case LE_RESIZE_AXIS_X:
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                                break;
                            case LE_RESIZE_AXIS_Y:
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                                break;
                            case LE_RESIZE_AXIS_BOTH:
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                                break;
                            default:
                                break;
                            }
                        }
                    }

                    // A render actually in progress means whatever's
                    // currently displayed may already be stale and a
                    // fresher frame is on its way - a lightweight corner
                    // overlay rather than blocking anything, since the
                    // image above is already the latest *completed*
                    // frame and stays interactive/pannable while a new
                    // one renders. show_loading_overlay (debounced), not
                    // raw is_rendering - purely cosmetic (AddText, no
                    // handle call), so it's safe to smooth over brief
                    // is_rendering blips here even though the same
                    // smoothing is unsafe for the mouse-forwarding
                    // choice just above (see show_loading_overlay's own
                    // declaration comment).
                    if (show_loading_overlay)
                    {
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(image_screen_pos.x + 8, image_screen_pos.y + 8),
                            IM_COL32(255, 255, 255, 220), "rendering...");
                    }
                }
                else
                {
                    // Dummy fills the same reserved area the Image above
                    // would otherwise occupy, so draw_status_bar below
                    // always lands at the same fixed spot at the bottom
                    // of this panel regardless of whether a design is
                    // loaded yet.
                    const float dummy_height = image_win_height > ImGui::GetTextLineHeight()
                        ? image_win_height - ImGui::GetTextLineHeight()
                        : 0.0f;
                    ImGui::Dummy(ImVec2(panel_width, dummy_height));
                    over_layout_content = ImGui::IsItemHovered();
                    if (show_loading_overlay)
                        ImGui::TextUnformatted("Loading design - this can take a while for a large one...");
                    else
                        ImGui::TextUnformatted("No design loaded yet - read_lef/open_design from the console.");
                }

                // Secondary toolbar (secondary_toolbar.hpp) - overlaid on
                // the design view's own top edge, directly under
                // ModeToolbar, rather than a row of its own: see
                // has_secondary_toolbar's own comment for why (showing it
                // must never resize the viewport). Drawn after the Image
                // above, so it's hit-tested on top of it - forward_mouse_input's
                // own IsItemHovered() is false while the mouse is over
                // this child window. The cursor is restored afterwards so
                // draw_status_bar below lands where it always does.
                if (have_content && has_secondary_toolbar(provider))
                {
                    const ImVec2 resume_pos = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(content_screen_pos);
                    // Opaque black, square corners - matches mode_toolbar_row.
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
                    // AutoResizeY: one 48px line (36px buttons + 6px padding
                    // each side), taller when a snap toolbar wraps a group
                    // onto another line at a narrow width.
                    ImGui::BeginChild("secondary_toolbar_row", ImVec2(panel_width, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    draw_secondary_toolbar(provider);
                    draw_child_edge(ImGuiDir_Down);
                    ImGui::EndChild();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                    ImGui::SetCursorScreenPos(resume_pos);
                }

                // Drawn at a fixed position - the design view's own
                // center - not at the mouse cursor the way this used to
                // work (ImGui::GetMousePos(), gated on over_layout_content/
                // hover). A real reported bug, not a style choice: the
                // whole point of this spinner is to be the *one* reliable
                // signal a render is in progress (Browser/Properties/
                // Layers/status bar deliberately show nothing themselves
                // now - see kBrowserWindowTitle's own comment further
                // up), but tying it to mouse hover meant it silently
                // never appeared whenever the mouse wasn't already over
                // the design view - e.g. selecting an object then
                // pressing a keyboard shortcut like fit while the mouse
                // is still sitting over the Properties panel reading
                // that object's properties. No mouse-cursor hiding
                // either, for the same reason - there's no longer a
                // single "the cursor's position" this icon is replacing.
                if (show_loading_overlay)
                {
                    const ImVec2 center(content_screen_pos.x + panel_width * 0.5f, content_screen_pos.y + image_win_height * 0.5f);
                    draw_loading_spinner(ImGui::GetForegroundDrawList(), center, static_cast<float>(glfwGetTime()));
                }

                // !is_rendering - forward_keyboard_input calls
                // le_key_down/_up/le_clear_all_keys, all
                // handle->mutex_-locked (see this frame's own
                // is_rendering doc comment further up); layout_view_hovered
                // already stays false while is_rendering (the
                // have_content block above only sets it in the
                // !is_rendering branch), so skipping the call outright
                // here (rather than just passing `active=false` through)
                // avoids it still calling le_clear_all_keys, which is
                // itself just as locked. Whatever was held down when
                // rendering started stays "held" from the backend's own
                // point of view until this resumes running once the
                // render finishes - BUGS_AND_ENHANCEMENTS.md B7 - see
                // forward_keyboard_input's own doc comment for why
                // io.WantTextInput, not io.WantCaptureKeyboard, is the
                // right flag here.
                if (!is_rendering)
                    forward_keyboard_input(provider, layout_view_hovered && !ImGui::GetIO().WantTextInput);

                // Called unconditionally - draw_status_bar's own
                // le_get_mode/le_tooltip_message/le_snapped_mouse_position/
                // le_selection_count calls are all std::shared_lock now
                // (kBrowserWindowTitle's own comment further up).
                draw_status_bar(provider, panel_width);

                ImGui::EndChild(); // layout_content_column

                ImGui::End();
                ImGui::PopStyleColor(2); // ChildBg, WindowBg
                ImGui::PopStyleVar();

                ImGui::Render();
                glViewport(0, 0, fb_width, fb_height);
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(window);
            }

            stop_render_thread.store(true, std::memory_order_relaxed);
            // Wakes the render thread out of its own le_wait_for_render_needed()
            // block (render_thread_loop's own doc comment) so it notices
            // stop_render_thread above and exits - without this it would
            // simply stay parked there forever if no further mutation ever
            // arrives, since a plain store to a std::atomic<bool> it isn't
            // waiting on can't itself wake it. A safe no-op if the render
            // thread was never even started (a window closed within its
            // very first couple of frames - render_thread's own declaration
            // comment) or happens to already be awake doing a render.
            le_cancel_render_wait(handle);

            // Tear the window/GL/ImGui resources down *before* waiting
            // for the render thread to actually exit, not after - it
            // never touches any of them (only le_render_pixel_buffer()
            // on `handle` and its own mailbox, under mailbox.mutex), so
            // there's no ordering hazard in destroying them first.
            // Joining first was a real, reproduced bug: the render
            // thread's own last in-flight le_render_pixel_buffer() call
            // can take several real seconds for a large design
            // (BENCHMARKS.md) - blocking here *before* the window was
            // destroyed left a live window on screen that stopped
            // responding to window-server events for that whole
            // duration, which macOS reports as "Application Not
            // Responding" (the spinning beachball cursor). Destroying
            // the window first makes it disappear immediately regardless
            // of how long the trailing render still has left to run.
            glDeleteTextures(1, &texture_id);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);

            // Still joined (not detached) before this function returns -
            // `mailbox` is this function's own stack storage, and the
            // thread writes into it right up until it observes `stop`,
            // so it has to be reaped before that storage goes away. May
            // never have been spawned at all - a window closed within
            // its very first couple of frames, before docking's own
            // layout ever settled enough to compute a real viewport size
            // (see this thread's own declaration comment above).
            if (render_thread.joinable())
            {
                render_thread.join();
            }
        }
    }

    // Never returns, deliberately, in every case - le_shell.cpp's own
    // main() relies on that (see its own comment on tcl_thread, detached
    // not joined: the whole process exits from inside that thread's own
    // std::exit() call, and main() falling through to `return 0` while
    // it's still mid-flight is a real, reproduced race/segfault, not a
    // theoretical one). glfwInit() failing (e.g. no DISPLAY - a real
    // case on a headless CI/Docker container with no Xvfb, confirmed by
    // hitting this in Dockerfile.linux-ci's own `ctest` run) used to
    // return here instead, breaking that invariant for exactly this one
    // case; idling forever below keeps it true unconditionally, so
    // show_gui simply never opens a window on such a machine (the
    // originally-intended degraded behavior) rather than the process
    // racing its own teardown.
    void run_main_thread_loop(LeHandle *handle)
    {
        if (!glfwInit())
        {
            std::fprintf(stderr, "gui: glfwInit failed - show_gui will never be able to open a window\n");
            for (;;)
            {
                std::this_thread::sleep_for(kIdlePollInterval);
            }
        }

        for (;;)
        {
            while (!le_take_show_gui_request(handle))
            {
                std::this_thread::sleep_for(kIdlePollInterval);
            }
            open_and_run_window(handle);
        }
    }
}
