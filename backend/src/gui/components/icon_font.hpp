#pragma once

#include "imgui.h"

namespace le::gui
{
    // Shared accessor for the large (32px), icon-only Lucide font used by
    // mode_selector.cpp/mode_toolbar.cpp's now-label-less icon buttons -
    // set once by le_gui.cpp's own font setup, right after the same
    // lucide.ttf file used for the merged 16px icon font (le_gui.cpp's
    // own font-merge block) loads successfully at a second, larger,
    // non-merged size. Left null if that load failed (or hasn't run
    // yet, e.g. in a unit test) - ImGui::PushFont(nullptr) is documented
    // as a safe no-op (falls back to whatever font is already active),
    // not a crash, so every call site here can use this unconditionally.
    // A function-local static reference, not a plain global, to keep
    // this header dependency-free and header-only, matching
    // tcl_command_queue.hpp's own convention - no .cpp file needed just
    // for one pointer's storage.
    inline ImFont *&large_icon_font()
    {
        static ImFont *font = nullptr;
        return font;
    }
}
