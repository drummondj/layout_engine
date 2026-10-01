#pragma once

namespace le
{
    /// @brief Which of a Shape's three geometry vectors (rects/polygons/
    /// paths) a piece index refers to - a Shape can bundle several
    /// pieces together (e.g. several RECT statements under one LEF PORT/
    /// OBS LAYER line), so addressing exactly one piece needs both a kind
    /// and an index into that kind's own vector. See
    /// Geometry::find_hit_piece/fully_enclosed_pieces/extract_piece/
    /// transform_piece_in_place (geometry.hpp) and LeHandle::SelectedObject/
    /// HoverTarget (api/le_handle.hpp).
    enum class PieceKind
    {
        RECT,
        POLYGON,
        PATH,
        VIA,         // a plain via instance (Shape.vias - a named via at an origin).
                     // NEW_FEATURES_SEPT_2026.md item 6.
        VIA_ITERATE, // a whole via array (Shape.via_iterates - DO n BY m STEP x y),
                     // selected and moved as one unit. NEW_FEATURES_SEPT_2026.md item 12.
    };
}
