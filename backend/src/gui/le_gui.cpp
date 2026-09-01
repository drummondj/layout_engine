#include "le_gui.hpp"

#include "api.hpp"

// Apple deprecated the whole OpenGL framework in favor of Metal (10.14+)
// but still fully implements it - every desktop-GL ImGui backend still
// targets it the same way, this is a purely informational warning.
#define GL_SILENCE_DEPRECATION

#include "imgui.h"
#include "imgui_impl_glfw.h"
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
        void forward_keyboard_input(LeHandle *handle)
        {
            ImGuiIO &io = ImGui::GetIO();
            static bool ctrl_was_held = false;
            static bool shift_was_held = false;
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
        // logical points.
        void forward_mouse_input(LeHandle *handle, ActiveGesture &gesture, float scale_x, float scale_y)
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
        }

        // How often the background render thread re-checks the handle
        // once it has nothing new to do - le_render_pixel_buffer() is
        // itself "close to free" when nothing changed (its own doc
        // comment), so this thread could legally spin with no sleep at
        // all and still be cheap, but a short sleep avoids needlessly
        // pinning a whole CPU core at 100% while idle for no benefit.
        constexpr auto kRenderThreadIdleInterval = std::chrono::milliseconds(33);

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
            // Nothing worth persisting yet - one fixed, non-movable/
            // non-resizable window (see the ImGuiWindowFlags below) -
            // without this, ImGui writes an "imgui.ini" into whatever
            // directory le_shell happens to be run from by default.
            io.IniFilename = nullptr;
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 150");

            GLuint texture_id = 0;
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            int uploaded_width = 0;
            int uploaded_height = 0;

            // Set synchronously, before the render thread below ever makes
            // its first call - le_render_pixel_buffer() degrades
            // gracefully (an empty buffer) when called with an unset/0x0
            // viewport, but at least one of this handle's rasterize
            // stages caches a "nothing to rasterize" result keyed on a
            // constant that never changes for a Layout view (there's no
            // real tiny-shapes content there yet, so nothing to version) -
            // meaning if its own *first-ever* call happens to catch the
            // viewport still unset, that empty result is what gets cached
            // forever, never revisited even once the viewport becomes
            // valid on every later call. No prior caller of
            // le_render_pixel_buffer ever raced this (a single Tcl
            // dump_png call, or Flutter's own occasional texture pull,
            // both only ever call it after a real viewport size was
            // already set) - this dedicated render thread is the first to
            // start calling it immediately, before this GUI's own first
            // frame would otherwise set one.
            int initial_fb_width = 0;
            int initial_fb_height = 0;
            glfwGetFramebufferSize(window, &initial_fb_width, &initial_fb_height);
            le_set_viewport_size(handle, initial_fb_width, initial_fb_height);

            int last_fb_width = initial_fb_width;
            int last_fb_height = initial_fb_height;
            ActiveGesture gesture = ActiveGesture::kNone;

            RenderMailbox mailbox;
            std::atomic<bool> stop_render_thread{false};
            std::thread render_thread(render_thread_loop, handle, std::ref(mailbox), std::ref(stop_render_thread));
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
                const float scale_x = static_cast<float>(fb_width) / static_cast<float>(win_width);
                const float scale_y = static_cast<float>(fb_height) / static_cast<float>(win_height);

                if (fb_width != last_fb_width || fb_height != last_fb_height)
                {
                    le_set_viewport_size(handle, fb_width, fb_height);
                    last_fb_width = fb_width;
                    last_fb_height = fb_height;
                }

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                forward_keyboard_input(handle);

                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_width), static_cast<float>(win_height)));
                ImGui::Begin(
                    "Layout Engine",
                    nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
                    ImGui::Image(
                        static_cast<ImTextureID>(static_cast<intptr_t>(texture_id)),
                        ImVec2(static_cast<float>(win_width), static_cast<float>(win_height)));
                    forward_mouse_input(handle, gesture, scale_x, scale_y);

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
                            ImVec2(8, 8), IM_COL32(255, 255, 255, 220), "rendering...");
                    }
                }
                else
                {
                    ImGui::TextUnformatted("No design loaded yet - read_lef/open_design from the console.");
                }

                ImGui::End();

                ImGui::Render();
                glViewport(0, 0, fb_width, fb_height);
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(window);
            }

            stop_render_thread.store(true, std::memory_order_relaxed);
            render_thread.join();

            glDeleteTextures(1, &texture_id);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
        }
    }

    void run_main_thread_loop(LeHandle *handle)
    {
        if (!glfwInit())
        {
            std::fprintf(stderr, "gui: glfwInit failed - show_gui will never be able to open a window\n");
            return;
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
