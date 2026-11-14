#pragma once
#include "area_ops.hpp"
#include "transforms.hpp"

namespace le
{
    /// @brief LEF ITERATE (DO ... BY ... STEP) expansion into the plain pieces it stands for.
    class IterateExpansion : public AreaOps, public GeometryTransforms
    {
    public:
        /// @brief The most elements one ITERATE or via array may expand to
        /// (num_x * num_y). A real one repeats a handful of times.
        static constexpr int64_t kMaxIterateCount = 1'000'000;

        /// @brief True if a num_x by num_y ITERATE or via array is positive
        /// and expands to at most kMaxIterateCount elements.
        static constexpr bool iterate_count_ok(int num_x, int num_y)
        {
            return num_x > 0 && num_y > 0 && static_cast<int64_t>(num_x) * num_y <= kMaxIterateCount;
        }

        /// @brief Expands RECT/PATH/POLYGON ITERATE (raw LEF storage) into
        /// concrete rects/paths/polygons on a copy of `shape`. An iterate
        /// failing iterate_count_ok is skipped.
        static ShapeData expand_iterates(ShapeData shape)
        {
            expand_iterate_list(shape.rect_iterates, shape.rects, [](const RectIterate &it, Point offset)
                                { return Rect{.ll = Point{.x = it.rect.ll.x + offset.x, .y = it.rect.ll.y + offset.y},
                                              .ur = Point{.x = it.rect.ur.x + offset.x, .y = it.rect.ur.y + offset.y}}; });
            expand_iterate_list(shape.path_iterates, shape.paths, [](const PathIterate &it, Point offset)
                                { return Path{.width = it.path.width, .polygon = transform(it.path.polygon, offset)}; });
            expand_iterate_list(shape.polygon_iterates, shape.polygons, [](const PolygonIterate &it, Point offset)
                                { return transform(it.polygon, offset); });
            return shape;
        }

        /// @brief Appends `make(it, offset)` to `out` for every element of
        /// every iterate in `iterates` (x outer, y inner), skipping one that
        /// fails iterate_count_ok, then clears `iterates`.
        template <typename Iterates, typename Out, typename Make>
        static void expand_iterate_list(Iterates &iterates, Out &out, Make make)
        {
            for (const auto &it : iterates)
            {
                if (!iterate_count_ok(it.num_x, it.num_y))
                    continue;
                out.reserve(out.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                        out.push_back(make(it, Point{.x = ix * it.space_x, .y = iy * it.space_y}));
            }
            iterates.clear();
        }
    };
}
