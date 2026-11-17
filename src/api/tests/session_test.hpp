#pragma once
// A GTest fixture owning one le::Session, with the shorthand the api tests
// share. Each member opens its own view for the one call, so never call one
// while the test holds a view on the same thread.

#include "views.hpp"
#include "../../core/themes.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace le::test
{
    /// @brief `name` under src/api/tests/fixtures.
    inline std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    /// @brief A session and the shorthand the api tests share; a test can
    /// make a second one to check what carries over.
    struct SessionHelpers
    {
        Session session;
        LeHandle *handle = session.handle();

        WriteView write() { return session.write(); }
        WriteView write(const std::string &label) { return session.write(label); }
        ReadView read() const { return session.read(); }

        /// @brief Reads `path` (absolute, as fixture_path gives) into `library`.
        bool read_lef(const std::string &path, const std::string &library) { return write().read_lef(path, library).has_value(); }
        bool read_def(const std::string &path, const std::string &library) { return write().read_def(path, library).has_value(); }

        /// @brief The `index`th live Design, in pool order; invalid past the end.
        DesignId design_at(size_t index) const
        {
            const ReadView view = read();
            const auto ids = view.root().get_design_ids();
            return index < ids.size() ? ids[index] : DesignId{};
        }
        /// @brief The first Design named `name`; invalid if none.
        DesignId design_named(const std::string &name) const
        {
            const ReadView view = read();
            for (const DesignId id : view.root().get_design_ids())
                if (view.root().get_design(id)->name == name)
                    return id;
            return {};
        }
        size_t design_count() const { return read().root().get_design_ids().size(); }
        std::string design_name(size_t index) const
        {
            const DesignId id = design_at(index);
            const ReadView view = read();
            const DesignData *design = view.root().get_design(id);
            return design ? design->name : std::string();
        }

        /// @brief A Library and its name, as the Browser lists it.
        struct LibraryInfo
        {
            LibraryId id;
            std::string name;
        };
        /// @brief A Design and its views, as the Browser lists it.
        struct DesignInfo
        {
            LibraryId library_id;
            DesignId id;
            AbstractId abstract_id;
            LayoutId layout_id;
            std::string name;
        };
        int library_count() const { return static_cast<int>(read().root().get_library_ids().size()); }
        /// @brief The `index`th Library; invalid past the end.
        LibraryInfo library_at(size_t index) const
        {
            const ReadView view = read();
            const auto ids = view.root().get_library_ids();
            if (index >= ids.size())
                return {};
            return {ids[index], view.root().get_library(ids[index])->name};
        }
        int library_design_count(size_t library_index) const
        {
            const LibraryId library = library_at(library_index).id;
            const ReadView view = read();
            return library.valid() ? static_cast<int>(view.root().get_library_designs(library).size()) : 0;
        }
        /// @brief Design `design_index` of Library `library_index`; invalid past the end.
        DesignInfo library_design_at(size_t library_index, size_t design_index) const
        {
            const LibraryId library = library_at(library_index).id;
            const ReadView view = read();
            if (!library.valid())
                return {};
            const auto &designs = view.root().get_library_designs(library);
            if (design_index >= designs.size())
                return {};
            const DesignId id = designs[design_index];
            return {library, id, view.root().get_design_abstract(id), view.root().get_design_layout(id), view.root().get_design(id)->name};
        }

        /// @brief Opens the `index`th Design's Abstract.
        bool open_abstract(size_t index)
        {
            const DesignId design = design_at(index);
            return write().open_abstract(design).has_value();
        }
        bool open_abstract(DesignId design) { return write().open_abstract(design).has_value(); }
        bool open_layout(DesignId design) { return write().open_layout(design).has_value(); }

        RenderedFrame render() const { return session.renderer().render(); }

        /// @brief A click (mouse down and up) at pixel (x, y).
        void click(int x, int y)
        {
            WriteView view = write();
            view.mouse_down(x, y);
            view.mouse_up(x, y);
        }

        int selection_count() const { return static_cast<int>(read().selection().size()); }
        /// @brief Selection entry `index` as the Property Viewer shows it (a
        /// piece as its Shape or Wire); nullopt past the end.
        std::optional<ObjectId> selected(size_t index) const;
        /// @brief Whether `id` is set and an `Id`.
        template <class Id>
        static bool is(const std::optional<ObjectId> &id)
        {
            return id && std::holds_alternative<Id>(*id);
        }
        /// @brief `id`'s owner; nullopt if it has none.
        std::optional<ObjectId> parent(const std::optional<ObjectId> &id) const
        {
            return id ? read().parent(*id) : std::nullopt;
        }
        /// @brief `id`'s property table; empty if it's unset or doesn't exist.
        std::vector<PropertyValue> properties(const std::optional<ObjectId> &id) const
        {
            return id ? read().properties(*id) : std::vector<PropertyValue>{};
        }
        /// @brief Selects every piece of `object`; true on success.
        bool select(SelectableObject object) { return write().select(object).has_value(); }

        /// @brief The current Technology's layer named `name`; invalid if none.
        LayerId layer_named(const std::string &name) const { return read().root().get_layer_by_name(name); }

        /// @brief A copy of `shape`'s data; a default one if it doesn't exist.
        ShapeData shape(ShapeId shape) const
        {
            const ReadView view = read();
            const ShapeData *data = view.root().get_shape(shape);
            return data ? *data : ShapeData{};
        }
        /// @brief Rect `index` of `shape`, in microns.
        RectUm rect_um(ShapeId id, size_t index) const { return units().to_um(shape(id).rects[index]); }
        /// @brief Point `index` of polygon `polygon` of `shape`, in microns.
        PointUm polygon_point_um(ShapeId id, size_t polygon, size_t index) const { return units().to_um(shape(id).polygons[polygon].points.at(index)); }
        /// @brief Point `index` of path `path` of `shape`, in microns.
        PointUm path_point_um(ShapeId id, size_t path, size_t index) const { return units().to_um(shape(id).paths[path].polygon.points.at(index)); }
        double path_width_um(ShapeId id, size_t path) const { return units().to_um(shape(id).paths[path].width); }

        /// @brief Child `index` of `parent` in a Root child list, e.g.
        /// child_at(&Root::get_obstruction_shapes, obstruction, 0); invalid past the end.
        template <class Parent, class Child>
        Child child_at(const std::vector<Child> &(Root::*list)(Parent) const, Parent parent, size_t index) const
        {
            const ReadView view = read();
            const std::vector<Child> &children = (view.root().*list)(parent);
            return index < children.size() ? children[index] : Child{};
        }
        template <class Parent, class Child>
        int child_count(const std::vector<Child> &(Root::*list)(Parent) const, Parent parent) const
        {
            const ReadView view = read();
            return static_cast<int>((view.root().*list)(parent).size());
        }

        /// @brief Layers-panel row `layer`'s color as 0xRRGGBB; -1 if there's no such row.
        int layer_color_rgb(const std::string &layer) const
        {
            const std::optional<Color> color = read().layer_color(layer);
            return color ? (color->r << 16) | (color->g << 8) | color->b : -1;
        }
        void set_layer_color(const std::string &layer, uint8_t r, uint8_t g, uint8_t b) { write().set_layer_color(layer, Color{r, g, b, 255}); }
        /// @brief The GUI theme's name.
        std::string theme() const { return std::string(kThemes[read().theme()].name); }

        /// @brief The name of `shape`'s layer; empty if it has none.
        std::string shape_layer_name(ShapeId shape) const
        {
            const ReadView view = read();
            const ShapeData *data = view.root().get_shape(shape);
            const LayerData *layer = data ? view.root().get_layer(data->layer) : nullptr;
            return layer ? layer->name : std::string();
        }

        /// @brief Micron conversion at the current Technology's scale, or
        /// 1 dbu per micron before one is read.
        Units units() const { return read().units().value_or(Units(1.0)); }
        /// @brief The `index`th purpose in the Layers panel's drawing order,
        /// as its ordinal; -1 past the end.
        int purpose_at(size_t index) const
        {
            const auto purposes = read().purposes();
            return index < purposes.size() ? static_cast<int>(purposes[index]) : -1;
        }
        int purpose_count() const { return static_cast<int>(read().purposes().size()); }

        int ruler_point_count(size_t ruler) const
        {
            const ReadView view = read();
            return ruler < view.rulers().size() ? static_cast<int>(view.rulers()[ruler].points.size()) : 0;
        }
        /// @brief A ruler point in microns.
        PointUm ruler_point_at(size_t ruler, size_t point) const
        {
            const ReadView view = read();
            return view.units()->to_um(view.rulers()[ruler].points.at(point));
        }
        /// @brief The snapped mouse position as the status bar shows it
        /// (display_point), in microns; nullopt off the view or with no Technology.
        std::optional<PointUm> snapped_mouse_position() const
        {
            const ReadView view = read();
            const auto snapped = view.snapped_mouse_position();
            const auto u = view.units();
            if (!snapped || !u)
                return std::nullopt;
            const auto [x, y] = view.display_point(*snapped);
            return PointUm{x / u->dbu_per_um(), y / u->dbu_per_um()};
        }
        /// @brief The Technology's MANUFACTURINGGRID in microns; 0 if none.
        double manufacturing_grid_um() const
        {
            const ReadView view = read();
            const auto grid = view.manufacturing_grid();
            return grid && view.units() ? view.units()->to_um(*grid) : 0.0;
        }

        /// @brief A Shape of `rects_um` ({llx, lly, urx, ury} each, in
        /// microns) on `layer`, or layer-less on `purpose`; invalid on failure.
        ShapeId create_rects(ShapeOwner owner, LayerId layer, std::span<const double> rects_um, std::optional<ShapePurpose> purpose = std::nullopt)
        {
            const Units u = units();
            ShapeData data{.owner = owner, .layer = layer, .purpose = purpose};
            for (size_t i = 0; i + 3 < rects_um.size(); i += 4)
                data.rects.push_back(u.to_dbu(RectUm{{rects_um[i], rects_um[i + 1]}, {rects_um[i + 2], rects_um[i + 3]}}));
            return write().create_shape(std::move(data)).value_or(ShapeId{});
        }
        /// @brief A Placement of `design` at `location_um`; invalid on failure.
        PlacementId create_placement(LayoutId layout, DesignId design, const std::string &name, PointUm location_um,
                                     Orientation orientation = Orientation::N, PlacementStatus status = PlacementStatus::PLACED)
        {
            const Units u = units();
            return write()
                .create_placement({.layout = layout,
                                   .name = name,
                                   .reference_design = design,
                                   .placement_status = status,
                                   .location = u.to_dbu(location_um),
                                   .orientation = orientation})
                .value_or(PlacementId{});
        }
        /// @brief A Row of `site` at `origin_um`; invalid on failure.
        RowId create_row(LayoutId layout, const std::string &name, const std::string &site, std::optional<PointUm> origin_um,
                         Orientation orientation = Orientation::N, std::optional<int> num_x = std::nullopt)
        {
            const Units u = units();
            RowData data{.layout = layout, .name = name, .site_name = site, .orientation = orientation, .num_x = num_x};
            if (origin_um)
                data.origin = u.to_dbu(*origin_um);
            return write().create_row(std::move(data)).value_or(RowId{});
        }

        /// @brief A Site in the current Technology `size_um` big; invalid on failure.
        SiteId create_site(const std::string &name, PointUm size_um, std::optional<Symmetry> symmetry = std::nullopt)
        {
            const Units u = units();
            const TechnologyId technology = read().current_technology();
            return write().create_site({.technology = technology, .name = name, .size = u.to_dbu(size_um), .symmetry = symmetry}).value_or(SiteId{});
        }

        /// @brief Fits a micron rect into the viewport.
        void fit_rect(double llx, double lly, double urx, double ury, int padding_px)
        {
            const Units u = units();
            write().fit_rect(u.to_dbu(RectUm{{llx, lly}, {urx, ury}}), padding_px);
        }
    };

    struct SessionTest : ::testing::Test, SessionHelpers
    {
    };

    inline std::optional<ObjectId> SessionHelpers::selected(size_t index) const
    {
        const ReadView view = read();
        const auto selection = view.selection();
        if (index >= selection.size())
            return std::nullopt;
        return std::visit(
            [](const auto &entry) -> ObjectId
            {
                using Entry = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<Entry, ShapePiece>)
                {
                    if (entry.wire_id.valid())
                        return entry.wire_id;
                    return entry.shape_id;
                }
                else
                    return entry;
            },
            selection[index]);
    }
}
