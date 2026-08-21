#pragma once
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Transforms Pipeline's filtered, ViewLayerId-grouped dbu-space
    /// output into pixel space (Scene's `pixel = (dbu - pan) * scale`).
    /// Root of Renderer's own stage DAG - the `shapes` it transforms comes
    /// from `Pipeline`, outside this module (`render` doesn't link
    /// `pipeline` - see src/core/'s own doc comment), so there's no
    /// upstream Renderer stage to compose this key from.
    ///
    /// Key: `{AbstractId, Scene::viewport_version(), Scene::visibility_version(),
    /// Root::mutation_version()}` - matches Pipeline::filter_by_layer_visibility's
    /// own output-invalidation triggers exactly (pan/scale/viewport-size all
    /// bump viewport_version()).
    class TransformToPixelsStage
    {
    public:
        const std::map<ViewLayerId, std::vector<PixelShape>> &run(const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                const Point pan = scene.pan();
                const double scale = scene.scale();
                auto to_pixel = [&](Point p)
                {
                    return PixelPoint{
                        .x = (static_cast<double>(p.x) - static_cast<double>(pan.x)) * scale,
                        .y = (static_cast<double>(p.y) - static_cast<double>(pan.y)) * scale,
                    };
                };

                std::map<ViewLayerId, std::vector<PixelShape>> result;

                for (const auto &[view_layer, group] : shapes)
                {
                    std::vector<PixelShape> pixel_group;
                    pixel_group.reserve(group.size());

                    for (const auto &rs : group)
                    {
                        PixelShape ps;
                        ps.origin = rs.origin;

                        ps.rects.reserve(rs.shape.rects.size());
                        for (const auto &r : rs.shape.rects)
                            ps.rects.push_back(PixelRect{.ll = to_pixel(r.ll), .ur = to_pixel(r.ur)});

                        ps.polygons.reserve(rs.shape.polygons.size());
                        for (const auto &poly : rs.shape.polygons)
                        {
                            PixelPolygon pp;
                            pp.points.reserve(poly.points.size());
                            for (const auto &pt : poly.points)
                                pp.points.push_back(to_pixel(pt));
                            ps.polygons.push_back(std::move(pp));
                        }

                        ps.paths.reserve(rs.shape.paths.size());
                        for (size_t i = 0; i < rs.shape.paths.size(); ++i)
                        {
                            const Path &path = rs.shape.paths[i];
                            PixelPath pp;
                            pp.width = static_cast<double>(path.width) * scale;
                            pp.polygon.points.reserve(path.polygon.points.size());
                            for (const auto &pt : path.polygon.points)
                                pp.polygon.points.push_back(to_pixel(pt));

                            pp.buffered_outline.reserve((*rs.path_outlines)[i].size());
                            for (const auto &outline_poly : (*rs.path_outlines)[i])
                            {
                                PixelPolygon outline_pp;
                                outline_pp.points.reserve(outline_poly.points.size());
                                for (const auto &pt : outline_poly.points)
                                    outline_pp.points.push_back(to_pixel(pt));
                                pp.buffered_outline.push_back(std::move(outline_pp));
                            }

                            ps.paths.push_back(std::move(pp));
                        }

                        ps.texts.reserve(rs.shape.texts.size());
                        for (const auto &t : rs.shape.texts)
                        {
                            // t.size (dbu) scales like any other width
                            // (mirrors PixelPath.width above), but a label
                            // shouldn't literally touch/overflow the edges
                            // of the geometry it's on (kLabelWidthRatio),
                            // and must stay legible even on hair-thin
                            // geometry at any zoom level (kMinLabelPixelSize).
                            const double pixel_size = std::max(t.size * scale * kLabelWidthRatio, kMinLabelPixelSize);
                            ps.texts.push_back(PixelText{.label = t.label, .location = to_pixel(t.location), .size = pixel_size});
                        }

                        pixel_group.push_back(std::move(ps));
                    }

                    result.emplace(view_layer, std::move(pixel_group));
                }

                return result;
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<PixelShape>>> stage_;
    };
}
