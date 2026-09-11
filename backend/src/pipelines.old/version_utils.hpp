#pragma once
#include <cstdint>

namespace le
{
    /// @brief Combines several trigger values into one uint64_t suitable
    /// for MemoizingStage's own single `data_version` field - the oneTBB
    /// migration's replacement for a `std::tuple{...}`-shaped
    /// core::VersionedStage key (see the migration plan's PipelineOptions
    /// design section). Needed because MemoizingStage::execute() only ever
    /// compares `data_version` by inequality - it never inspects `InputData`
    /// itself - so when InputData is an id (e.g. AbstractId) rather than
    /// already-resolved content, that id's own {index, generation} must be
    /// folded into data_version too, or two calls for two different ids
    /// with the same other triggers would collide on the same data_version
    /// and wrongly reuse the first id's cached result.
    ///
    /// Order-sensitive (boost::hash_combine's own mixing constant/shifts,
    /// restated locally rather than pulling in
    /// <boost/functional/hash.hpp> for one function), not commutative - two
    /// arguments swapped produce a different result, which matters when
    /// e.g. an id's `index` and `generation` are combined in a fixed order.
    inline uint64_t combine_versions(uint64_t seed, uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    template <typename... Rest>
    uint64_t combine_versions(uint64_t seed, uint64_t value, Rest... rest)
    {
        return combine_versions(combine_versions(seed, value), rest...);
    }
}
