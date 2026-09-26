#pragma once

#include "compact_button.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace le::gui
{
    // A number setting as a "commit on Enter" field
    // (ImGuiInputTextFlags_EnterReturnsTrue) with a label - the Settings
    // panel's rows (settings_panel.cpp; moved here from layer_manager.cpp
    // with Hierarchy Depth and the flightline fanout limit, NEW_FEATURES_SEPT_2026.md
    // item 9). Submitting a valid value shows *only* that value - ignoring
    // `backend_value`'s own still-stale value - until the backend actually
    // catches up to it: every setting is applied through a queued Tcl
    // command, and pressing Enter defocuses the field immediately, so
    // without this the very next frame's re-sync would snap the field back
    // to the old value for the ~100ms the command takes to land, then
    // forward again once it does (a real, reported flicker). Still
    // re-synced whenever nothing is pending and the field isn't focused,
    // so an external change (e.g. from the Tcl console, or a loaded
    // settings file) shows up here too.
    template <typename T>
    struct CommittedField
    {
        T buf{};
        bool was_active = false;
        bool has_pending = false;
        T pending_value{};
        T backend_at_submit{}; // the backend applies its own rounding (um -> whole dbu) - any change ends the wait too
    };

    namespace committed_field_detail
    {
        inline bool matches(int32_t a, int32_t b) { return a == b; }
        inline bool matches(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); }

        // Has the backend caught up with a submitted value - reached it, or
        // moved at all since the submit?
        template <typename T>
        bool caught_up(const CommittedField<T> &field, T backend_value)
        {
            return matches(backend_value, field.pending_value) || !matches(backend_value, field.backend_at_submit);
        }
    }

    // A non-negative integer with "-"/"+" step buttons (a "-" at 0
    // settles at 0; a typed negative value resets to the backend's).
    template <typename Apply>
    void draw_committed_int_field(const char *id, const char *label, int32_t backend_value, CommittedField<int32_t> &field, Apply apply)
    {
        if (field.has_pending && committed_field_detail::caught_up(field, backend_value))
            field.has_pending = false;
        if (!field.was_active && !field.has_pending)
            field.buf = backend_value;

        const auto submit = [&](int32_t new_value)
        {
            new_value = std::max<int32_t>(new_value, 0);
            field.buf = new_value;
            apply(new_value);
            field.has_pending = true;
            field.pending_value = new_value;
            field.backend_at_submit = backend_value;
        };

        // 3 characters wide - these fields only ever hold a small integer.
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
        int value = field.buf;
        if (ImGui::InputInt(id, &value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (value >= 0)
                submit(value);
            else
            {
                field.buf = backend_value; // the backend rejects a negative value
                field.has_pending = false;
            }
        }
        else
            field.buf = value;
        field.was_active = ImGui::IsItemActive();

        constexpr float kStepButtonWidth = 24.0f;
        // compact_button (compact_button.hpp), not a plain ImGui::Button -
        // see its own doc comment for the centering bug a button this
        // narrow would otherwise hit.
        ImGui::PushID(id);
        ImGui::SameLine();
        if (compact_button("-", kStepButtonWidth))
            submit(field.buf - 1);
        ImGui::SameLine();
        if (compact_button("+", kStepButtonWidth))
            submit(field.buf + 1);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    }

    // A positive number, shown with `format`; a typed value <= 0 resets
    // to the backend's. `backend_value` < 0 means "not known yet" (e.g.
    // grid spacing in um before any Technology is read) - shown as a
    // disabled placeholder with `unknown_hint` as its tooltip.
    template <typename Apply>
    void draw_committed_double_field(const char *id, const char *label, const char *format, double backend_value, CommittedField<double> &field,
                                     Apply apply, const char *unknown_hint = "")
    {
        const float width = ImGui::CalcTextSize("00000.000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (backend_value < 0.0 && !field.has_pending)
        {
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(width);
            char placeholder[2] = "";
            ImGui::InputText(id, placeholder, sizeof(placeholder));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", unknown_hint);
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            return;
        }

        if (field.has_pending && committed_field_detail::caught_up(field, backend_value))
            field.has_pending = false;
        if (!field.was_active && !field.has_pending)
            field.buf = backend_value;

        ImGui::SetNextItemWidth(width);
        if (ImGui::InputDouble(id, &field.buf, 0.0, 0.0, format, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (field.buf > 0.0)
            {
                apply(field.buf);
                field.has_pending = true;
                field.pending_value = field.buf;
                field.backend_at_submit = backend_value;
            }
            else
            {
                field.buf = backend_value;
                field.has_pending = false;
            }
        }
        field.was_active = ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    }
}
