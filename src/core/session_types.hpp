#pragma once

// Plain types for a session's state beyond the database - its selection,
// rulers and view - and the file readers' results and writers' options. Shared by LeHandle, the
// le::edit ops and the extension SDK, so they carry no dependencies beyond
// the database's ids.

#include "../database/database.hpp"
#include "../database/piece_kind.hpp"
#include "../database/wire_helpers.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace le
{
    /// @brief One selected piece: the rect, polygon, path, via or via array
    /// at `piece_index` of `piece_kind` in a Shape, or in a Wire (exactly one
    /// of `shape_id`/`wire_id` is valid; a Wire's pieces are those of the
    /// Shape it converts to, wire_to_shape). Properties are per Shape, so
    /// only outlines and Move care which piece it is.
    struct ShapePiece
    {
        ShapeId shape_id;
        WireId wire_id;
        PieceKind piece_kind = PieceKind::RECT;
        size_t piece_index = 0;

        /// @brief The Shape or Wire this is a piece of.
        GeometryId owner() const { return wire_id.valid() ? GeometryId::of(wire_id) : GeometryId::of(shape_id); }

        friend auto operator<=>(const ShapePiece &, const ShapePiece &) = default;
    };

    /// @brief One selection entry: a piece of anything with Shape or Wire
    /// geometry (Terminal, Obstruction, Blockage, Route, PhysicalPort), or a
    /// whole Row, Placement or Region, which have none of their own.
    using SelectedObject = std::variant<ShapePiece, RowId, PlacementId, RegionId>;

    /// @brief What selecting a whole object takes: a Shape, Wire, Route or
    /// PhysicalPort selects every piece of its geometry; a Row, Placement or
    /// Region selects itself.
    using SelectableObject = std::variant<ShapeId, WireId, RouteId, PhysicalPortId, RowId, PlacementId, RegionId>;

    /// @brief One ruler: a polyline of dbu points. At most one ruler is
    /// unfinished - the last - and Ruler-mode clicks append to it.
    struct Ruler
    {
        std::vector<Point> points;
        bool finished = false;
    };

    /// @brief How the view is mirrored on screen (WriteView::set_view_flip): about
    /// the frame's vertical centre line (HORIZONTAL, left and right swap)
    /// or its horizontal one (VERTICAL, top and bottom swap). The same dbu
    /// area shows either way.
    enum class ViewFlip : uint8_t
    {
        NONE,
        HORIZONTAL,
        VERTICAL,
    };

    /// @brief The design view: what it shows and where.
    struct ViewState
    {
        /// @brief The open Abstract, or invalid; at most one of `abstract`
        /// and `layout` is valid.
        AbstractId abstract;
        /// @brief The open Layout, or invalid.
        LayoutId layout;
        /// @brief The dbu area the viewport shows, before any flip.
        Rect visible_area;
        /// @brief Pixels per dbu.
        double scale = 1.0;
        int width_px = 0;
        int height_px = 0;
        ViewFlip flip = ViewFlip::NONE;
    };

    /// @brief What a LEF write includes besides MACROs (LEFWriter::LayerWriteMode).
    enum class LefLayerWriteMode
    {
        /// @brief MACROs only.
        NONE,
        /// @brief The Technology's layers, then the MACROs.
        INCLUDE_WITH_ABSTRACT,
        /// @brief The Technology's layers only; no MACRO.
        TECHNOLOGY_ONLY,
    };

    /// @brief What reading a DEF's placements into an existing Layout did.
    struct DefPlacementsReport
    {
        /// @brief Placements whose status, location or orientation changed.
        size_t updated = 0;
        /// @brief Components naming no Placement in the Layout; left out.
        std::vector<std::string> unmatched;
        /// @brief Placements (updated anyway) whose cell isn't the component's.
        std::vector<std::string> cell_mismatches;
    };
}
