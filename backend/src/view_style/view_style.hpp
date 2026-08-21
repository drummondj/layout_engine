#pragma once
#include "../database/database.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace le
{
    /// @brief What a ViewLayer represents: which kind of object drew the
    /// shapes on it. Purely an application/rendering concept, not a LEF
    /// vocabulary term - a closed enum we own, not an open string field.
    enum class ViewLayerPurpose
    {
        TERMINAL,
        OBSTRUCTION,
        BOUNDARY,
    };

    struct Color
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;
    };

    /// @brief The fill treatment Renderer::draw_group draws a ViewLayer's
    /// shapes with, on top of/instead of a flat color wash - see its own
    /// doc comment for how each one is actually drawn (tiled shader vs.
    /// drawn directly per-shape). Always drawn with a transparent
    /// background so ViewLayers below remain visible through the gaps.
    enum class FillPattern
    {
        NONE,                // flat translucent fill (BOUNDARY today)
        DIAGONAL_STRIPES_NE, // ROUTING, horizontal direction: "/" hatch
        DIAGONAL_STRIPES_NW, // ROUTING, vertical direction: "\" hatch
        CROSS,               // CUT: an X spanning each shape's own bounds
        BRICK,               // any OBSTRUCTION, regardless of layer type
        DOTS,                // every other layer type
    };

    struct ViewLayerStyle
    {
        Color outline_color;
        Color fill_color;
        FillPattern fill_pattern = FillPattern::NONE;
    };

    struct ViewLayerTag
    {
    };
    using ViewLayerId = Id<ViewLayerTag>;

    /// @brief A named, styled, purpose-tagged rendering layer. `layer` is
    /// invalid for the one special BOUNDARY ViewLayer, which isn't tied to
    /// any physical LEF Layer.
    struct ViewLayerData
    {
        std::string name;       // e.g. "M1/TERMINAL", "BOUNDARY"
        std::string layer_name; // e.g. "M1", "BOUNDARY" - Scene::is_view_layer_visible's grouping key (its ViewLayerRow's own name)
        ViewLayerPurpose purpose;
        LayerId layer;
        ViewLayerStyle style;
    };

    /// @brief One purpose-column of a ViewLayerRow.
    struct ViewLayerColumn
    {
        ViewLayerPurpose purpose;
        ViewLayerId id;
    };

    /// @brief One row of a layer visibility/selectability widget ("one row
    /// per layer, columns per purpose" - see UPDATES.md 2.2): a name plus
    /// every purpose-column that belongs to it. Deliberately not keyed by
    /// LayerId - BOUNDARY is a row with a single column and no physical
    /// Layer behind it, and any future non-Technology-derived ("extra")
    /// ViewLayer becomes a row the same way, so a caller enumerating
    /// ViewLayerSet::rows() never needs to know or care whether a given
    /// row came from a physical Layer.
    struct ViewLayerRow
    {
        std::string name;
        std::vector<ViewLayerColumn> columns;
    };

    /// @brief The set of ViewLayers for a Technology: a TERMINAL and an
    /// OBSTRUCTION ViewLayer per physical Layer, plus one BOUNDARY
    /// ViewLayer not tied to any physical Layer. Shared/global for a
    /// Technology (not per-Scene) - which ViewLayers are toggled off is a
    /// per-Scene concern (Scene::set_layer_name_visible/set_purpose_visible),
    /// but the set of ViewLayers that exist and how they're styled is not.
    class ViewLayerSet
    {
    public:
        static ViewLayerSet build_for_technology(const Root &root, TechnologyId technology_id)
        {
            ViewLayerSet set;
            set.generation_ = next_generation();

            // Two independent palette cursors (see kRoutingCutColors/
            // kOtherColors below) plus the most recently assigned ROUTING
            // color, so a CUT layer can inherit it - see the CUT branch
            // below for why. One color per physical Layer, shared by every
            // purpose of that layer; purpose has no visual distinction yet
            // beyond that shared color.
            size_t routing_cut_index = 0;
            size_t other_index = 0;
            std::optional<Color> last_routing_color;

            for (LayerId layer_id : root.get_technology_layers(technology_id))
            {
                const LayerData *layer = root.get_layer(layer_id);

                Color color;
                if (layer->type == "ROUTING")
                {
                    color = routing_cut_color(routing_cut_index++);
                    last_routing_color = color;
                }
                else if (layer->type == "CUT" && last_routing_color)
                {
                    // LEF's LAYER declaration order is bottom-up physical
                    // stacking order (see CLAUDE.md's Pipeline note on
                    // ViewLayerId ordering) - so the most recent ROUTING
                    // color seen so far is the ROUTING layer directly
                    // below this CUT, i.e. exactly the "CUT layer above a
                    // ROUTING layer" this color-sharing rule is about.
                    color = *last_routing_color;
                }
                else if (layer->type == "CUT")
                {
                    // No ROUTING layer below (e.g. this CUT is declared
                    // before any ROUTING layer) - still a CUT, so it still
                    // draws from the bright/high-contrast palette, just
                    // its own next slot rather than a borrowed color.
                    color = routing_cut_color(routing_cut_index++);
                }
                else
                {
                    color = other_color(other_index++);
                }

                // TERMINAL and OBSTRUCTION of the same physical Layer
                // share the same color (see the palette logic above) but
                // not the same FillPattern - OBSTRUCTION always reads as
                // BRICK regardless of layer type, so blockage regions are
                // recognizable at a glance across every layer.
                const ViewLayerId terminal_id = set.add(layer->name, layer->name + "/TERMINAL", ViewLayerPurpose::TERMINAL, layer_id, layer_style(color, terminal_fill_pattern(*layer)));
                const ViewLayerId obstruction_id = set.add(layer->name, layer->name + "/OBSTRUCTION", ViewLayerPurpose::OBSTRUCTION, layer_id, layer_style(color, FillPattern::BRICK));

                set.rows_.push_back(ViewLayerRow{
                    .name = layer->name,
                    .columns = {
                        ViewLayerColumn{.purpose = ViewLayerPurpose::TERMINAL, .id = terminal_id},
                        ViewLayerColumn{.purpose = ViewLayerPurpose::OBSTRUCTION, .id = obstruction_id},
                    },
                });
            }

            set.boundary_id_ = set.add("BOUNDARY", "BOUNDARY", ViewLayerPurpose::BOUNDARY, LayerId{}, boundary_style());
            set.rows_.push_back(ViewLayerRow{
                .name = "BOUNDARY",
                .columns = {ViewLayerColumn{.purpose = ViewLayerPurpose::BOUNDARY, .id = set.boundary_id_}},
            });

            return set;
        }

        /// @brief Resolve a physical Layer + purpose to its ViewLayerId.
        /// Invalid if no such ViewLayer was registered (e.g. `layer` itself
        /// is invalid/unknown - callers don't need to check that first).
        ViewLayerId find(LayerId layer, ViewLayerPurpose purpose) const
        {
            for (const auto &entry : lookup_)
                if (entry.layer == layer && entry.purpose == purpose)
                    return entry.id;
            return ViewLayerId{};
        }

        ViewLayerId boundary_view_layer() const { return boundary_id_; }

        /// @brief Identifies *which* built ViewLayerSet this is - distinct
        /// from every other one build_for_technology() has ever produced,
        /// even one built for the same TechnologyId (e.g. after a second
        /// LEF file adds a layer, changing every later ViewLayerId
        /// assignment - see le_read_lef's own doc comment on why it
        /// rebuilds on every call, not just the first). A fresh, empty
        /// (default-constructed) ViewLayerSet reads 0. Exists so a cache
        /// keyed on some other stable identity (e.g. AbstractId, which
        /// doesn't change across a rebuild) can still detect that the
        /// ViewLayerIds it previously resolved shapes against no longer
        /// mean the same thing - see Pipeline::generate_shapes's cache
        /// key, which learned this the hard way.
        uint64_t generation() const { return generation_; }

        const ViewLayerData *get(ViewLayerId id) const { return pool_.get(id); }

        std::vector<ViewLayerId> all() const { return pool_.ids(); }

        /// @brief Every row of a layer visibility/selectability widget, in
        /// declaration order (physical Layers in their LEF-declared
        /// bottom-up stacking order, then BOUNDARY last) - see
        /// ViewLayerRow's own comment for why this is the API a caller
        /// should enumerate rather than going through Root's Technology
        /// directly.
        const std::vector<ViewLayerRow> &rows() const { return rows_; }

        /// @brief Every distinct ViewLayerPurpose present across every row,
        /// in first-encountered order (rows() order - LEF declaration order
        /// then BOUNDARY last) with duplicates removed, e.g. {TERMINAL,
        /// OBSTRUCTION, BOUNDARY} for a typical Technology. The "columns"
        /// axis of a layer visibility/selectability widget - deliberately
        /// not scoped to any one row/layer, since Scene's own visibility/
        /// selectability model toggles a purpose across every layer at
        /// once (see Scene::set_purpose_visible), not per row.
        std::vector<ViewLayerPurpose> purposes() const
        {
            std::vector<ViewLayerPurpose> result;
            for (const auto &row : rows_)
                for (const auto &column : row.columns)
                    if (std::find(result.begin(), result.end(), column.purpose) == result.end())
                        result.push_back(column.purpose);
            return result;
        }

    private:
        struct LookupEntry
        {
            LayerId layer;
            ViewLayerPurpose purpose;
            ViewLayerId id;
        };

        // Process-wide, monotonically increasing - not per-Root/per-handle,
        // since ViewLayerSet has no back-reference to either. Only the
        // relative distinctness of two generation() values is ever relied
        // on (as a cache-key component), never a specific number or a
        // per-handle sequence, so sharing this counter across every
        // LeHandle in the process is fine. Atomic since this project's
        // pipeline is meant to become multi-threaded (see CLAUDE.md/
        // README's Threading open design question) - a plain static
        // counter would race if build_for_technology is ever called from
        // more than one thread.
        static uint64_t next_generation()
        {
            static std::atomic<uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        ViewLayerId add(std::string layer_name, std::string name, ViewLayerPurpose purpose, LayerId layer, ViewLayerStyle style)
        {
            ViewLayerId id = pool_.create(ViewLayerData{
                .name = std::move(name),
                .layer_name = std::move(layer_name),
                .purpose = purpose,
                .layer = layer,
                .style = style,
            });
            lookup_.push_back(LookupEntry{.layer = layer, .purpose = purpose, .id = id});
            return id;
        }

        // Bright, high-contrast palette for ROUTING/CUT layers - the
        // layers users actually route/edit on, so they need to stand out.
        // Ported from the sibling project's `layer_generator_node.hpp`
        // (../../layout_engine/backend/pipeline/nodes/layer_generator_node.hpp),
        // which cycles through the same 30 colors one per physical Layer.
        static constexpr std::array<Color, 30> kRoutingCutColors = {{
            {255, 0, 0, 255},     // red
            {0, 255, 0, 255},     // green
            {100, 100, 255, 255}, // light blue
            {255, 255, 0, 255},   // yellow
            {255, 0, 255, 255},   // magenta
            {0, 255, 255, 255},   // cyan
            {128, 0, 0, 255},     // maroon
            {0, 128, 0, 255},     // dark green
            {0, 0, 128, 255},     // navy
            {128, 128, 0, 255},   // olive
            {128, 0, 128, 255},   // purple
            {0, 128, 128, 255},   // teal
            {192, 192, 192, 255}, // silver
            {128, 128, 128, 255}, // gray
            {255, 165, 0, 255},   // orange
            {210, 105, 30, 255},  // chocolate
            {139, 69, 19, 255},   // saddle brown
            {255, 20, 147, 255},  // deep pink
            {50, 205, 50, 255},   // lime green
            {72, 209, 204, 255},  // medium turquoise
            {123, 104, 238, 255}, // medium slate blue
            {255, 215, 0, 255},   // gold
            {160, 82, 45, 255},   // sienna
            {32, 178, 170, 255},  // light sea green
            {218, 112, 214, 255}, // orchid
            {95, 158, 160, 255},  // cadet blue
            {255, 99, 71, 255},   // tomato
            {60, 179, 113, 255},  // medium sea green
            {106, 90, 205, 255},  // slate blue
            {238, 130, 238, 255}, // violet
        }};

        // Muted palette for every other layer type (MASTERSLICE, IMPLANT,
        // OVERLAP, WELL, ...) - present for context but not something a
        // user routes/edits, so kept visually recessive relative to
        // kRoutingCutColors's bright hues rather than competing with them.
        static constexpr std::array<Color, 16> kOtherColors = {{
            {180, 180, 180, 255}, // light gray
            {200, 180, 160, 255}, // tan
            {170, 190, 170, 255}, // sage green
            {190, 170, 190, 255}, // mauve
            {160, 180, 200, 255}, // dusty blue
            {210, 200, 170, 255}, // khaki
            {180, 160, 160, 255}, // dusty rose
            {170, 170, 190, 255}, // lavender gray
            {190, 190, 150, 255}, // olive gray
            {160, 190, 190, 255}, // teal gray
            {200, 170, 180, 255}, // pink gray
            {170, 180, 160, 255}, // moss gray
            {190, 180, 200, 255}, // lilac gray
            {180, 170, 150, 255}, // taupe
            {160, 170, 180, 255}, // slate gray
            {200, 190, 190, 255}, // rosy gray
        }};

        // Both wrap (modulo), not clamp or overflow, once there are more
        // Layers than palette entries - unlike the sibling's own indexing
        // (`default_colors[++color_idx]`), which has no bounds check and
        // reads out of range past 30 layers.
        static Color routing_cut_color(size_t index)
        {
            return kRoutingCutColors[index % kRoutingCutColors.size()];
        }

        static Color other_color(size_t index)
        {
            return kOtherColors[index % kOtherColors.size()];
        }

        // TERMINAL's FillPattern: bright-palette layer types (ROUTING,
        // CUT) get a distinctive pattern of their own; everything else
        // gets the generic muted-palette DOTS.
        static FillPattern terminal_fill_pattern(const LayerData &layer)
        {
            if (layer.type == "ROUTING")
                return layer.direction == RoutingDirection::V ? FillPattern::DIAGONAL_STRIPES_NW : FillPattern::DIAGONAL_STRIPES_NE;

            if (layer.type == "CUT")
                return FillPattern::CROSS;

            return FillPattern::DOTS;
        }

        static ViewLayerStyle layer_style(Color base, FillPattern pattern = FillPattern::NONE)
        {
            Color fill = base;
            fill.a = 100;
            return ViewLayerStyle{.outline_color = base, .fill_color = fill, .fill_pattern = pattern};
        }

        static ViewLayerStyle boundary_style()
        {
            return ViewLayerStyle{.outline_color = {255, 255, 255, 255}, .fill_color = {0, 0, 0, 0}};
        }

        Pool<ViewLayerData, ViewLayerId> pool_;
        std::vector<LookupEntry> lookup_;
        ViewLayerId boundary_id_;
        std::vector<ViewLayerRow> rows_;
        uint64_t generation_ = 0;
    };
}
