#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief Collects every Shape from an Abstract's Terminals' Ports, its
    /// Obstructions, and its boundary polygon, each resolved to its
    /// ViewLayerId, in dbu-space. The first stage in both of Pipeline's
    /// chains (see Pipeline's own class comment for the two chains) - has
    /// no upstream stage of its own, so its key is the one place in this
    /// file that lists real trigger sources directly rather than composing
    /// via another stage's version().
    ///
    /// Key: `{AbstractId, ViewLayerSet::generation(), Root::mutation_version()}`.
    /// `ViewLayerSet::generation()` is included (not just AbstractId)
    /// because this stage resolves every shape straight to a ViewLayerId
    /// using the *given* ViewLayerSet - api.cpp's le_read_lef rebuilds its
    /// handle's ViewLayerSet from scratch (a new Pool, not an in-place
    /// update) on every call, so re-reading a LEF file while viewing an
    /// already-cached Abstract must invalidate this stage even though the
    /// AbstractId itself hasn't changed, or it would keep returning
    /// RenderedShapes whose ViewLayerIds were resolved against a
    /// now-discarded ViewLayerSet. `Root::mutation_version()` is included
    /// because a Terminal/TerminalPort/Obstruction/Shape CRUD mutation
    /// (UPDATES.md item 15's TCL/API surface - api.cpp's le_create_terminal,
    /// le_update_shape, etc., each bumping this counter on success)
    /// changes neither AbstractId nor ViewLayerSet (no LEF was re-read) -
    /// without it, this stage would keep returning the pre-mutation shape
    /// list forever, no matter how many times the caller re-rendered.
    /// Found exactly this way (a Tcl-created Shape never appeared on
    /// screen, no amount of requesting a new frame fixed it, since the
    /// frame *was* being regenerated - from a stale cache) rather than
    /// anticipated up front.
    class GenerateShapesStage
    {
    public:
        /// @brief Collect every Shape from the Abstract's Terminals' Ports,
        /// its Obstructions, and its boundary polygon, each resolved to its
        /// ViewLayerId, in dbu-space. BOUNDARY shapes skip the lookup
        /// entirely; they're always view_layers.boundary_view_layer(). A
        /// shape whose layer_name doesn't resolve to a known Layer (e.g. an
        /// undeclared/typo'd name) keeps an invalid ViewLayerId rather than
        /// being dropped - there's no visibility toggle to check it
        /// against yet. An unknown AbstractId (nothing created yet, or a
        /// stale/erased one) yields an empty result rather than an error -
        /// Root's own lookups already degrade gracefully for that.
        ///
        /// Each Terminal gets one text label *per distinct layer_name* it
        /// has geometry on - its name, placed via Geometry::get_label_location
        /// and sized via Geometry::local_width_at on the union of that
        /// layer's geometry across all the Terminal's Ports (not per-Port;
        /// several Ports can share a layer, and the label represents all
        /// of a Terminal's geometry on that layer). Both calls share the
        /// same combined geometry and anchor point, so the label is sized
        /// to whichever piece it actually landed on, not the layer's
        /// overall bbox. A
        /// single-layer Terminal (the common case) gets exactly one label;
        /// a Terminal with shapes on e.g. both M1 and M2 gets two, each
        /// correctly tied to its own layer's ViewLayerId so it only shows
        /// alongside that layer, not the Terminal as a whole. Each label is
        /// appended to the .texts of its layer's *first* Port Shape rather
        /// than emitted as its own text-only Shape: FilterByViewportAndSizeStage
        /// drops shapes with no bbox, and Geometry::bbox doesn't account for
        /// Shape::texts, so a standalone label would be silently culled.
        /// Riding along on a real geometric shape means it inherits that
        /// shape's bbox (and ViewLayerId) for free. Known gap: if that
        /// specific first shape happens to have no geometry of its own
        /// (unusual but possible), its label is dropped with it - not
        /// handled specially.
        ///
        /// Also computes each Shape's RenderedShape::path_outlines here
        /// (one Geometry::path_to_polygons buffer op per path) - this is
        /// the right cache tier for that real geometry cost (~768ns/call,
        /// see BM_PathToPolygonsSingleCall) since this stage only
        /// recomputes on an Abstract switch, a ViewLayerSet rebuild, or a
        /// database mutation (Root::mutation_version() - see this class's
        /// own doc comment), not on every pan/zoom the way
        /// Renderer::transform_to_pixels does.
        const std::vector<RenderedShape> &run(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers)
        {
            return stage_.get(std::tuple{abstract_id, view_layers.generation(), root.mutation_version()}, [&]
            {
                std::vector<RenderedShape> shapes;
                const auto &terminals = root.get_abstract_terminals(abstract_id);
                const auto &obstructions = root.get_abstract_obstructions(abstract_id);
                shapes.reserve(terminals.size() + obstructions.size());

                auto resolve = [&](const Shape &shape, ViewLayerPurpose purpose)
                {
                    return view_layers.find(root.get_layer_by_name(shape.layer_name), purpose);
                };

                // UPDATES.md 12 Phase 1's ITERATE rework - LEFReader stores
                // RECT/PATH/POLYGON ITERATE statements raw (rect_iterates/
                // path_iterates/polygon_iterates) rather than pre-expanding
                // them at parse time, so LEFWriter can re-emit compact
                // ITERATE syntax on write. This is the one place downstream
                // Pipeline/Renderer code needs to know about that - expands
                // each into concrete Rect/Path/Polygon entries appended to
                // the same Shape's rects/paths/polygons, so everything below
                // (label accumulation, compute_path_outlines, RenderedShape
                // itself) sees a conventional fully-expanded Shape exactly
                // as before this rework. Bounds each statement's own num_x*num_y the same
                // way LEFReader::safe_iteration_count did at parse time -
                // defense in depth, not just trusting the database's
                // already-validated values.
                auto expand_iterates = [](Shape shape)
                {
                    constexpr int kMaxReasonableCount = 1'000'000;

                    for (const RectIterate &it : shape.rect_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.rects.reserve(shape.rects.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                                shape.rects.push_back(Rect{
                                    .ll = Point{.x = it.rect.ll.x + ix * it.space_x, .y = it.rect.ll.y + iy * it.space_y},
                                    .ur = Point{.x = it.rect.ur.x + ix * it.space_x, .y = it.rect.ur.y + iy * it.space_y},
                                });
                    }
                    shape.rect_iterates.clear();

                    for (const PathIterate &it : shape.path_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.paths.reserve(shape.paths.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                            {
                                const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                                shape.paths.push_back(Path{.polygon = Geometry::transform(it.path.polygon, offset), .width = it.path.width});
                            }
                    }
                    shape.path_iterates.clear();

                    for (const PolygonIterate &it : shape.polygon_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.polygons.reserve(shape.polygons.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                            {
                                const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                                shape.polygons.push_back(Geometry::transform(it.polygon, offset));
                            }
                    }
                    shape.polygon_iterates.clear();

                    return shape;
                };

                // One buffered outline per path, computed once here (see
                // RenderedShape::path_outlines's own comment for why this
                // is the right cache tier, and why it's shared_ptr-wrapped).
                auto compute_path_outlines = [](const Shape &shape)
                {
                    std::vector<std::vector<Polygon>> outlines;
                    outlines.reserve(shape.paths.size());
                    for (const Path &path : shape.paths)
                        outlines.push_back(Geometry::path_to_polygons(path));
                    return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
                };

                for (auto terminal_id : terminals)
                {
                    // Single pass over this Terminal's Ports/Shapes: push
                    // each one into `shapes` immediately (preserving its own
                    // layer_name/ViewLayerId - combining shapes across
                    // layers would break per-layer visibility for
                    // multi-layer Terminals, and coarsen viewport/sub-pixel
                    // culling to a combined bbox instead of each piece's
                    // own), while also accumulating just the geometry
                    // primitives (not whole Shapes) into a per-layer_name
                    // combined Shape for label placement only. Each layer's
                    // label is attached after the loop, once its location
                    // is known, to the first Shape pushed for *that layer*
                    // (remembered by index - safe across any reallocation
                    // `shapes.push_back` triggers, since vector indices
                    // stay valid across growth, only references/iterators
                    // taken before it don't).
                    struct LabelAccumulator
                    {
                        Shape combined;
                        size_t first_shape_index = 0;
                    };
                    std::unordered_map<std::string, LabelAccumulator> by_layer;

                    for (auto port_id : root.get_terminal_ports(terminal_id))
                    {
                        for (const auto &shape_id : root.get_terminal_port_shapes(port_id))
                        {
                            const auto *raw_shape = root.get_shape(shape_id);
                            if (!raw_shape)
                                continue;
                            const Shape shape = expand_iterates(*raw_shape);
                            auto [it, inserted] = by_layer.try_emplace(shape.layer_name);
                            if (inserted)
                                it->second.first_shape_index = shapes.size();

                            Shape &combined = it->second.combined;
                            combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                            combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                            combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                            shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::TERMINAL), .origin = SelectionRef{terminal_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
                        }
                    }

                    if (by_layer.empty())
                        continue;

                    if (const TerminalData *terminal = root.get_terminal(terminal_id))
                    {
                        for (const auto &[layer_name, acc] : by_layer)
                        {
                            const Point location = Geometry::get_label_location(acc.combined);
                            shapes[acc.first_shape_index].shape.texts.push_back(Text{
                                .label = terminal->name,
                                .location = location,
                                .size = Geometry::local_width_at(acc.combined, location),
                            });
                        }
                    }
                }

                for (auto obstruction_id : obstructions)
                {
                    for (const auto &shape_id : root.get_obstruction_shapes(obstruction_id))
                    {
                        const auto *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape)
                            continue;
                        const Shape shape = expand_iterates(*raw_shape);
                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::OBSTRUCTION), .origin = SelectionRef{obstruction_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
                    }
                }

                if (const AbstractData *abstract = root.get_abstract(abstract_id); abstract && !abstract->boundary.empty())
                {
                    shapes.push_back(RenderedShape{
                        .shape = Shape{.layer_name = "BOUNDARY", .polygons = abstract->boundary},
                        .view_layer = view_layers.boundary_view_layer(),
                    });
                }

                return shapes;
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> stage_;
    };
}
