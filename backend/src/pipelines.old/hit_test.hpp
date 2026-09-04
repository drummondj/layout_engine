#pragma once
#include "../core/rendered_shape.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <map>
#include <optional>
#include <vector>

namespace le
{
    /// @brief oneTBB migration port of Pipeline::hit_test_point/hit_test_rect
    /// (backend/ONETBB_INTEGRATION.md migration plan, Phase 5) - unchanged
    /// verbatim from the original, as free functions rather than static
    /// methods: neither is a cache/stage at all (dbu_point/dbu_rect change
    /// on every call, so there's no reusable output to memoize), so they
    /// don't belong to any particular pipeline/stage class - just plain
    /// utilities operating on an already-filtered, ViewLayerId-grouped
    /// shape map (typically AbstractShapePipeline::run's or
    /// LayoutShapePipeline::run's own output).

    /// @brief Topmost-layer-first point hit-test (UPDATES.md 7.1 items
    /// 1-3) - only ever scans what's actually on screen (an already
    /// viewport-culled and visibility-filtered map), not the whole
    /// design. Reverse-iterates `shapes` (its std::map key order is
    /// bottom-up stacking order - see FilterByLayerVisibilityStage's own
    /// comment - so reverse means topmost first), skipping a ViewLayer the
    /// Scene has made unselectable and any RenderedShape with no `origin`
    /// (the BOUNDARY shape - not selectable). Returns the first hit's
    /// origin plus a copy of just the single rect/polygon/path piece that
    /// was actually hit (Geometry::find_hit_piece) - not the whole
    /// RenderedShape's Shape, which can bundle several rects/polygons/
    /// paths together. First-match-within-a-layer wins for two
    /// overlapping shapes on the same layer, an accepted MVP limitation.
    /// nullopt if nothing was hit. Works identically against either an
    /// Abstract's or a Layout's own output.
    inline std::optional<HoverTarget> hit_test_point(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Point dbu_point)
    {
        for (auto it = shapes.rbegin(); it != shapes.rend(); ++it)
        {
            const ViewLayerData *data = view_layers.get(it->first);
            if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                continue;

            for (const auto &rs : it->second)
            {
                if (!rs.origin)
                    continue;

                if (auto piece = Geometry::find_hit_piece(rs.shape, dbu_point))
                    return HoverTarget{.origin = *rs.origin, .outline = piece->outline, .shape_id = rs.shape_id};
            }
        }

        return std::nullopt;
    }

    /// @brief Rubber-band enclosure hit-test (UPDATES.md 7.1 item 5: "all
    /// selectable shapes on all layers completely enclosed by the
    /// selection rectangle") - unlike hit_test_point, scans every layer
    /// (no topmost-only restriction) and collects every match, in no
    /// particular order. Same unselectable-layer/no-origin skip as
    /// hit_test_point. Piece-level, like hit_test_point (via
    /// Geometry::fully_enclosed_pieces rather than the coarser
    /// fully_enclosed) - one RenderedShape can bundle several rects/
    /// polygons/paths together, and a drag enclosing only some of them
    /// must report only those. Returns one HoverTarget per enclosed
    /// piece.
    inline std::vector<HoverTarget> hit_test_rect(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Rect dbu_rect)
    {
        std::vector<HoverTarget> result;

        for (const auto &[view_layer_id, group] : shapes)
        {
            const ViewLayerData *data = view_layers.get(view_layer_id);
            if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                continue;

            for (const auto &rs : group)
            {
                if (!rs.origin)
                    continue;

                for (auto &piece : Geometry::fully_enclosed_pieces(dbu_rect, rs.shape))
                    result.push_back(HoverTarget{.origin = *rs.origin, .outline = std::move(piece.outline), .shape_id = rs.shape_id});
            }
        }

        return result;
    }
}
