#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "filter_by_viewport_and_size_stage.hpp"
#include "generate_obstructions_on_layer_stage.hpp"
#include "generate_terminals_on_layer_stage.hpp"
#include "generate_vias_on_layer_stage.hpp"
#include <deque>
#include <map>
#include <taskflow/taskflow.hpp>
#include <vector>

namespace le::flow
{
    /// @brief The per-layer DAG builder (TASKFLOW_EXPERIMENT.md's
    /// per-layer-granularity redesign): one `generate -> filter` task pair
    /// per physical layer per object type - two pairs (terminals,
    /// obstructions) for a routing layer, one pair for a cut/via layer
    /// (`GenerateViasOnLayerStage` - see that class's own comment for why
    /// a via/cut layer gets exactly one task rather than being folded into
    /// whichever routing layer references it) - plus one final
    /// `collect_shapes` task depending on every filter task. Every task is
    /// **static** (`taskflow.emplace([]{...})`), not dynamic/`Subflow`-
    /// based - the graph's shape is fully known from `view_layers.rows()`
    /// before `executor.run()` is ever called, so there's nothing dynamic
    /// tasking would buy here (and it sidesteps the whole class of
    /// Subflow-double-join bug the previous chunked-within-a-stage design
    /// hit).
    ///
    /// Owns six `std::map<LayerId, Stage>` - persistent across calls, one
    /// generate/filter pair of maps per (object type), so a routing
    /// layer's own terminal-filter and obstruction-filter stages never
    /// share a cache slot (their upstream generate stage's own `.version()`
    /// differs). Entries are created lazily via `operator[]` the first
    /// time a given `LayerId` is seen; one that stops appearing after a
    /// LEF reload just leaves a stale, harmless, never-evicted entry -
    /// matches this codebase's existing single-slot-cache-without-eviction
    /// pattern elsewhere (`VersionedStage` itself never evicts either).
    class GenerateAndFilterPipeline
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers, const Scene &scene, tf::Executor &executor)
        {
            tf::Taskflow taskflow;

            // Stable storage for the pointers each (generate, filter)
            // task pair passes its result through - std::deque, not
            // std::vector, since every task lambda below captures its own
            // slot by reference and more slots keep getting push_back-ed
            // as the row loop continues; std::deque never invalidates
            // existing elements' references on growth, unlike
            // std::vector, which would dangle every earlier task's
            // captured slot the moment a later push_back reallocates.
            std::deque<const std::vector<RenderedShape> *> generated_slots;
            std::deque<const std::vector<RenderedShape> *> filtered_slots;
            std::vector<tf::Task> filter_tasks;

            auto add_chain = [&](auto &generate_stage, FilterByViewportAndSizeStage &filter_stage, LayerId physical_layer, const char *generate_name, const char *filter_name)
            {
                generated_slots.push_back(nullptr);
                const std::vector<RenderedShape> *&generated_slot = generated_slots.back();
                filtered_slots.push_back(nullptr);
                const std::vector<RenderedShape> *&filtered_slot = filtered_slots.back();

                tf::Task generate_task = taskflow.emplace([&, physical_layer]
                                                            { generated_slot = &generate_stage.run(root, abstract_id, physical_layer, view_layers); })
                                              .name(generate_name);
                tf::Task filter_task = taskflow.emplace([&]
                                                          { filtered_slot = &filter_stage.run(generate_stage, *generated_slot, scene); })
                                            .name(filter_name);
                generate_task.precede(filter_task);
                filter_tasks.push_back(filter_task);
            };

            for (const auto &row : view_layers.rows())
            {
                ViewLayerId terminal_view_layer_id{};
                for (const auto &column : row.columns)
                    if (column.purpose == ViewLayerPurpose::TERMINAL)
                        terminal_view_layer_id = column.id;

                if (!terminal_view_layer_id.valid())
                    continue; // pseudo-row (BOUNDARY/ROW/GCELLGRID/PLACEMENT_BLOCKAGE/REGION) - no physical layer behind it

                const ViewLayerData *terminal_view_layer = view_layers.get(terminal_view_layer_id);
                if (!terminal_view_layer)
                    continue;
                const LayerId physical_layer = terminal_view_layer->layer;

                const LayerData *layer_data = root.get_layer(physical_layer);
                if (!layer_data)
                    continue;

                if (layer_data->type == "CUT")
                {
                    add_chain(via_generate_stages_[physical_layer], via_filter_stages_[physical_layer], physical_layer,
                               "generate_vias_on_layer", "filter_vias_on_layer");
                }
                else
                {
                    add_chain(terminal_generate_stages_[physical_layer], terminal_filter_stages_[physical_layer], physical_layer,
                               "generate_terminals_on_layer", "filter_terminals_on_layer");
                    add_chain(obstruction_generate_stages_[physical_layer], obstruction_filter_stages_[physical_layer], physical_layer,
                               "generate_obstructions_on_layer", "filter_obstructions_on_layer");
                }
            }

            tf::Task collect_task = taskflow.emplace([&]
                                                       {
                collected_.clear();
                for (const auto *filtered : filtered_slots)
                    if (filtered)
                        collected_.insert(collected_.end(), filtered->begin(), filtered->end());

                if (const Shape *boundary_shape = root.get_shape(root.get_abstract_boundary(abstract_id)))
                {
                    collected_.push_back(RenderedShape{
                        .shape = *boundary_shape,
                        .view_layer = view_layers.boundary_view_layer(),
                    });
                } })
                                         .name("collect_shapes");

            for (tf::Task &filter_task : filter_tasks)
                filter_task.precede(collect_task);

            executor.run(taskflow).wait();
            return collected_;
        }

        // Aggregated across every per-layer stage instance - exposed
        // purely to make cache hits/misses observable in tests (there's
        // no longer a single shared generate/filter stage the way the
        // earlier design had one - see this class's own comment).
        uint64_t total_generate_calls() const
        {
            uint64_t total = 0;
            for (const auto &[layer, stage] : terminal_generate_stages_)
                total += stage.call_count();
            for (const auto &[layer, stage] : obstruction_generate_stages_)
                total += stage.call_count();
            for (const auto &[layer, stage] : via_generate_stages_)
                total += stage.call_count();
            return total;
        }

        uint64_t total_filter_calls() const
        {
            uint64_t total = 0;
            for (const auto &[layer, stage] : terminal_filter_stages_)
                total += stage.call_count();
            for (const auto &[layer, stage] : obstruction_filter_stages_)
                total += stage.call_count();
            for (const auto &[layer, stage] : via_filter_stages_)
                total += stage.call_count();
            return total;
        }

    private:
        std::map<LayerId, GenerateTerminalsOnLayerStage> terminal_generate_stages_;
        std::map<LayerId, FilterByViewportAndSizeStage> terminal_filter_stages_;
        std::map<LayerId, GenerateObstructionsOnLayerStage> obstruction_generate_stages_;
        std::map<LayerId, FilterByViewportAndSizeStage> obstruction_filter_stages_;
        std::map<LayerId, GenerateViasOnLayerStage> via_generate_stages_;
        std::map<LayerId, FilterByViewportAndSizeStage> via_filter_stages_;
        std::vector<RenderedShape> collected_;
    };
}
