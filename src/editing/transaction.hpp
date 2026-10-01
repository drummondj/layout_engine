#pragma once
#include "command.hpp"
#include "id_cell.hpp"
#include "../database/database.hpp"
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace le::editing
{
    /// @brief One ordered group of ICommand steps recorded together and
    /// undone/redone as a single unit (UPDATES.md item 21) - either every
    /// Root mutation a single typed Tcl command made (see
    /// CommandHistory::begin/end and le_repl_eval), or every ShapeId a
    /// single Move gesture translated. `label` is the typed command text
    /// (or a synthesized label like "move") - what backs the migrated
    /// Terminal command-recall list.
    ///
    /// Known limitation: id_cell_for() only resolves a reference to an id
    /// created/recreated *earlier in this same Transaction* - see
    /// IdCell's own comment for why an id can change across an undo/redo
    /// round trip at all. A reference held by some *other* object outside
    /// this Transaction (or from an earlier, already-committed one) is
    /// not retroactively repointed if that id is later deleted, undone,
    /// and redone - the same class of staleness Scene already documents
    /// and tolerates from ordinary pool-slot reuse (see scene.hpp), not a
    /// new failure mode this introduces. No back-patching mechanism is
    /// built for it.
    class Transaction
    {
    public:
        explicit Transaction(std::string label) : label_(std::move(label)) {}

        const std::string &label() const { return label_; }
        bool empty() const { return steps_.empty(); }

        template <typename IdT, typename DataT>
        void record_create(IdT id, DataT data,
                            std::function<IdT(Root &, const DataT &)> create_fn,
                            std::function<bool(Root &, IdT)> delete_fn)
        {
            steps_.push_back(std::make_unique<CreateCommand<IdT, DataT>>(
                id_cell_for<IdT>(id), std::move(data), std::move(create_fn), std::move(delete_fn)));
        }

        template <typename IdT, typename DataT>
        void record_update(IdT id, DataT before, DataT after,
                            std::function<bool(Root &, IdT, const DataT &)> apply_fn)
        {
            steps_.push_back(std::make_unique<UpdateCommand<IdT, DataT>>(
                id_cell_for<IdT>(id), std::move(before), std::move(after), std::move(apply_fn)));
        }

        template <typename IdT, typename DataT>
        void record_delete(IdT id, DataT data,
                            std::function<IdT(Root &, const DataT &)> create_fn,
                            std::function<bool(Root &, IdT)> delete_fn)
        {
            steps_.push_back(std::make_unique<DeleteCommand<IdT, DataT>>(
                id_cell_for<IdT>(id), std::move(data), std::move(create_fn), std::move(delete_fn)));
        }

        /// @brief The shared IdCell<IdT> tracking `id` within *this*
        /// transaction - the same cell for every call with an id whose
        /// (index, generation) currently matches, keyed by (type, packed
        /// index/generation) rather than one map per IdT type, since a
        /// Transaction is shared across every one of the ~35 id types
        /// without enumerating them here. Lazily creates one (seeded to
        /// `id`) on first reference. Exposed publicly (not just used
        /// internally by record_create/_update/_delete) so a hand-written
        /// call site can look up an *other* object's cell to build a
        /// create_fn that reads a live (possibly just-recreated) parent
        /// id instead of a stale one captured in its own data snapshot -
        /// see the cascading-delete call sites in api.cpp.
        template <typename IdT>
        IdCellPtr<IdT> id_cell_for(IdT id)
        {
            auto &type_map = live_cells_[std::type_index(typeid(IdT))];
            const uint64_t key = (static_cast<uint64_t>(id.generation) << 32) | id.index;
            auto it = type_map.find(key);
            if (it != type_map.end())
                return std::static_pointer_cast<IdCell<IdT>>(it->second);

            auto cell = std::make_shared<IdCell<IdT>>(IdCell<IdT>{id});
            type_map.emplace(key, cell);
            return cell;
        }

        void undo_all(Root &root)
        {
            for (auto it = steps_.rbegin(); it != steps_.rend(); ++it)
                (*it)->undo(root);
        }

        void redo_all(Root &root)
        {
            for (auto &step : steps_)
                step->redo(root);
        }

    private:
        std::string label_;
        std::vector<std::unique_ptr<ICommand>> steps_;
        std::unordered_map<std::type_index, std::unordered_map<uint64_t, std::shared_ptr<void>>> live_cells_;
    };
}
