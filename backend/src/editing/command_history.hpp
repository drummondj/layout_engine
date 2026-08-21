#pragma once
#include "transaction.hpp"
#include "../database/database.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace le::editing
{
    /// @brief Per-handle undo/redo stack plus the command-recall log
    /// (UPDATES.md item 21, migrated from the Flutter Terminal's own
    /// local `_commandHistory`). One LeHandle owns exactly one of these
    /// (see api.cpp's LeHandle struct).
    ///
    /// begin()/end() bracket a single recording transaction - every
    /// generated create_x/update_x call (see schema.py's create_api_body/
    /// update_api_body) and the 4 hand-written le_delete_* functions
    /// check is_recording()/current() and record themselves into it if
    /// active. No nesting: a begin() while already recording is a no-op,
    /// matching le_repl_eval's own "one begin/end per top-level typed
    /// command" usage - there is no reentrant call path that needs more.
    class CommandHistory
    {
    public:
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
        /// regardless of `succeeded`. If `succeeded` is true, the
        /// transaction's own label is also appended to the recall log -
        /// this is what makes command_history() only ever show commands
        /// that actually ran successfully, even ones (like a pure read)
        /// that recorded zero undoable steps.
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
                redo_stack_.clear();
            }

            if (succeeded)
                command_text_log_.push_back(label);
        }

        bool is_recording() const { return recording_.has_value(); }
        Transaction *current() { return recording_ ? &*recording_ : nullptr; }

        bool undo(Root &root)
        {
            if (undo_stack_.empty())
                return false;

            Transaction txn = std::move(undo_stack_.back());
            undo_stack_.pop_back();
            txn.undo_all(root);
            // Every ICommand replays straight against Root (create_x/
            // update_x/delete_x), none of which bump mutation_version()
            // themselves (the same "caller's job" convention every other
            // mutation path already follows - see create_api_body's own
            // comment) - without this, the render pipeline's cache (keyed
            // on Root::mutation_version(), see GenerateShapesStage et al.)
            // never notices anything changed, so the canvas stays stale
            // until something unrelated happens to bump it.
            root.bump_mutation_version();
            redo_stack_.push_back(std::move(txn));
            return true;
        }

        bool redo(Root &root)
        {
            if (redo_stack_.empty())
                return false;

            Transaction txn = std::move(redo_stack_.back());
            redo_stack_.pop_back();
            txn.redo_all(root);
            root.bump_mutation_version(); // see undo()'s own comment
            undo_stack_.push_back(std::move(txn));
            return true;
        }

        bool can_undo() const { return !undo_stack_.empty(); }
        bool can_redo() const { return !redo_stack_.empty(); }

        // --- Command recall (migrated from Terminal._commandHistory) ---
        size_t recall_count() const { return command_text_log_.size(); }

        const std::string &recall_at(size_t index) const { return command_text_log_.at(index); }

    private:
        std::optional<Transaction> recording_;
        std::vector<Transaction> undo_stack_;
        std::vector<Transaction> redo_stack_;
        std::vector<std::string> command_text_log_;
    };
}
