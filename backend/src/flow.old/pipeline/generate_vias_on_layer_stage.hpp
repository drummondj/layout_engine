#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../view_style/view_style.hpp"
#include "via_shapes.hpp"
#include <string>
#include <tuple>
#include <vector>

namespace le::flow
{
    /// @brief A via/cut layer's own generate task - see this experiment's
    /// "via handling" note: a cut layer (e.g. VIA12) is a real physical
    /// `Layer` in its own right, exactly like M1/M2, so it gets its own
    /// task rather than folding via resolution into whichever routing
    /// layer happens to reference one. This is the *only* task that ever
    /// resolves a via named after this layer - `GenerateTerminalsOnLayerStage`/
    /// `GenerateObstructionsOnLayerStage` do no via resolution at all.
    ///
    /// Walks *both* Terminals' and Obstructions' Shapes (a via can be
    /// referenced from either), filtering each Shape's own `.vias`/
    /// `.via_iterates` down to just the entries whose `via_name` matches
    /// this layer's own name (a plain string already on the Shape - no
    /// resolution needed to check this) before resolving via
    /// `append_via_shapes` (via_shapes.hpp, ported unchanged from
    /// src/pipeline/stages/via_shapes.hpp) - whose own output naturally
    /// spans whichever physical layers this via's own `ViaLayer` entries
    /// are on (typically the cut layer itself plus the routing layer(s)
    /// immediately above/below), tagged with whichever purpose
    /// (TERMINAL/OBSTRUCTION) the referencing Shape had. No `origin`/
    /// `shape_id` set on the emitted RenderedShapes - matches
    /// append_via_shapes's own existing behavior (a via-shape isn't
    /// independently selectable; the Shape that referenced it already is).
    ///
    /// Known scope simplification: assumes `ShapeVia.via_name` matches a
    /// same-named cut `Layer` - the overwhelming common LEF convention
    /// (and what every fixture in this codebase already does, e.g.
    /// `VIA VIA12`), but not guaranteed by the LEF grammar itself. A via
    /// whose name doesn't match any cut layer's name is never generated
    /// under this scheme - a real, narrower gap than the actual
    /// `Pipeline`'s own via_shapes.hpp usage has, accepted for this
    /// experiment (see TASKFLOW_EXPERIMENT.md).
    ///
    /// Walks every Terminal/Obstruction in the Abstract itself, same as
    /// the other two per-layer stages - a `ShapesByLayerIndex`
    /// pre-bucketing pass (keyed by via_name instead of physical layer)
    /// was tried here too and reverted alongside them; see
    /// TASKFLOW_EXPERIMENT.md's "Pre-process step" entry for why.
    class GenerateViasOnLayerStage : public le::ShapeGenerationStage
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, AbstractId abstract_id, LayerId via_layer, const ViewLayerSet &view_layers)
        {
            return stage_.get(std::tuple{abstract_id, view_layers.generation(), root.mutation_version()}, [&]
            {
                std::vector<RenderedShape> shapes;

                const LayerData *via_layer_data = root.get_layer(via_layer);
                if (!via_layer_data)
                    return shapes;
                const std::string &via_name = via_layer_data->name;

                auto process = [&](const Shape *raw_shape, ViewLayerPurpose purpose)
                {
                    if (!raw_shape)
                        return;

                    Shape matching;
                    for (const ShapeVia &via : raw_shape->vias)
                        if (via.via_name == via_name)
                            matching.vias.push_back(via);
                    for (const ShapeViaIterate &via : raw_shape->via_iterates)
                        if (via.via_name == via_name)
                            matching.via_iterates.push_back(via);

                    if (matching.vias.empty() && matching.via_iterates.empty())
                        return;

                    append_via_shapes(root, matching, purpose, view_layers, LayoutId{}, shapes);
                };

                for (auto terminal_id : root.get_abstract_terminals(abstract_id))
                    for (auto port_id : root.get_terminal_ports(terminal_id))
                        for (const auto &shape_id : root.get_terminal_port_shapes(port_id))
                            process(root.get_shape(shape_id), ViewLayerPurpose::TERMINAL);

                for (auto obstruction_id : root.get_abstract_obstructions(abstract_id))
                    for (const auto &shape_id : root.get_obstruction_shapes(obstruction_id))
                        process(root.get_shape(shape_id), ViewLayerPurpose::OBSTRUCTION);

                return shapes;
            });
        }

        uint64_t version() const override { return stage_.version(); }
        uint64_t call_count() const override { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> stage_;
    };
}
