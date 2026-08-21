#pragma once
#include <cstdint>

namespace le
{
    /// @brief Remembers the last (key, value) pair produced by a single
    /// compute function; recomputes only when the key differs from last
    /// time, bumping its own monotonic output version whenever it does.
    /// Not a general reactive/dependency-graph primitive - just enough to
    /// give each stage class in `pipeline`/`render` its own cache slot
    /// without hand-written invalidation flags scattered through the
    /// class. Lives in `core` (UPDATES.md item 16) rather than `pipeline`
    /// because both `pipeline` and `render` build their own stage classes
    /// on top of it independently - neither module depends on the other
    /// for this piece.
    ///
    /// version() is the mechanism that replaced hand-copied cache-key
    /// triggers between stages: a downstream stage's own key includes
    /// *this* stage's version() instead of manually re-deriving everything
    /// this stage's own key depends on - so adding a new trigger to one
    /// stage (e.g. Root::mutation_version(), see pipeline's
    /// GenerateShapesStage) only requires updating that one stage's own
    /// key; every downstream stage that composes via .version() invalidates
    /// correctly for free, without needing to know the new trigger exists
    /// at all. This is the structural fix for the exact caching-bug class
    /// TCL_EXPLORATION.md describes: before version() existed, adding
    /// Root::mutation_version() as a new trigger required manually
    /// touching 9 separate hand-written cache keys across
    /// pipeline.hpp/render.hpp, because nothing connected a downstream
    /// stage's key to what its own upstream actually depended on.
    template <typename Key, typename Value>
    class VersionedStage
    {
    public:
        template <typename Fn>
        const Value &get(const Key &key, Fn &&compute)
        {
            if (!valid_ || key != last_key_)
            {
                value_ = compute();
                ++version_;
                last_key_ = key;
                valid_ = true;
                ++call_count_;
            }
            return value_;
        }

        /// This stage's own output version - bumped only when it actually
        /// recomputed. A downstream stage folds this into its own key
        /// instead of needing to know (or re-derive) everything this
        /// stage itself depends on.
        uint64_t version() const { return version_; }

        // Number of times compute() actually ran - exposed purely to make
        // cache hits/misses observable in tests.
        uint64_t call_count() const { return call_count_; }

    private:
        Key last_key_{};
        Value value_{};
        uint64_t version_ = 0;
        bool valid_ = false;
        uint64_t call_count_ = 0;
    };

    /// @brief Backward-compatible alias for VersionedStage's old name -
    /// render.hpp's own stages still use it unchanged. Safe: every
    /// existing CachedStage<Key, Value> use gets VersionedStage's strict
    /// superset behavior (the same get()/call_count(), plus an unused
    /// version() nothing in render.hpp calls yet) with zero source changes
    /// there. Remove once render's own follow-up switches it to
    /// VersionedStage directly and starts composing via version() too.
    template <typename Key, typename Value>
    using CachedStage = VersionedStage<Key, Value>;
}
