#pragma once
#include "../database/database.hpp"
#include "../editing/transaction.hpp"
#include "../io/read_rollback.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace le::api_detail
{
    /// @brief One file read as an undo step. Undo retires every object
    /// the read created (Root::retire_created), clears the references it
    /// set on older objects and puts the Technologies back as they were
    /// before; redo reverses each. Done, the step holds only the
    /// creation journal (12 bytes per object); undone, it holds the
    /// retired objects themselves, which nothing else does then.
    class ReadCommand : public le::editing::ICommand
    {
    public:
        explicit ReadCommand(ReadRecord record) : record_(std::move(record)) {}

        bool undo(Root &root) override
        {
            if (retired_)
                return false;
            bool ok = true;
            for (const auto &[id, design] : record_.based)
                ok &= root.update_design(id, LibraryId{}, std::optional<DesignId>(DesignId{}), std::nullopt);
            for (const auto &[id, design] : record_.linked)
                ok &= root.update_instance(id, SchematicId{}, std::optional<DesignId>(DesignId{}), std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt, std::nullopt);
            retired_ = root.retire_created(record_.created);
            swap_technologies(root);
            return ok;
        }

        bool redo(Root &root) override
        {
            if (!retired_)
                return false;
            swap_technologies(root);
            bool ok = root.restore_created(*retired_);
            retired_.reset();
            for (const auto &[id, design] : record_.linked)
                ok &= root.update_instance(id, SchematicId{}, std::optional<DesignId>(design), std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt, std::nullopt);
            for (const auto &[id, design] : record_.based)
                ok &= root.update_design(id, LibraryId{}, std::optional<DesignId>(design), std::nullopt);
            return ok;
        }

        void release(Root &root) override
        {
            if (retired_)
                root.release_created(*retired_);
        }

    private:
        // Technologies are edited in place, not created: each undo and
        // redo trades the live data with the other side's.
        void swap_technologies(Root &root)
        {
            for (auto &[id, data] : record_.technologies)
                if (TechnologyData *technology = root.get_technology(id))
                {
                    std::swap(*technology, data);
                    root.note_technology_changed(id);
                }
        }

        ReadRecord record_;
        std::optional<RetiredCreated> retired_; // set while undone
    };

    /// @brief Makes a read one undo step of `txn` (the recording
    /// transaction, or nullptr: not recorded). Construct before the read;
    /// commit() after a successful one. Otherwise the read is rolled back
    /// as ReadRollback does.
    class RecordedRead
    {
    public:
        RecordedRead(Root &root, le::editing::Transaction *txn, bool tracks_links) : txn_(txn)
        {
            if (txn_)
                guard_.emplace(root, tracks_links);
        }

        void commit()
        {
            if (guard_)
                txn_->record(std::make_unique<ReadCommand>(guard_->commit_for_undo()));
        }

    private:
        le::editing::Transaction *txn_;
        std::optional<ReadRollback> guard_;
    };
}
