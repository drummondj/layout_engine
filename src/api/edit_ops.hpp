#pragma once
// The C++ half of the C API's edits: validate, change the Root, bump its
// mutation version and record undo, in native types (database ids, dbu,
// std::expected errors). The le_* functions convert their C arguments and
// call these under the handle's lock; extensions reach them through
// le::ext::WriteView. Every function here expects the caller to hold the
// handle's exclusive lock.

#include "api.hpp"
#include "database.hpp"
#include "generated/api/edit_types.hpp"
#include "../database/wire_helpers.hpp"
#include "../core/session_types.hpp"
#include "../geometry/shape_op_types.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace le::edit
{
    /// @brief update_placement's check (Klass.update_check): a FIXED or
    /// COVER placement's location and orientation can't change unless the
    /// same update unlocks it.
    std::optional<std::string> check_placement_update(const Root &root, const PlacementData &before, const PlacementChanges &changes);

    /// @brief create_instance's check (Klass.create_check): an Instance
    /// can't make its Design instantiate itself, directly or through
    /// its descendants.
    std::optional<std::string> check_instance_create(const Root &root, const InstanceData &data);

    /// @brief update_instance's check (Klass.update_check): the same rule
    /// as check_instance_create, for a new reference_design or schematic.
    std::optional<std::string> check_instance_update(const Root &root, const InstanceData &before, const InstanceChanges &changes);

#include "generated/api/edit_ops_decls.inc"

    /// @brief The Technology's dbu per micron (DATABASE MICRONS), if one
    /// with a positive scale has been read. The session has one Technology.
    std::optional<double> dbu_per_um(const Root &root);

    /// @brief New Shapes' ids, or a user-facing error.
    using ShapeOpResult = std::expected<std::vector<ShapeId>, std::string>;

    // The shape_* operations. Each new Shape goes to `parent`, else the
    // open view's Abstract/Layout (the GUI's, then the current one);
    // an unset `layer` keeps each input's own.

    /// @brief One new Shape per input, same geometry, on `layer`.
    ShapeOpResult shape_copy(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer,
                             const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape holding `op` of `a` against `b`, on a[0]'s layer unless `layer` is set.
    ShapeOpResult shape_boolean(LeHandle &handle, const std::vector<GeometryId> &a, const std::vector<GeometryId> &b, BooleanOp op,
                                const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new polygon-only Shape per input.
    ShapeOpResult shape_to_polygons(LeHandle &handle, const std::vector<GeometryId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                    const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new rect-only Shape per input, cut in `direction`.
    ShapeOpResult shape_to_rects(LeHandle &handle, const std::vector<GeometryId> &shapes, FractureDirection direction,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape per input, grown (positive) or shrunk (negative) by dx/dy dbu.
    ShapeOpResult shape_size(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t dx, int64_t dy,
                             const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new path-only Shape per input along its outline, `width` dbu wide.
    ShapeOpResult shape_outline_paths(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t width,
                                      const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief Moves each shape in place onto `layer`; all or nothing.
    std::expected<void, std::string> shape_change_layer(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer);

    /// @brief Removes the rect, polygon or path at `index` from `shape` (a
    /// Wire has paths only), with its mask if it has one; later pieces'
    /// indexes shift down by one.
    std::expected<void, std::string> remove_shape_piece(LeHandle &handle, GeometryId shape, PieceKind kind, size_t index);

    // --- Selection ---
    // Neither undoable nor a database change: selection is session state.

    /// @brief Selects every piece of a Shape, Wire, Route or PhysicalPort,
    /// or a whole Row, Placement or Region, adding to the selection. An
    /// error if it doesn't exist or has no geometry.
    std::expected<void, std::string> select(LeHandle &handle, SelectableObject object);
    /// @brief Adds one entry - a single piece, or a whole Row, Placement or
    /// Region - to the selection. An error if it doesn't exist.
    std::expected<void, std::string> select_entry(LeHandle &handle, const SelectedObject &entry);
    /// @brief Removes one entry from the selection, if it's there.
    void deselect_entry(LeHandle &handle, const SelectedObject &entry);
    void clear_selection(LeHandle &handle);

    // --- The design view ---

    /// @brief Opens `design`'s Abstract in the design view, closing any
    /// Layout, and makes it the current Abstract; the selection and rulers
    /// clear when the view changes. A Design with no Abstract opens nothing.
    std::expected<void, std::string> open_abstract(LeHandle &handle, DesignId design);
    /// @brief The same for `design`'s Layout.
    std::expected<void, std::string> open_layout(LeHandle &handle, DesignId design);
    /// @brief What the design view shows and where.
    ViewState view_state(const LeHandle &handle);
    /// @brief Fits the open view's boundary (a Layout's DIEAREA, an
    /// Abstract's declared size) into the viewport with `padding_px` around it.
    void fit_view(LeHandle &handle, int padding_px);
    /// @brief Fits `area` (dbu) into the viewport with `padding_px` around it.
    void fit_rect(LeHandle &handle, Rect area, int padding_px);
    /// @brief Zooms by `factor` (new scale = scale * (1 + factor)) about
    /// pixel (x, y), top-left origin; a factor <= -1 does nothing.
    void zoom(LeHandle &handle, double factor, int x_px, int y_px);
    /// @brief Pans by a fraction of the viewport's size, toward increasing
    /// dbu x/y.
    void pan(LeHandle &handle, double x_factor, double y_factor);
    void set_view_flip(LeHandle &handle, ViewFlip flip);

    // --- Rulers ---

    /// @brief Adds a finished ruler through `points` (dbu, at least 2).
    std::expected<void, std::string> add_ruler(LeHandle &handle, std::span<const Point> points);
    /// @brief Finishes the ruler being drawn, if any.
    void finish_ruler(LeHandle &handle);
    void clear_rulers(LeHandle &handle);

    // --- Files ---
    // Each logs what it reports, as the C API always has (the readers and
    // writers log as they go), and returns the error text too: the reader's
    // or writer's ERROR lines, or its own reason. A read records into the
    // open undo step, if any.

    /// @brief Reads a LEF file, its MACROs into the Library named `library`
    /// (created if missing).
    std::expected<void, std::string> read_lef(LeHandle &handle, const std::string &path, const std::string &library);
    /// @brief Reads a DEF file, its DESIGN into the Library named `library`.
    std::expected<void, std::string> read_def(LeHandle &handle, const std::string &path, const std::string &library);
    /// @brief Reads SystemVerilog/Verilog files into the Library named
    /// `library`: elaborated as a netlist, or syntax-only RTL.
    std::expected<void, std::string> read_verilog(LeHandle &handle, std::span<const std::string> files, bool netlist,
                                                  const std::string &library);
    /// @brief Loads a native database file into an empty database.
    std::expected<void, std::string> read_db(LeHandle &handle, const std::string &path, bool with_session);
    /// @brief Writes a LEF file: `abstracts` if any, else every Abstract in
    /// `library` if valid, else the current Abstract (none of them for
    /// TECHNOLOGY_ONLY).
    std::expected<void, std::string> write_lef(LeHandle &handle, const std::string &path, std::span<const AbstractId> abstracts,
                                               LibraryId library, LefLayerWriteMode layers);
    /// @brief Writes a DEF file for `layout`, or the current Layout if invalid.
    std::expected<void, std::string> write_def(LeHandle &handle, const std::string &path, LayoutId layout);
    /// @brief Saves the whole database (and, with `with_session`, the
    /// session) to a native database file.
    std::expected<void, std::string> write_db(LeHandle &handle, const std::string &path, bool with_session);
    /// @brief Writes stub Verilog modules for `library`'s Abstract-only
    /// Designs; an invalid `library` means the only Library.
    std::expected<void, std::string> write_verilog_stubs(LeHandle &handle, const std::string &path, LibraryId library);
}
