#pragma once
// The design view's interaction vocabulary - modes, keys, snapping and
// edit operations - shared by the C++ API (views.hpp) and the code behind
// it. No dependencies, so any front end can name them.

#include <cstdint>

namespace le
{
    /// @brief What a click or drag in the design view does. Only Select
    /// changes the selection; Edit edits it (Move, Resize, Delete); Ruler
    /// measures.
    enum class InteractionMode : int32_t
    {
        SELECT = 0,
        EDIT = 1,
        RULER = 2,
    };

    /// @brief A key the design view reacts to. Ctrl and Shift are
    /// modifiers, tracked while held; the others act on key_down, each for
    /// its own modifier combinations only.
    enum class Key : int32_t
    {
        SHIFT = 1,
        /// @brief Zoom in at the mouse; Shift zooms out; Ctrl undoes, Ctrl-Shift redoes.
        ZOOM = 2,
        /// @brief Fit the open view; Ctrl fits the selection.
        FIT = 3,
        PAN_LEFT = 4,
        PAN_RIGHT = 5,
        PAN_UP = 6,
        PAN_DOWN = 7,
        CTRL = 8,
        /// @brief DIGIT_1..DIGIT_9 toggle the 1st..9th routing layer, or
        /// the 11th..19th with Ctrl; DIGIT_0 the 10th.
        DIGIT_1 = 10,
        DIGIT_2 = 11,
        DIGIT_3 = 12,
        DIGIT_4 = 13,
        DIGIT_5 = 14,
        DIGIT_6 = 15,
        DIGIT_7 = 16,
        DIGIT_8 = 17,
        DIGIT_9 = 18,
        /// @brief Ctrl: clear the selection (Select mode only).
        DESELECT_ALL = 19,
        DIGIT_0 = 20,
        SELECT_MODE = 21,
        EDIT_MODE = 22,
        /// @brief Ruler mode; Ctrl arms Resize.
        RULER_MODE = 23,
        /// @brief Escape: cancels a drag, else finishes the ruler being
        /// drawn, cancels Move and cancels or disarms Resize. Works with
        /// any modifiers held.
        FINISH_RULER = 24,
        /// @brief Ctrl: arm Move.
        MOVE = 25,
        /// @brief Delete the selected pieces (Edit mode only).
        DELETE = 26,
    };

    /// @brief What a moving Placement's location (the lower-left of its
    /// placed bbox, as in DEF) snaps to.
    enum class PlacementSnapMode : int32_t
    {
        NONE = 0,
        SITE = 1,
        FIN_GRID = 2,
        MANUFACTURING_GRID = 3,
    };

    /// @brief One orientation edit applied to a Placement, in world space:
    /// a 90-degree counterclockwise rotation (N -> W, as in DEF), a
    /// horizontal flip (mirror in X, N -> FN) or a vertical flip (mirror in
    /// Y, N -> FS).
    enum class OrientationOp : int32_t
    {
        ROTATE_CCW = 0,
        FLIP_HORIZONTAL = 1,
        FLIP_VERTICAL = 2,
    };

    /// @brief What a resized edge or segment (or a moved path or via)
    /// snaps to. Rects and polygons offer NONE, USER_GRID,
    /// MANUFACTURING_GRID and FIN_GRID; paths, vias and via arrays NONE,
    /// USER_GRID, MANUFACTURING_GRID (a path's edges land on it) and TRACKS
    /// (its centerline lands on a routing track of its layer).
    enum class ShapeSnapMode : int32_t
    {
        NONE = 0,
        USER_GRID = 1,
        MANUFACTURING_GRID = 2,
        FIN_GRID = 3,
        TRACKS = 4,
    };

    /// @brief Which way the edge or segment under the mouse moves when
    /// Resize grabs it.
    enum class ResizeAxis
    {
        /// @brief A vertical edge: moves left and right.
        X,
        /// @brief A horizontal edge: moves up and down.
        Y,
        /// @brief A diagonal polygon edge or path segment.
        BOTH,
    };

    /// @brief Why apply_placement_orientation_op did nothing.
    enum class OrientationOpError
    {
        NOTHING_SELECTED,
        /// @brief The selection allows no such op right now: a Move is
        /// under way, no Layout is open, or SITE snapping forbids it.
        NOT_AVAILABLE,
        /// @brief A selected Placement is FIXED or COVER.
        LOCKED,
    };
}
