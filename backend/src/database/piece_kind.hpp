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
    };
}
