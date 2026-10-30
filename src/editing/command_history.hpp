#pragma once
#include "transaction.hpp"
#include "../database/database.hpp"
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace le::editing
{
    /// @brief What one undo() or redo() did.
    struct ReplayResult
    {
        bool replayed = false;    // false: nothing to undo/redo
        size_t steps = 0;         // in the replayed transaction
        size_t failed_steps = 0;  // of those, couldn't be applied
        std::string label;        // the replayed transaction's
    };

    /// @brief Per-handle undo/redo stack plus the command-recall log, over
    /// one Root. One LeHandle owns exactly one of these.
    ///
    /// Both are bounded: the undo stack keeps the newest undo_limit()
    /// transactions (each holds full before/after copies of what it
    /// changed), the recall log the newest kRecallLimit lines. A
    /// transaction that leaves the history (trimmed, the redo stack
    /// cleared by a new edit, or clear_undo_redo()) releases the ids it
    /// holds retired, so their slots can be reused.
    ///
    /// begin()/end() bracket a single recording transaction - every
    /// generated create/update/delete edit op and the hand-written edits
    /// check current() and record themselves into it if active. No nesting: a begin() while already recording is a no-op,
    /// matching le_repl_eval's own "one begin/end per top-level typed
    /// command" usage - there is no reentrant call path that needs more.
    class CommandHistory
    {
    public:
        static constexpr size_t kDefaultUndoLimit = 200;
        static constexpr size_t kRecallLimit = 10'000;

        explicit CommandHistory(Root &root) : root_(root) {}

        /// @brief How many transactions undo can step back through.
        size_t undo_limit() const { return undo_limit_; }

        /// @brief Sets undo_limit(), dropping the oldest transactions past
        /// it now. 0 keeps none.
        void set_undo_limit(size_t limit)
        {
            undo_limit_ = limit;
            trim_undo_stack();
        }

        void begin(std::string label)
        {
            if (recording_)
                return;
            recording_.emplace(std::move(label));
        }

        /// @brief Ends the current recording transaction (a no-op if
        /// none is active). If it recorded at least one step, pushes it
        /// onto the undo stack (clearing the redo stack - a fresh edit
        /// invalidates old redo history, standard undo/redo convention)
        /// regardless of `succeeded`. The transaction's own label is
        /// always appended to the recall log too, `succeeded` or not -
        /// a failed command is exactly
        /// the one a user most wants back, to recall and edit into a
        /// working one, rather than retyping it from scratch. `succeeded`
        /// is still accepted (not collapsed into a no-op parameter) since
        /// callers - le_repl_eval, Move - already compute it for their
        /// own error-message/undo-step-empty reasons regardless.
        void end(bool succeeded)
        {
            if (!recording_)
                return;

            Transaction txn = std::move(*recording_);
            recording_.reset();
            const std::string label = txn.label();

            if (!txn.empty())
            {
                undo_stack_.push_back(std::move(txn));
                release_all(redo_stack_);
                trim_undo_stack();
            }

            command_text_log_.push_back(label);
            if (command_text_log_.size() > kRecallLimit)
                command_text_log_.pop_front();
        }

        bool is_recording() const { return recording_.has_value(); }
        Transaction *current() { return recording_ ? &*recording_ : nullptr; }

        /// @brief Undo the most recent transaction, moving it to the redo
        /// stack even if some of its steps fail.
        ReplayResult undo() { return replay(undo_stack_, redo_stack_, &Transaction::undo_all); }

        /// @brief Redo the most recently undone transaction, moving it to
        /// the undo stack even if some of its steps fail.
        ReplayResult redo() { return replay(redo_stack_, undo_stack_, &Transaction::redo_all); }

        bool can_undo() const { return !undo_stack_.empty(); }
        bool can_redo() const { return !redo_stack_.empty(); }

        /// @brief Forget every undo/redo step (the recall log is kept) -
        /// for when the database is replaced wholesale (le_read_db), since
        /// the steps' object Ids would refer to the new objects.
        void clear_undo_redo()
        {
            release_all(undo_stack_);
            release_all(redo_stack_);
        }

        // --- Command recall (migrated from Terminal._commandHistory) ---
        size_t recall_count() const { return command_text_log_.size(); }

        const std::string &recall_at(size_t index) const { return command_text_log_.at(index); }

    private:
        ReplayResult replay(std::deque<Transaction> &from, std::deque<Transaction> &to, size_t (Transaction::*apply)(Root &))
        {
            if (from.empty())
                return {};
            Transaction txn = std::move(from.back());
            from.pop_back();
            ReplayResult result{.replayed = true, .steps = txn.size(), .failed_steps = (txn.*apply)(root_), .label = txn.label()};
            // Replayed steps call Root directly, which leaves bumping the
            // mutation version to its caller; render caches key on it.
            root_.bump_mutation_version();
            to.push_back(std::move(txn));
            return result;
        }

        void release_all(std::deque<Transaction> &stack)
        {
            for (Transaction &txn : stack)
                txn.release(root_);
            stack.clear();
        }

        // Oldest first: undo can no longer reach them.
        void trim_undo_stack()
        {
            while (undo_stack_.size() > undo_limit_)
            {
                undo_stack_.front().release(root_);
                undo_stack_.pop_front();
            }
        }

        Root &root_;
        size_t undo_limit_ = kDefaultUndoLimit;
        std::optional<Transaction> recording_;
        std::deque<Transaction> undo_stack_;
        std::deque<Transaction> redo_stack_;
        std::deque<std::string> command_text_log_;
    };
}
