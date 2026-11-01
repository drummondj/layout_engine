#pragma once
#include "command.hpp"
#include "../database/database.hpp"
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace le::editing
{
    /// @brief One ordered group of ICommand steps recorded together and
    /// undone/redone as a single unit - every Root mutation a single typed
    /// Tcl command made (see CommandHistory::begin/end and le_repl_eval),
    /// or every edit one GUI gesture made. `label` is the typed command
    /// text (or a synthesized label like "move"), which backs the
    /// Terminal's command-recall list.
    class Transaction
    {
    public:
        explicit Transaction(std::string label) : label_(std::move(label)) {}

        const std::string &label() const { return label_; }
        bool empty() const { return steps_.empty(); }
        size_t size() const { return steps_.size(); }

        /// @brief Record that `id` was just created.
        template <typename IdT>
        void record_create(IdT id)
        {
            steps_.push_back(std::make_unique<CreateCommand<IdT>>(id));
        }

        template <typename IdT, typename DataT>
        void record_update(IdT id, DataT before, DataT after, std::function<bool(Root &, IdT, const DataT &)> apply_fn)
        {
            steps_.push_back(std::make_unique<UpdateCommand<IdT, DataT>>(id, std::move(before), std::move(after), std::move(apply_fn)));
        }

        /// @brief Record that `id` was just retired (Root::retire).
        template <typename IdT>
        void record_delete(IdT id, RetiredOf<IdT> retired)
        {
            steps_.push_back(std::make_unique<DeleteCommand<IdT>>(id, std::move(retired)));
        }

        /// @brief Record a Root::retire_batch result as one step.
        template <typename IdT>
        void record_batch_delete(RetiredBatchOf<IdT> retired)
        {
            steps_.push_back(std::make_unique<BatchDeleteCommand<IdT>>(std::move(retired)));
        }

        /// @brief Undo every step, last first. Returns how many failed.
        size_t undo_all(Root &root)
        {
            size_t failed = 0;
            for (auto it = steps_.rbegin(); it != steps_.rend(); ++it)
                failed += (*it)->undo(root) ? 0 : 1;
            return failed;
        }

        /// @brief Redo every step, first first. Returns how many failed.
        size_t redo_all(Root &root)
        {
            size_t failed = 0;
            for (auto &step : steps_)
                failed += step->redo(root) ? 0 : 1;
            return failed;
        }

        /// @brief The transaction is leaving the history: give back every
        /// id it holds retired.
        void release(Root &root)
        {
            for (auto &step : steps_)
                step->release(root);
        }

    private:
        std::string label_;
        std::vector<std::unique_ptr<ICommand>> steps_;
    };

    /// @brief Delete `id`: retired and recorded into `txn` if there is one
    /// (so undo restores it at the same id), else erased. False if `id`
    /// doesn't exist.
    template <typename IdT>
    bool delete_object(Root &root, Transaction *txn, IdT id)
    {
        if (!txn)
            return root.erase(id);
        auto retired = root.retire(id);
        if (!retired)
            return false;
        txn->record_delete(id, std::move(*retired));
        return true;
    }

    /// @brief delete_object for many objects of one class, linear in the
    /// objects and the child lists they leave; recorded as one step.
    /// Skips ids that don't exist or repeat. Returns how many were deleted.
    template <typename IdT>
    size_t delete_objects(Root &root, Transaction *txn, std::span<const IdT> ids)
    {
        if (!txn)
            return root.erase_batch(ids);
        auto retired = root.retire_batch(ids);
        const size_t deleted = retired.size();
        if (deleted)
            txn->record_batch_delete<IdT>(std::move(retired));
        return deleted;
    }
}
