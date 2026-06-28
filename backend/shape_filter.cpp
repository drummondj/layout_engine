#include <iostream>
#include "shape_filter.h"

namespace le
{
    ShapeRefs ShapeFilter::filter(const ShapeRefs shapes, const database::RectT &viewport, const std::weak_ptr<Coordinates> coordinates)
    {
        auto pixel_width = coordinates.lock()->pixels_2_dbu(1);

        ShapeRefs filtered;
        for (const auto &shape_ref : shapes)
        {
            const auto &shape = shape_ref.get();
            const auto bbox = Geometry::bbox(shape);

            // Filter out small shapes
            if (bbox->ur->x - bbox->ll->x < pixel_width)
                continue;

            if (bbox->ur->y - bbox->ll->y < pixel_width)
                continue;

            // Filter out shapes not in the viewport
            if (!Geometry::rects_overlap(viewport, *bbox.get()))
                continue;

            filtered.push_back(shape_ref);
        }
        return filtered;
    }
}