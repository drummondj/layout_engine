#include "le_gui.hpp"

#include "api.hpp"
#include "components/status_bar.hpp"
#include "components/library_browser.hpp"
#include "components/property_viewer.hpp"
#include "components/layer_manager.hpp"
#include "components/mode_selector.hpp"
#include "components/mode_toolbar.hpp"

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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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
        void forward_keyboard_input(LeHandle *handle, bool active)
        {
            static bool was_active = false;
            static bool ctrl_was_held = false;
            static bool shift_was_held = false;

            if (!active)
            {
                if (was_active)
                {
                    le_clear_all_keys(handle);
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
                (io.KeyCtrl ? le_key_down : le_key_up)(handle, LE_KEY_CTRL);
                ctrl_was_held = io.KeyCtrl;
            }
            if (io.KeyShift != shift_was_held)
            {
                (io.KeyShift ? le_key_down : le_key_up)(handle, LE_KEY_SHIFT);
                shift_was_held = io.KeyShift;
            }

            for (const KeyMapping &mapping : kKeyMappings)
            {
                if (ImGui::IsKeyPressed(mapping.imgui_key))
                    le_key_down(handle, mapping.le_key_code);
                if (ImGui::IsKeyReleased(mapping.imgui_key))
                    le_key_up(handle, mapping.le_key_code);
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
        bool forward_mouse_input(LeHandle *handle, ActiveGesture &gesture, float scale_x, float scale_y)
        {
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 origin = ImGui::GetItemRectMin();
            const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
            const int32_t px = static_cast<int32_t>((mouse_pos.x - origin.x) * scale_x);
            const int32_t py = static_cast<int32_t>((mouse_pos.y - origin.y) * scale_y);

            static bool was_hovered = false;
            if (hovered)
            {
                le_set_mouse_position(handle, px, py);
            }
            else if (was_hovered)
            {
                le_clear_mouse_position(handle);
            }
            was_hovered = hovered;

            if (gesture == ActiveGesture::kNone && hovered)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    le_mouse_down(handle, px, py);
                    gesture = ActiveGesture::kSelect;
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    le_zoom_drag_down(handle, px, py);
                    gesture = ActiveGesture::kZoomDrag;
                }
            }
            else if (gesture == ActiveGesture::kSelect && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                le_mouse_up(handle, px, py);
                gesture = ActiveGesture::kNone;
            }
            else if (gesture == ActiveGesture::kZoomDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                le_mouse_up(handle, px, py);
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
                le_zoom(handle, wheel * kZoomStepPerWheelTick, px, py);
            }

            return hovered;
        }

        // How often the background render thread re-checks the handle
        // once it has nothing new to do - le_render_pixel_buffer() is
        // itself "close to free" when nothing changed (its own doc
        // comment), so this thread could legally spin with no sleep at
        // all and still be cheap, but a short sleep avoids needlessly
        // pinning a whole CPU core at 100% while idle for no benefit.
        constexpr auto kRenderThreadIdleInterval = std::chrono::milliseconds(33);

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

        void render_thread_loop(LeHandle *handle, RenderMailbox &mailbox, std::atomic<bool> &stop)
        {
            while (!stop.load(std::memory_order_relaxed))
            {
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
                std::this_thread::sleep_for(kRenderThreadIdleInterval);
            }
        }

        // Window titles used both as the ImGui window label (must match
        // exactly what DockBuilderDockWindow below targets) and as the
        // panel's own on-screen tab text.
        constexpr const char *kBrowserWindowTitle = "Browser";
        constexpr const char *kPropertiesWindowTitle = "Properties";
        constexpr const char *kLayersWindowTitle = "Layers";
        constexpr const char *kLayoutWindowTitle = "Layout";

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
                const ImGuiID right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.28f, nullptr, &center_id);

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
            ImGuiIO &io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
            io.Fonts->AddFontFromFileTTF(LE_LUCIDE_FONT_PATH, 16.0f, &icon_font_config, icon_ranges);

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
                glfwPollEvents();

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

                // BUGS_AND_ENHANCEMENTS.md B7 - set once the layout
                // view's own hover state is known (forward_mouse_input,
                // below, only runs once the image is actually drawn);
                // forward_keyboard_input is called unconditionally after
                // that, once per frame, using whatever this ends up as.
                bool layout_view_hovered = false;

                const bool dock_layout_just_built = draw_dockspace_and_default_layout(dockspace_built);

                // Left sidebar - components/library_browser.hpp, the
                // ImGui port of frontend/lib/components/library_browser.dart.
                ImGui::Begin(kBrowserWindowTitle);
                draw_library_browser(handle);
                ImGui::End();

                // Right sidebar - two separate dockable panels docked
                // into the same node by default (see
                // draw_dockspace_and_default_layout's own comment on
                // why not a single BeginTabBar/BeginTabItem pair),
                // mirroring home.dart's own DockingTabs([layers,
                // properties]) grouping: property_viewer.hpp
                // (frontend/lib/components/property_viewer.dart) and
                // layer_manager.hpp (frontend/lib/components/layer_manager.dart).
                ImGui::Begin(kPropertiesWindowTitle);
                draw_property_viewer(handle);
                ImGui::End();

                ImGui::Begin(kLayersWindowTitle);
                draw_layer_manager(handle);
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
                constexpr float kModeSelectorWidth = 72.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
                ImGui::BeginChild("mode_selector_column", ImVec2(kModeSelectorWidth, full_panel_height));
                draw_mode_selector(handle);
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
                constexpr float kModeToolbarHeight = 44.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
                ImGui::BeginChild("mode_toolbar_row", ImVec2(0.0f, kModeToolbarHeight));
                draw_mode_toolbar(handle);
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
                    if (is_first_ever_apply || (glfwGetTime() - pending_viewport_change_time) >= kResizeDebounceSeconds)
                    {
                        le_set_viewport_size(handle, viewport_width, viewport_height);
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

                if (have_content)
                {
                    // Captured before drawing the Image, not a fixed
                    // (8,8) window-relative offset - this panel can now
                    // sit anywhere within the GLFW window (docking, not
                    // always the top-left corner), and AddText below
                    // draws in absolute screen coordinates.
                    const ImVec2 image_screen_pos = ImGui::GetCursorScreenPos();
                    ImGui::Image(
                        static_cast<ImTextureID>(static_cast<intptr_t>(texture_id)),
                        ImVec2(panel_width, image_win_height));
                    layout_view_hovered = forward_mouse_input(handle, gesture, scale_x, scale_y);

                    // A render actually in progress (le_is_rendering, E17's
                    // own spinner signal) means whatever's currently
                    // displayed may already be stale and a fresher frame
                    // is on its way - a lightweight corner overlay rather
                    // than blocking anything, since the image above is
                    // already the latest *completed* frame and stays
                    // interactive/pannable while a new one renders.
                    if (le_is_rendering(handle))
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
                    ImGui::TextUnformatted("No design loaded yet - read_lef/open_design from the console.");
                }

                // BUGS_AND_ENHANCEMENTS.md B7 - see forward_keyboard_input's
                // own doc comment for why io.WantTextInput, not
                // io.WantCaptureKeyboard, is the right flag here.
                forward_keyboard_input(handle, layout_view_hovered && !ImGui::GetIO().WantTextInput);

                draw_status_bar(handle, panel_width);

                ImGui::EndChild(); // layout_content_column

                ImGui::End();
                ImGui::PopStyleVar();

                ImGui::Render();
                glViewport(0, 0, fb_width, fb_height);
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(window);
            }

            stop_render_thread.store(true, std::memory_order_relaxed);

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
