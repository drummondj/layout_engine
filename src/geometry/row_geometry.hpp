#pragma once
#include "../database/database.hpp"
#include <optional>
#include <vector>

namespace le
{
    /// @brief A Row's own synthesized footprint bbox (site size tiled
    /// num_x/num_y times at step_x/step_y, falling back to the site's own
    /// size as the step when unset - the common case where sites sit
    /// edge-to-edge). Row has no stored Shape of its own (purely
    /// parametric geometry), so rendering (HierarchyResolverStage) and
    /// selection (api.cpp) both derive its bbox here. nullopt if the Row's
    /// site_name doesn't resolve, its Site has no stored size, or the Row
    /// itself has no stored origin.
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

    /// @brief Every Row of `layout_id` whose footprint contains `p` (edges
    /// inclusive), in Layout order.
    inline std::vector<RowId> hit_test_rows_point_all(const Root &root, LayoutId layout_id, Point p)
    {
        std::vector<RowId> hits;
        for (const RowId row_id : root.get_layout_rows(layout_id))
            if (const std::optional<Rect> bbox = row_footprint_bbox(root, row_id))
                if (p.x >= bbox->ll.x && p.x <= bbox->ur.x && p.y >= bbox->ll.y && p.y <= bbox->ur.y)
                    hits.push_back(row_id);
        return hits;
    }

    /// @brief Every Row of `layout_id` whose footprint lies entirely
    /// inside `rect`, in Layout order.
    inline std::vector<RowId> hit_test_rows_rect(const Root &root, LayoutId layout_id, const Rect &rect)
    {
        std::vector<RowId> hits;
        for (const RowId row_id : root.get_layout_rows(layout_id))
            if (const std::optional<Rect> bbox = row_footprint_bbox(root, row_id))
                if (bbox->ll.x >= rect.ll.x && bbox->ll.y >= rect.ll.y && bbox->ur.x <= rect.ur.x && bbox->ur.y <= rect.ur.y)
                    hits.push_back(row_id);
        return hits;
    }
}
