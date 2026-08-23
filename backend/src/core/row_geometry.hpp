#pragma once
#include "../database/database.hpp"
#include <optional>

namespace le
{
    /// @brief A Row's own synthesized footprint bbox (site size tiled
    /// num_x/num_y times at step_x/step_y, falling back to the site's own
    /// size as the step when unset - the common case where sites sit
    /// edge-to-edge) - Row has no stored Shape of its own (purely
    /// parametric geometry, see Migration Step 2's own plan), so this is
    /// the same computation GenerateLayoutShapesStage's own
    /// append_row_shapes uses to synthesize its RenderedShape, factored
    /// out here (E1, BUGS_AND_ENHANCEMENTS.md) so render's own
    /// BuildSelectionOverlayPictureStage can resolve a selected RowId's
    /// bbox too - `render` doesn't link `pipeline` (see CMakeLists.txt),
    /// so this couldn't stay private to GenerateLayoutShapesStage the way
    /// it used to; `core` sits below both. nullopt if the Row's own
    /// site_name doesn't resolve, its Site has no stored size, or the Row
    /// itself has no stored origin - the same skip conditions
    /// append_row_shapes already applies.
    inline std::optional<Rect> row_footprint_bbox(const Root &root, RowId row_id)
    {
        const RowData *row = root.get_row(row_id);
        if (!row || !row->origin)
            return std::nullopt;
        const SiteId site_id = root.get_site_by_name(row->site_name);
        const SiteData *site = site_id.valid() ? root.get_site(site_id) : nullptr;
        if (!site || !site->size)
            return std::nullopt;

        const int num_x = row->num_x.value_or(1);
        const int num_y = row->num_y.value_or(1);
        const int64_t step_x = row->step_x.value_or(site->size->x);
        const int64_t step_y = row->step_y.value_or(site->size->y);
        const int64_t width = site->size->x + static_cast<int64_t>(num_x > 0 ? num_x - 1 : 0) * step_x;
        const int64_t height = site->size->y + static_cast<int64_t>(num_y > 0 ? num_y - 1 : 0) * step_y;

        return Rect{.ll = *row->origin, .ur = Point{.x = row->origin->x + width, .y = row->origin->y + height}};
    }
}
