#include "api_internal.hpp"
#include "hit_test.hpp"
#include "../geometry/geometry.hpp"
#include "../geometry/row_geometry.hpp"
#include "../pipelines/via_shapes.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace le::api_detail;

namespace le::api_detail
{
    // --- Vias as selectable, movable pieces ---

    // One via instance's own drawn geometry, in world space - every layer's
    // cut/enclosure rects - from the same expansion RasterizeBlend2DStage
    // draws (via_shapes.hpp), gathered into one plain Shape for selection
    // outlines, Move ghosts and hit boxes. A via piece's own extract_piece
    // Shape holds just the ShapeVia record, which stroke_piece_outline
    // can't draw.
    // `vias_only` holds just the via/via array to expand (its vias/
    // via_iterates); a via array expands to every one of its instances.
    le::ShapeData expanded_via_geometry(const le::Root &root, const le::ShapeData &vias_only, le::LayoutId layout_id)
    {
        static const le::ViewLayerSet no_view_layers; // every expanded shape lands under one (invalid) key - only the geometry matters here
        const le::ShapeData &one = vias_only;
        std::unordered_map<le::ViewLayerId, std::vector<le::RenderShape>> by_layer;
        le::append_via_shapes(root, one, le::ViewLayerPurpose::ROUTE, no_view_layers, layout_id, by_layer);

        le::ShapeData out;
        for (auto &[view_layer, shapes] : by_layer)
            for (le::RenderShape &shape : shapes)
            {
                out.rects.insert(out.rects.end(), shape.rects.begin(), shape.rects.end());
                out.polygons.insert(out.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                out.paths.insert(out.paths.end(), shape.paths.begin(), shape.paths.end());
            }
        return out;
    }

    le::ShapeData via_instance_geometry(const le::Root &root, const le::ShapeVia &via, le::LayoutId layout_id)
    {
        return expanded_via_geometry(root, le::ShapeData{.vias = {via}}, layout_id);
    }

    // A selected piece's drawable geometry: its own one-piece Shape, or for
    // a via or via array, its expanded geometry.
    le::ShapeData drawable_piece(const le::Root &root, const le::ShapeData &data, le::PieceKind kind, size_t index, le::LayoutId layout_id)
    {
        if (kind == le::PieceKind::VIA || kind == le::PieceKind::VIA_ITERATE)
            return le::Geometry::piece_in_range(data, kind, index) ? expanded_via_geometry(root, le::Geometry::extract_piece(data, kind, index), layout_id) : le::ShapeData{};
        return le::Geometry::extract_piece(data, kind, index);
    }
}

namespace
{
    // World-space hit boxes of via instances for one hit-test call - each
    // distinct via definition (name, plus the routed width a GENERATE via
    // sizes itself from) is expanded once, at the origin, then placed per
    // instance by its orientation and origin (via_shapes.hpp's own
    // transform: orientation about the via's origin, then translate).
    class ViaHitBoxes
    {
    public:
        ViaHitBoxes(const le::Root &root, le::LayoutId layout_id) : root_(root), layout_id_(layout_id) {}

        std::optional<le::Rect> bbox(const le::ShapeVia &via)
        {
            // Looked up by name without building a key string - this runs
            // once per via instance of every candidate a click tests.
            auto &by_width = local_[via.via_name];
            const int64_t width = via.width.value_or(-1);
            auto it = std::ranges::find(by_width, width, &std::pair<int64_t, std::optional<le::Rect>>::first);
            if (it == by_width.end())
            {
                le::ShapeVia at_origin = via;
                at_origin.origin = le::Point{};
                at_origin.orientation = le::Orientation::N;
                by_width.emplace_back(width, le::Geometry::bbox(via_instance_geometry(root_, at_origin, layout_id_)));
                it = by_width.end() - 1;
            }
            if (!it->second)
                return std::nullopt;
            const le::Geometry::InstanceTransform transform{
                .linear = le::Geometry::orientation_linear(via.orientation.value_or(le::Orientation::N)),
                .translation = via.origin,
            };
            return le::Geometry::transform_bbox(transform, *it->second);
        }

        // A via array's box: its first instance's box, stretched to cover
        // every DO/BY step (via_shapes.hpp places instance (ix, iy) at
        // origin + (ix*space_x, iy*space_y), each with the same orientation).
        std::optional<le::Rect> bbox(const le::ShapeViaIterate &array)
        {
            if (array.num_x <= 0 || array.num_y <= 0)
                return std::nullopt;
            std::optional<le::Rect> box = bbox(le::ShapeVia{.via_name = array.via_name, .origin = array.origin, .orientation = array.orientation, .width = array.width});
            if (!box)
                return std::nullopt;
            const int64_t span_x = static_cast<int64_t>(array.num_x - 1) * array.space_x;
            const int64_t span_y = static_cast<int64_t>(array.num_y - 1) * array.space_y;
            box->ll.x += std::min<int64_t>(0, span_x);
            box->ur.x += std::max<int64_t>(0, span_x);
            box->ll.y += std::min<int64_t>(0, span_y);
            box->ur.y += std::max<int64_t>(0, span_y);
            return box;
        }

    private:
        const le::Root &root_;
        le::LayoutId layout_id_;
        // Transparent, so lookup by the via's own name allocates nothing.
        struct NameHash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
        };
        std::unordered_map<std::string, std::vector<std::pair<int64_t, std::optional<le::Rect>>>, NameHash, std::equal_to<>> local_;
    };

    // Visits (id, purpose) for every Shape or Wire in the current view that
    // can own a selectable via: an Abstract's terminal-port and
    // obstruction Shapes, or a Layout's route Wires and Shapes.
    template <typename Visit>
    void for_each_via_owner_shape(const LeHandle *handle, Visit visit)
    {
        const le::Root &root = handle->root;
        if (handle->current_layout().valid())
        {
            for (const le::RouteId route_id : root.get_layout_routes(handle->current_layout()))
            {
                for (const le::WireId wire_id : root.get_route_wires(route_id))
                    visit(le::GeometryId::of(wire_id), le::ViewLayerPurpose::ROUTE);
                for (const le::ShapeId shape_id : root.get_route_shapes(route_id))
                    visit(le::GeometryId::of(shape_id), le::ViewLayerPurpose::ROUTE);
            }
            return;
        }
        const le::AbstractId abstract_id = handle->current_abstract();
        for (const le::TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
            for (const le::TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                for (const le::ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                    visit(le::GeometryId::of(shape_id), le::ViewLayerPurpose::TERMINAL);
        for (const le::ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
            for (const le::ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
                visit(le::GeometryId::of(shape_id), le::ViewLayerPurpose::OBSTRUCTION);
    }

    // A via is selectable when its owning Shape's own layer/purpose is
    // visible and selectable - the same rule its sibling rects/paths follow.
    bool via_owner_selectable(const LeHandle *handle, const le::ShapeData &shape, le::ViewLayerPurpose purpose)
    {
        const le::LayerData *layer = handle->root.get_layer(shape.layer);
        return layer && handle->is_view_layer_visible(layer->name, purpose) && handle->is_view_layer_selectable(layer->name, purpose);
    }

    // Placements are selectable only while the PLACEMENT purpose (their
    // outline and name) is visible and selectable.
    bool placements_selectable(const LeHandle *handle)
    {
        return handle->is_view_layer_visible("PLACEMENT", le::ViewLayerPurpose::PLACEMENT) &&
               handle->is_view_layer_selectable("PLACEMENT", le::ViewLayerPurpose::PLACEMENT);
    }

    // Whether `object` survives the Placement and route value filters -
    // none of its values is hidden or unselectable. A Route's shape pieces
    // and vias carry the Route's use and their own Wire's or Shape's
    // routing status and SHAPE; everything else passes.
    bool passes_object_filters(const LeHandle *handle, const LeHandle::SelectedObject &object)
    {
        const le::ObjectFilterSets &hidden = handle->hidden_objects();
        const le::ObjectFilterSets &unselectable = handle->unselectable_objects();
        const le::Root &root = handle->root;
        if (const auto *piece = std::get_if<LeHandle::ShapePiece>(&object))
        {
            const le::GeometryId owner = piece->owner();
            const le::RouteData *route = root.get_route(le::geometry_route(root, owner));
            if (!route)
                return true;
            if (!hidden.route_uses.empty() || !unselectable.route_uses.empty())
            {
                const std::string use = le::route_use(*route);
                if (hidden.route_uses.contains(use) || unselectable.route_uses.contains(use))
                    return false;
            }
            const le::RouteWiringFilter hidden_wiring = le::RouteWiringFilter::from(hidden);
            const le::RouteWiringFilter unselectable_wiring = le::RouteWiringFilter::from(unselectable);
            auto filtered = [&](const auto &wiring)
            { return hidden_wiring.hides(wiring) || unselectable_wiring.hides(wiring); };
            if (const le::ShapeData *shape = root.get_shape(owner.shape))
                return !filtered(*shape);
            if (const le::WireData *wire = root.get_wire(owner.wire))
                return !filtered(*wire);
            return true;
        }
        if (const auto *placement_id = std::get_if<le::PlacementId>(&object))
        {
            const le::PlacementData *placement = root.get_placement(*placement_id);
            if (!placement)
                return true;
            if (!hidden.placement_types.empty() || !unselectable.placement_types.empty())
            {
                const std::string type = le::placement_type(root, placement->reference_design);
                if (hidden.placement_types.contains(type) || unselectable.placement_types.contains(type))
                    return false;
            }
            return !le::placement_status_mask(hidden).hides(placement->placement_status) &&
                   !le::placement_status_mask(unselectable).hides(placement->placement_status);
        }
        return true;
    }

    bool rows_selectable(const LeHandle *handle)
    {
        return handle->is_view_layer_visible("ROW", le::ViewLayerPurpose::ROW) &&
               handle->is_view_layer_selectable("ROW", le::ViewLayerPurpose::ROW);
    }

    // Candidates for a Layout-view click or rubber band over `query`: the
    // objects whose rendered geometry overlaps it, read from the render
    // tree (the last resolver output's per-chunk rtrees and their sources -
    // hierarchy_resolver_stage.hpp's ChunkSources), plus every object edited
    // since that output (the tree may still show it somewhere else; the
    // exact hit test sorts that out). nullopt when that output doesn't
    // match the current view, or when something since could have moved
    // objects without their own log entries - the caller then scans the
    // whole Layout, which is always correct.
    struct LayoutCandidates
    {
        std::vector<le::GeometryId> shapes;
        std::vector<le::PlacementId> placements;
    };
    std::optional<LayoutCandidates> layout_candidates(const LeHandle *handle, le::Rect query)
    {
        const le::Root &root = handle->root;
        const le::LayoutId layout_id = handle->current_layout();
        const le::HierarchyResolverStage::OutputHandle output = handle->view_render_pipeline.resolved_output();
        if (!output || !output->view_layers || output->root != &root || output->top_level != le::HierarchyId{layout_id} ||
            output->hierarchy_depth != handle->hierarchy_depth() || !root.change_log().covers(output->log_end))
            return std::nullopt;
        const auto top = output->view_data.find(le::HierarchyId{layout_id});
        if (top == output->view_data.end())
            return std::nullopt;

        LayoutCandidates out;
        bool usable = true;
        root.change_log().for_each_since(output->log_end, [&](const le::ChangeLogEntry &entry)
                                         {
            if (!usable)
                return;
            switch (entry.klass)
            {
            case le::ChangeKlass::Shape:
            {
                const std::string_view field = le::Root::change_parent_field_name(le::ChangeKlass::Shape, entry.parent.slot);
                if (entry.parent.klass != le::ChangeKlass::None && (field == "layout" || field == "abstract"))
                    usable = false; // a boundary - it sizes placements
                else
                    out.shapes.push_back(le::GeometryId::of(le::ShapeId{entry.index, entry.generation}));
                return;
            }
            case le::ChangeKlass::Wire:
                out.shapes.push_back(le::GeometryId::of(le::WireId{entry.index, entry.generation}));
                return;
            case le::ChangeKlass::Route:
            {
                const le::RouteId route{entry.index, entry.generation};
                for (const le::WireId wire : root.get_route_wires(route))
                    out.shapes.push_back(le::GeometryId::of(wire));
                for (const le::ShapeId shape : root.get_route_shapes(route))
                    out.shapes.push_back(le::GeometryId::of(shape));
                return;
            }
            case le::ChangeKlass::PhysicalPortSegment:
                for (const le::ShapeId shape : root.get_physical_port_segment_shapes(le::PhysicalPortSegmentId{entry.index, entry.generation}))
                    out.shapes.push_back(le::GeometryId::of(shape));
                return;
            case le::ChangeKlass::PhysicalPort:
                for (const le::PhysicalPortSegmentId segment : root.get_physical_port_segments(le::PhysicalPortId{entry.index, entry.generation}))
                    for (const le::ShapeId shape : root.get_physical_port_segment_shapes(segment))
                        out.shapes.push_back(le::GeometryId::of(shape));
                return;
            case le::ChangeKlass::Placement:
                out.placements.push_back(le::PlacementId{entry.index, entry.generation});
                return;
            // Not selectable here, and no effect on what is.
            case le::ChangeKlass::Layer:
            case le::ChangeKlass::Terminal:
            case le::ChangeKlass::TerminalPort:
            case le::ChangeKlass::Obstruction:
            case le::ChangeKlass::Blockage:
            case le::ChangeKlass::Row:
            case le::ChangeKlass::Track:
            case le::ChangeKlass::GCellGrid:
            case le::ChangeKlass::Region:
            case le::ChangeKlass::Net:
            case le::ChangeKlass::NetBus:
            case le::ChangeKlass::Instance:
            case le::ChangeKlass::Pin:
            case le::ChangeKlass::Port:
            case le::ChangeKlass::PortBus:
            case le::ChangeKlass::Schematic:
            case le::ChangeKlass::PropertyDefinition:
                return;
            default: // via definitions, designs, abstracts, layouts, technology, ...
            {
                // A renderable class's object: its shapes are candidates.
                bool renderable_object = false;
                le::renderable::for_each([&]<class R>(R) {
                    if (entry.klass != R::klass)
                        return;
                    renderable_object = true;
                    for (const le::ShapeId shape : R::shapes(root, typename R::Id{entry.index, entry.generation}))
                        out.shapes.push_back(le::GeometryId::of(shape));
                });
                if (!renderable_object)
                    usable = false;
                return;
            }
            } });
        if (!usable)
            return std::nullopt;

        namespace bgi = boost::geometry::index;
        const le::ViewLayerId placement_layer = output->view_layers->placement_view_layer();
        std::vector<le::ShapeIndexEntry> hits;
        for (const le::ViewShapeChunk &chunk : top->second.chunks)
        {
            if (!chunk.sources || !chunk.shapes_index)
                continue;
            for (const auto &[view_layer, index] : *chunk.shapes_index)
            {
                hits.clear();
                index.query(bgi::intersects(query), std::back_inserter(hits));
                if (hits.empty())
                    continue;
                if (view_layer == placement_layer && !chunk.sources->placements.empty())
                {
                    // A tile's batched PLACEMENT shape: rect i is placement i.
                    for (const le::ShapeIndexEntry &hit : hits)
                    {
                        const auto &rects = chunk.shapes->at(view_layer)[hit.second].rects;
                        for (size_t i = 0; i < rects.size() && i < chunk.sources->placements.size(); ++i)
                            if (boost::geometry::intersects(rects[i], query))
                                out.placements.push_back(chunk.sources->placements[i]);
                    }
                    continue;
                }
                const auto sources = chunk.sources->shapes.find(view_layer);
                if (sources == chunk.sources->shapes.end())
                    continue;
                for (const le::ShapeIndexEntry &hit : hits)
                    if (hit.second < sources->second.size() && sources->second[hit.second].valid())
                        out.shapes.push_back(sources->second[hit.second]);
            }
        }

        // Only this Layout's route/port/renderable-class shapes and
        // placements (edits anywhere are in the log), each once.
        std::erase_if(out.shapes, [&](le::GeometryId id)
                      {
            if (const le::WireData *wire = root.get_wire(id.wire))
            {
                const le::RouteData *route = root.get_route(wire->route);
                return !route || route->layout != layout_id;
            }
            const le::ShapeData *shape = root.get_shape(id.shape);
            if (!shape)
                return true;
            if (const le::RouteData *route = root.get_route(shape->route()))
                return route->layout != layout_id;
            if (const le::PhysicalPortSegmentData *segment = root.get_physical_port_segment(shape->physical_port_segment()))
                if (const le::PhysicalPortData *port = root.get_physical_port(segment->physical_port))
                    return port->layout != layout_id;
            bool in_layout = false;
            le::renderable::for_each([&]<class R>(R) {
                if (const auto owner = R::owner_of(*shape); owner.valid())
                    in_layout = R::layout_of(root, owner) == layout_id;
            });
            return !in_layout; });
        std::erase_if(out.placements, [&](le::PlacementId id)
                      {
            const le::PlacementData *placement = root.get_placement(id);
            return !placement || placement->layout != layout_id; });
        std::ranges::sort(out.shapes);
        out.shapes.erase(std::unique(out.shapes.begin(), out.shapes.end()), out.shapes.end());
        std::ranges::sort(out.placements);
        out.placements.erase(std::unique(out.placements.begin(), out.placements.end()), out.placements.end());
        ++handle->render_tree_selections;
        return out;
    }

    // for_each_via_owner_shape, limited in a Layout view to `candidates`
    // (layout_candidates) when given - its via owners are route shapes.
    template <typename Visit>
    void for_each_via_candidate(const LeHandle *handle, const std::vector<le::GeometryId> *candidates, Visit visit)
    {
        if (!candidates || !handle->current_layout().valid())
        {
            for_each_via_owner_shape(handle, visit);
            return;
        }
        for (const le::GeometryId id : *candidates)
            if (le::geometry_route(handle->root, id).valid())
                visit(id, le::ViewLayerPurpose::ROUTE);
    }

    // Every selectable via or via array whose hit box contains dbu `p`,
    // smallest first (a via stacked inside a bigger one, or inside an
    // array's box, comes before it). A click anywhere in an array's box -
    // between its instances too - picks the whole array.
    std::vector<LeHandle::ShapePiece> hit_test_via_point_all(const LeHandle *handle, le::Point p, const std::vector<le::GeometryId> *candidates = nullptr)
    {
        ViaHitBoxes boxes(handle->root, handle->current_layout());
        std::vector<std::pair<double, LeHandle::ShapePiece>> hits;
        le::ShapeData scratch;
        for_each_via_candidate(handle, candidates, [&](le::GeometryId id, le::ViewLayerPurpose purpose)
                                 {
            if (const le::WireData *wire = handle->root.get_wire(id.wire); wire && wire->vias.empty())
                return;
            const le::ShapeData *shape = le::geometry_shape(handle->root, id, scratch);
            if (!shape || (shape->vias.empty() && shape->via_iterates.empty()) || !via_owner_selectable(handle, *shape, purpose))
                return;
            const auto consider = [&](const std::optional<le::Rect> &box, le::PieceKind kind, size_t i)
            {
                if (!box || p.x < box->ll.x || p.x > box->ur.x || p.y < box->ll.y || p.y > box->ur.y)
                    return;
                const double area = static_cast<double>(box->ur.x - box->ll.x) * static_cast<double>(box->ur.y - box->ll.y);
                hits.emplace_back(area, LeHandle::ShapePiece{.shape_id = id.shape, .wire_id = id.wire, .piece_kind = kind, .piece_index = i});
            };
            for (size_t i = 0; i < shape->vias.size(); ++i)
                consider(boxes.bbox(shape->vias[i]), le::PieceKind::VIA, i);
            for (size_t i = 0; i < shape->via_iterates.size(); ++i)
                consider(boxes.bbox(shape->via_iterates[i]), le::PieceKind::VIA_ITERATE, i); });
        std::ranges::stable_sort(hits, {}, [](const auto &h)
                                 { return h.first; });
        std::vector<LeHandle::ShapePiece> out;
        out.reserve(hits.size());
        for (const auto &[area, piece] : hits)
            out.push_back(piece);
        return out;
    }

    // Every selectable via or via array whose hit box lies entirely inside
    // `rect` - the rubber-band rule every other piece follows.
    std::vector<LeHandle::ShapePiece> hit_test_via_rect(const LeHandle *handle, le::Rect rect, const std::vector<le::GeometryId> *candidates = nullptr)
    {
        ViaHitBoxes boxes(handle->root, handle->current_layout());
        std::vector<LeHandle::ShapePiece> hits;
        le::ShapeData scratch;
        for_each_via_candidate(handle, candidates, [&](le::GeometryId id, le::ViewLayerPurpose purpose)
                                 {
            if (const le::WireData *wire = handle->root.get_wire(id.wire); wire && wire->vias.empty())
                return;
            const le::ShapeData *shape = le::geometry_shape(handle->root, id, scratch);
            if (!shape || (shape->vias.empty() && shape->via_iterates.empty()) || !via_owner_selectable(handle, *shape, purpose))
                return;
            const auto consider = [&](const std::optional<le::Rect> &box, le::PieceKind kind, size_t i)
            {
                if (box && box->ll.x >= rect.ll.x && box->ll.y >= rect.ll.y && box->ur.x <= rect.ur.x && box->ur.y <= rect.ur.y)
                    hits.push_back(LeHandle::ShapePiece{.shape_id = id.shape, .wire_id = id.wire, .piece_kind = kind, .piece_index = i});
            };
            for (size_t i = 0; i < shape->vias.size(); ++i)
                consider(boxes.bbox(shape->vias[i]), le::PieceKind::VIA, i);
            for (size_t i = 0; i < shape->via_iterates.size(); ++i)
                consider(boxes.bbox(shape->via_iterates[i]), le::PieceKind::VIA_ITERATE, i); });
        return hits;
    }
}

namespace le::api_detail
{
    // Resolves one LeHandle::SelectedObject alternative to its own
    // dbu-space outline geometry, for ComposeStage's own selection
    // overlay. `remaining_depth` is only meaningful for a PlacementId
    // alternative (Layout-view top-level selection) - Abstract-view
    // selection never produces one, and a Placement never becomes a
    // ShapePiece (see LeHandle::SelectedObject's own comment).
    std::optional<le::ShapeData> resolve_selected_outline(const LeHandle *handle, const LeHandle::SelectedObject &selected, int remaining_depth)
    {
        return std::visit(
            [&](const auto &s) -> std::optional<le::ShapeData>
            {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                {
                    le::ShapeData scratch;
                    if (const le::ShapeData *data = le::geometry_shape(handle->root, s.owner(), scratch))
                        return drawable_piece(handle->root, *data, s.piece_kind, s.piece_index, handle->current_layout());
                    return std::nullopt;
                }
                else if constexpr (std::is_same_v<T, le::RowId>)
                {
                    if (auto bbox = le::row_footprint_bbox(handle->root, s))
                        return le::ShapeData{.rects = {*bbox}};
                    return std::nullopt;
                }
                else if constexpr (std::is_same_v<T, le::RegionId>)
                {
                    if (const le::RegionData *region = handle->root.get_region(s))
                        return le::ShapeData{.rects = region->rects};
                    return std::nullopt;
                }
                else // le::PlacementId
                {
                    if (!handle->current_layout().valid())
                        return std::nullopt;
                    if (auto bbox = le::placement_world_bbox(handle->root, s, remaining_depth))
                        return le::ShapeData{.rects = {*bbox}};
                    return std::nullopt;
                }
            },
            selected);
    }
    // Every PlacementId in the current selection, in selection order.
    std::vector<le::PlacementId> selected_placements_unlocked(const LeHandle *handle)
    {
        std::vector<le::PlacementId> ids;
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (const le::PlacementId *id = std::get_if<le::PlacementId>(&selected))
                ids.push_back(*id);
        return ids;
    }
}

namespace
{
    // Every selectable object under dbu `p`, in click priority order: vias
    // first (a via sits on top of the wires it joins), smallest first;
    // then shape pieces, topmost layer first; then (Layout view)
    // placements, topmost first, and last the Rows the placements sit on.
    std::vector<LeHandle::SelectedObject> objects_under_point_unlocked(const LeHandle *handle, le::Point p)
    {
        std::vector<LeHandle::SelectedObject> objects;
        // In a Layout view only the objects the render tree has under the
        // click are tested (layout_candidates), not the whole design.
        std::optional<std::vector<le::GeometryId>> shape_candidates;
        std::optional<std::vector<le::PlacementId>> placement_candidates;
        if (handle->current_layout().valid())
            if (std::optional<LayoutCandidates> candidates = layout_candidates(handle, le::Rect{.ll = p, .ur = p}))
            {
                shape_candidates = std::move(candidates->shapes);
                placement_candidates = std::move(candidates->placements);
            }
        for (const LeHandle::ShapePiece &via : hit_test_via_point_all(handle, p, shape_candidates ? &*shape_candidates : nullptr))
            objects.emplace_back(via);

        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };
        const auto add_pieces = [&](const std::vector<le::AbstractHitPiece> &hits)
        {
            for (const le::AbstractHitPiece &hit : hits)
                objects.emplace_back(LeHandle::ShapePiece{.shape_id = hit.shape_id, .wire_id = hit.wire_id, .piece_kind = hit.piece_kind, .piece_index = hit.piece_index});
        };

        if (const le::LayoutId layout_id = handle->current_layout(); layout_id.valid())
        {
            add_pieces(le::hit_test_layout_point_all(handle->root, handle->view_layers, layout_id, p, handle->scale(), is_selectable, false,
                                                     shape_candidates ? &*shape_candidates : nullptr));
            const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
            if (placements_selectable(handle))
                for (const le::PlacementId placement_id : le::hit_test_placements_point_all(handle->root, layout_id, remaining_depth, p,
                                                                                           placement_candidates ? &*placement_candidates : nullptr))
                    objects.emplace_back(placement_id);
            if (rows_selectable(handle))
                for (const le::RowId row_id : le::hit_test_rows_point_all(handle->root, layout_id, p))
                    objects.emplace_back(row_id);
        }
        else
            add_pieces(le::hit_test_abstract_point_all(handle->root, handle->view_layers, handle->current_abstract(), p, handle->scale(), is_selectable));
        std::erase_if(objects, [handle](const LeHandle::SelectedObject &object)
                      { return !passes_object_filters(handle, object); });
        return objects;
    }
}

namespace le::api_detail
{
    // le_mouse_up's Select-mode click. A plain click cycles through
    // everything under the mouse: if the one object selected before the
    // click is among them, the next one (wrapping round) is selected
    // instead, so repeated clicks at one spot step through overlapping
    // objects; otherwise the first. A shift-click adds the first object
    // under the mouse that isn't selected yet, so repeated shift-clicks at
    // one spot add the stacked objects one by one (and do nothing once
    // they're all selected).
    void click_select_unlocked(LeHandle *handle, le::Point p, const std::vector<LeHandle::SelectedObject> &previous, bool shift)
    {
        const std::vector<LeHandle::SelectedObject> candidates = objects_under_point_unlocked(handle, p);
        if (candidates.empty())
            return;

        if (shift)
        {
            // The selection wasn't cleared for a shift-click, so it's the
            // same as `previous` here.
            for (const LeHandle::SelectedObject &candidate : candidates)
                if (std::ranges::find(previous, candidate) == previous.end())
                {
                    handle->select_any(candidate);
                    return;
                }
            return;
        }

        size_t next = 0;
        if (previous.size() == 1)
            if (const auto it = std::ranges::find(candidates, previous.front()); it != candidates.end())
                next = (static_cast<size_t>(it - candidates.begin()) + 1) % candidates.size();
        handle->select_any(candidates[next]);
    }

    // le_mouse_up's Select-mode, Abstract-view drag-select branch (a
    // click goes through click_select_unlocked). Called with
    // handle->mutex_ already held, `x`/`y` the release point le_mouse_up
    // received. Hit-tests Root's raw ShapeData directly
    // (hit_test.hpp's hit_test_abstract_rect) rather than
    // rendered output - a drag is a rare, one-off query, not a per-frame
    // cost.
    void select_in_abstract_view_unlocked(LeHandle *handle, int32_t x, int32_t y)
    {
        const le::AbstractId abstract_id = handle->current_abstract();
        // Both visible AND selectable - see le_set_mouse_position's own
        // comment on this same predicate shape.
        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };

        {
            const le::Point start = handle->pixel_to_dbu(handle->drag_start_x_px(), handle->drag_start_y_px());
            const le::Point end = handle->pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            for (const le::AbstractHitPiece &hit : le::hit_test_abstract_rect(handle->root, handle->view_layers, abstract_id, drag_rect, handle->scale(), is_selectable))
                handle->select(hit.shape_id, hit.piece_kind, hit.piece_index);
            for (const LeHandle::ShapePiece &via : hit_test_via_rect(handle, drag_rect))
                handle->select(via.shape_id, via.piece_kind, via.piece_index);
        }
    }

    // le_mouse_up's Select-mode, Layout-view drag-select branch: selects
    // every top-level placement, Route/PhysicalPort shape piece, via and
    // Row fully inside the rectangle, never recursing into a Placement's
    // reference_design. Blockage and Region aren't hit-tested yet.
    void select_in_layout_view_unlocked(LeHandle *handle, int32_t x, int32_t y)
    {
        const le::LayoutId layout_id = handle->current_layout();
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
        // Both visible AND selectable - see le_set_mouse_position's own
        // comment on this same predicate shape.
        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };

        {
            const le::Point start = handle->pixel_to_dbu(handle->drag_start_x_px(), handle->drag_start_y_px());
            const le::Point end = handle->pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            // Only objects the render tree has overlapping the rectangle can
            // be inside it (layout_candidates); without one, scan everything.
            const std::optional<LayoutCandidates> candidates = layout_candidates(handle, drag_rect);
            const std::vector<le::GeometryId> *shape_candidates = candidates ? &candidates->shapes : nullptr;
            if (placements_selectable(handle))
                for (le::PlacementId placement_id : le::hit_test_placements_rect(handle->root, layout_id, remaining_depth, drag_rect,
                                                                                 candidates ? &candidates->placements : nullptr))
                    if (passes_object_filters(handle, placement_id))
                        handle->select(placement_id);

            for (const le::AbstractHitPiece &hit : le::hit_test_layout_rect(handle->root, handle->view_layers, layout_id, drag_rect, handle->scale(), is_selectable, shape_candidates))
            {
                const LeHandle::ShapePiece piece{.shape_id = hit.shape_id, .wire_id = hit.wire_id, .piece_kind = hit.piece_kind, .piece_index = hit.piece_index};
                if (passes_object_filters(handle, piece))
                    handle->select_any(piece);
            }
            for (const LeHandle::ShapePiece &via : hit_test_via_rect(handle, drag_rect, shape_candidates))
                if (passes_object_filters(handle, via))
                    handle->select_any(via);
            if (rows_selectable(handle))
                for (const le::RowId row_id : le::hit_test_rows_rect(handle->root, layout_id, drag_rect))
                    handle->select(row_id);
        }
    }
}

extern "C"
{
    void le_deselect_all(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_selection();
    }

    int32_t le_selected_shape_piece_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(std::ranges::count_if(handle->selection(), [](const LeHandle::SelectedObject &selected)
                                                          { return std::holds_alternative<LeHandle::ShapePiece>(selected); }));
    }

    int32_t le_selected_placement_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(selected_placements_unlocked(handle).size());
    }

    int32_t le_selection_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return static_cast<int32_t>(handle->selection().size());
    }

    int64_t le_selection_version(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return static_cast<int64_t>(handle->selection_version());
    }

    LeObjectRef le_selected_object_ref(LeHandle *handle, int32_t selection_index)
    {
        if (!handle || selection_index < 0)
            return invalid_object_ref();
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const std::vector<LeHandle::SelectedObject> &selection = handle->selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return invalid_object_ref();

        // Dispatches every SelectedObject alternative to its own
        // LeObjectKind; ShapePiece (Terminal/Obstruction/Blockage/Route/
        // PhysicalPort) resolves to LE_OBJECT_KIND_SHAPE, or
        // LE_OBJECT_KIND_WIRE for routed wiring - a Property Viewer wanting
        // the owning Blockage/Route/PhysicalPort instead walks up via
        // le_object_parent (see object_ref_parent's own Shape->blockage/
        // route/physical_port_segment and Wire->route hops).
        return std::visit([](const auto &s) -> LeObjectRef
                          {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                return s.wire_id.valid() ? ref_from_id(LE_OBJECT_KIND_WIRE, s.wire_id) : ref_from_id(LE_OBJECT_KIND_SHAPE, s.shape_id);
            else if constexpr (std::is_same_v<T, le::RowId>)
                return ref_from_id(LE_OBJECT_KIND_ROW, s);
            else if constexpr (std::is_same_v<T, le::PlacementId>)
                return ref_from_id(LE_OBJECT_KIND_PLACEMENT, s);
            else if constexpr (std::is_same_v<T, le::RegionId>)
                return ref_from_id(LE_OBJECT_KIND_REGION, s); }, selection[static_cast<size_t>(selection_index)]);
    }

    int32_t le_select_object_ref(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        // Shared by SHAPE/ROUTE/PHYSICAL_PORT below - selects every rect/
        // polygon/path entry of one Shape, since a bare ShapeId can't
        // express "just this one piece" (see this function's own
        // api.hpp doc comment). Returns whether it actually selected
        // anything, so a caller resolving several child Shapes (Route/
        // PhysicalPort) can tell "no geometry anywhere" apart from
        // "some/all of them had real geometry" with one shared check.
        const auto select_all_pieces_of = [&](le::ShapeId shape_id) -> bool
        {
            const le::ShapeData *shape = handle->root.get_shape(shape_id);
            if (!shape)
                return false;
            for (size_t i = 0; i < shape->rects.size(); i++)
                handle->select(shape_id, le::PieceKind::RECT, i);
            for (size_t i = 0; i < shape->polygons.size(); i++)
                handle->select(shape_id, le::PieceKind::POLYGON, i);
            for (size_t i = 0; i < shape->paths.size(); i++)
                handle->select(shape_id, le::PieceKind::PATH, i);
            for (size_t i = 0; i < shape->vias.size(); i++)
                handle->select(shape_id, le::PieceKind::VIA, i);
            for (size_t i = 0; i < shape->via_iterates.size(); i++)
                handle->select(shape_id, le::PieceKind::VIA_ITERATE, i);
            return !shape->rects.empty() || !shape->polygons.empty() || !shape->paths.empty() || !shape->vias.empty() || !shape->via_iterates.empty();
        };
        // The same for a Wire - its pieces are those of the Shape it
        // converts to (le::wire_to_shape): its paths, then its vias.
        const auto select_all_pieces_of_wire = [&](le::WireId wire_id) -> bool
        {
            const le::WireData *wire = handle->root.get_wire(wire_id);
            if (!wire)
                return false;
            size_t paths = 0;
            le::for_each_wire_path(*wire, [&](size_t, size_t)
                                   { handle->select(wire_id, le::PieceKind::PATH, paths++); });
            for (size_t i = 0; i < wire->vias.size(); i++)
                handle->select(wire_id, le::PieceKind::VIA, i);
            return paths > 0 || !wire->vias.empty();
        };

        switch (ref.kind)
        {
        case LE_OBJECT_KIND_SHAPE:
        {
            const le::ShapeId shape_id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_shape(shape_id))
            {
                spdlog::error("select: no such Shape");
                return 1;
            }
            if (!select_all_pieces_of(shape_id))
            {
                spdlog::error("select: Shape has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_ROUTE:
        {
            // A Route's own shapes ride the exact same ShapeId+piece
            // selection Terminal/Obstruction/Shape already use above -
            // Route (like Blockage/PhysicalPort) has a real backing
            // Shape per piece of routed geometry, unlike Row/Region.
            const le::RouteId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_route(id))
            {
                spdlog::error("select: no such Route");
                return 1;
            }
            bool any_selected = false;
            for (le::WireId wire_id : handle->root.get_route_wires(id))
                any_selected |= select_all_pieces_of_wire(wire_id);
            for (le::ShapeId shape_id : handle->root.get_route_shapes(id))
                any_selected |= select_all_pieces_of(shape_id);
            if (!any_selected)
            {
                spdlog::error("select: Route has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_WIRE:
        {
            const le::WireId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_wire(id))
            {
                spdlog::error("select: no such Wire");
                return 1;
            }
            if (!select_all_pieces_of_wire(id))
            {
                spdlog::error("select: Wire has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT:
        {
            const le::PhysicalPortId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_physical_port(id))
            {
                spdlog::error("select: no such PhysicalPort");
                return 1;
            }
            bool any_selected = false;
            for (le::PhysicalPortSegmentId segment_id : handle->root.get_physical_port_segments(id))
                for (le::ShapeId shape_id : handle->root.get_physical_port_segment_shapes(segment_id))
                    any_selected |= select_all_pieces_of(shape_id);
            if (!any_selected)
            {
                spdlog::error("select: PhysicalPort has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_ROW:
        {
            const le::RowId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_row(id))
            {
                spdlog::error("select: no such Row");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        case LE_OBJECT_KIND_PLACEMENT:
        {
            const le::PlacementId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_placement(id))
            {
                spdlog::error("select: no such Placement");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        case LE_OBJECT_KIND_REGION:
        {
            const le::RegionId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_region(id))
            {
                spdlog::error("select: no such Region");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        default:
            spdlog::error("select: unsupported object kind (only Shape/Route/PhysicalPort/Row/Placement/Region can be selected)");
            return 1;
        }
    }
}
