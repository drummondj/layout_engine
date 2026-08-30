#pragma once
#include "../../database/database.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkMatrix.h"
#include "include/core/SkPicture.h"
#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace le
{
    /// @brief HierarchyResolver's own graph-node identity (backend/
    /// ONETBB_INTEGRATION.md follow-up - see hierarchy_resolver.hpp's own
    /// class doc comment for the full node-key scheme rationale). Two
    /// kinds, collapsing DesignId away (its own dispatch -
    /// resolve_design_target, core/placement_geometry.hpp - is a cheap,
    /// non-recursive decision, not something that needs its own graph
    /// node):
    ///
    /// Kind::Layout mirrors the old layout_pictures_ map's own
    /// {LayoutId, int} key exactly - a Layout's own resolved content DOES
    /// depend on remaining_depth (how deep its own placements may still
    /// recurse). Kind::Abstract is keyed on AbstractId alone, NOT
    /// {DesignId, depth} like the old design_pictures_ map's own fallback
    /// case - an Abstract's own content never depends on remaining_depth,
    /// so two different DesignIds at different depths that both resolve
    /// to the same Abstract now correctly share one node instead of
    /// recomputing twice (a deliberate, safe improvement, not an
    /// incidental behavior change - no existing test depends on the old
    /// double-recompute).
    struct NodeKey
    {
        enum class Kind
        {
            Abstract,
            Layout
        } kind = Kind::Abstract;
        AbstractId abstract_id; // Kind::Abstract only
        LayoutId layout_id;     // Kind::Layout only
        int remaining_depth = 0; // Kind::Layout only

        friend auto operator<=>(const NodeKey &, const NodeKey &) = default;
        friend bool operator==(const NodeKey &, const NodeKey &) = default;
    };

    /// @brief One fan-in slot's worth of a Layout node's own resolved
    /// placement content - a child's freshly-tagged {slot, transform,
    /// picture}, gathered via FanInCollectStage and sorted by slot before
    /// HierarchyLayoutNodeStage::compute() builds its own `instances`
    /// vector (hierarchy_resolver.hpp's own fan-in wiring section) -
    /// purely for byte-identical-picture determinism across repeated
    /// requests, not correctness; nothing in this codebase treats paint
    /// order among sibling placements as meaningful.
    struct ResolvedInstanceSlot
    {
        std::size_t slot = 0;
        SkMatrix transform;
        sk_sp<SkPicture> picture; // empty/null if unresolved - skipped, not drawn
    };

    /// @brief One discovered placement edge from a Layout node's own
    /// discovery pass (hierarchy_resolver.hpp's own discover_layout_children)
    /// into a child NodeKey's own (already constructed, possibly not yet
    /// computed) graph node - what HierarchyLayoutNodeStage::wire_fan_in
    /// wires into its own fan-in.
    struct DiscoveredEdge
    {
        NodeKey child_key;
        class HierarchyNodeBase *child_node = nullptr; // non-owning - graph_->nodes owns it
        std::size_t slot = 0;
        SkMatrix transform;
    };

    /// @brief Common base for every HierarchyResolver graph node
    /// (HierarchyAbstractLeafStage/HierarchyLayoutNodeStage) - lets
    /// HierarchyResolver's own ensure_node_built/touch_children/
    /// sweep_stale_nodes machinery operate uniformly over graph_->nodes
    /// without needing to know which concrete MemoizingStage<InputData, ...>
    /// specialization a given NodeKey resolved to (each has a different
    /// InputData, hence a different node() type - the one thing every
    /// node shares is its OutputData, sk_sp<SkPicture>, which
    /// picture_sender() exposes as a type-erased TBB sender for wiring a
    /// parent's own per-edge adapter, regardless of the child's concrete
    /// kind).
    class HierarchyNodeBase
    {
    public:
        virtual ~HierarchyNodeBase() = default;

        /// This node's own last-computed picture - stashed as a member
        /// inside compute() rather than exposed via a separate sink node
        /// (deliberate deviation from SynchronousStageRunner's sink
        /// pattern - that pattern exists because SynchronousStageRunner
        /// wires an *existing* stage class from outside; here we own the
        /// node classes, so exposing the result directly is simpler and
        /// is one fewer node per key).
        virtual sk_sp<SkPicture> last_picture() const = 0;

        /// Whether this node's own underlying function_node has already
        /// fired at least once (set true at the end of compute(), true
        /// for the rest of this node's lifetime thereafter). Load-bearing
        /// for HierarchyLayoutNodeStage::wire_fan_in: a TBB function_node
        /// does NOT replay its last output to a successor edge added via
        /// make_edge *after* it already fired - a child node reused
        /// (cache hit) from an earlier, already-fully-settled top-level
        /// call is exactly that case, so wire_fan_in must feed such a
        /// child's already-known last_picture() directly instead of
        /// wiring graph propagation for it. A node discovered fresh this
        /// same call is always still false at wire time (nothing
        /// computes during the single-threaded discovery pass - only
        /// HierarchyResolver::run_pending's own wait_for_all(), called
        /// once at the very end, ever advances this), so the normal
        /// make_edge-based wiring is correct for it.
        bool computed = false;

        /// The common TBB sender every child node broadcasts its own
        /// picture through - a parent Layout node's own per-edge adapter
        /// wires from this, regardless of the concrete node kind.
        virtual oneapi::tbb::flow::sender<StageData<sk_sp<SkPicture>, PipelineOptions>> &picture_sender() = 0;

        /// Explicitly seeds this node's own computation for the "pure
        /// source" case - a leaf (always) or a Layout node with zero
        /// edges (no resolvable placements) - neither ever receives an
        /// upstream try_put naturally, so HierarchyResolver::run_pending
        /// calls this directly once discovery for the current call
        /// finishes, before a single wait_for_all() settles everything
        /// discovered so far.
        virtual void trigger(const PipelineOptions &options) = 0;

        /// Unwires every edge THIS node itself registered as part of its
        /// own incoming fan-in - called on every node in a stale sweep's
        /// batch before any of them are destroyed
        /// (HierarchyResolver::sweep_stale_nodes's two-pass
        /// unwire-then-destroy sweep). No-op for a leaf
        /// (HierarchyAbstractLeafStage has no incoming edges at all).
        virtual void unwire_incoming() {}

        /// Generation stamping for reachability-based pruning - see
        /// HierarchyResolver::touch_children/sweep_stale_nodes. A live
        /// node's children are always also live (the touch-propagation
        /// invariant sweep_stale_nodes's correctness rests on) - stamped
        /// on both the cache-hit and cache-miss paths of
        /// HierarchyResolver::ensure_node_built.
        uint64_t last_touched_generation = 0;

        /// This node's own deduped child NodeKeys (Layout nodes only,
        /// left empty for a leaf) - what touch_children walks. Kept
        /// separate from a Layout node's own incoming_edges_ (which
        /// tracks one entry per placement/edge, not deduped) since the
        /// two answer different questions: "what do I draw" (incoming_edges_)
        /// vs. "what depends on me for liveness" (children).
        std::vector<NodeKey> children;
    };
}
