#pragma once
#include <memory>

namespace le::editing
{
    /// @brief A tiny mutable box around one IdT, always held via
    /// IdCellPtr<IdT> (a shared_ptr). Pool<T,IdT>::create() (see
    /// backend/src/database/generated/pool.hpp) always allocates a fresh
    /// index/generation - there is no "create at this specific id"
    /// operation - so undoing a delete or redoing a create never
    /// reproduces the original id. Every ICommand that creates/recreates
    /// an object writes the new id into its own IdCell; anything else in
    /// the same Transaction that references that object reads cell->id
    /// at *execution* time (not a value captured when the step was
    /// recorded), so it always sees whichever id is currently live - see
    /// Transaction::id_cell_for.
    template <typename IdT>
    struct IdCell
    {
        IdT id;
    };

    template <typename IdT>
    using IdCellPtr = std::shared_ptr<IdCell<IdT>>;
}
