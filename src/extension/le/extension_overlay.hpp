#pragma once
// The extension SDK's drawing hook: an overlay draws on the design view with
// Blend2D, after Layout Engine's own overlays (selection, rulers, cursor),
// every time the view is drawn. Register one with Registry::add_overlay.

#include "le/extension.hpp"

#include <blend2d/blend2d.h>

namespace le
{
    struct OverlayFrame;
}

namespace le::ext
{
    /// @brief What an overlay draws with, valid for one call. It runs on the
    /// render thread with the session locked for reading: read root() and
    /// data<T>(), but don't call le_* functions or ExtensionContext::read()/
    /// write(). Keep it quick - it runs on every mouse move.
    class OverlayContext
    {
    public:
        OverlayContext(BLContext &canvas, const OverlayFrame &frame, LeHandle *handle, std::string_view extension_name);

        /// @brief The view's pixels. Its state (transform, clip, styles) is
        /// restored after the overlay returns.
        BLContext &canvas() { return canvas_; }
        /// @brief Where a design point (dbu) is on the canvas.
        BLPoint to_pixel(Point dbu) const;
        /// @brief Pixels per dbu.
        double scale() const;
        /// @brief The design area the canvas shows (dbu).
        Rect visible_area() const;
        int width() const;
        int height() const;

        /// @brief The database (read-only; the session is locked).
        const Root &root() const;
        /// @brief This extension's per-session state (ExtensionContext::data).
        template <class T>
        T &data()
        {
            return extension_.data<T>();
        }

    private:
        BLContext &canvas_;
        const OverlayFrame &frame_;
        LeHandle *handle_;
        ExtensionContext extension_;
    };
}
