#pragma once
#include "../database/database.hpp"

#include <utility>
#include <vector>

namespace le
{
    /// @brief What a committed read changed, for undoing it
    /// (ReadRollback::commit_for_undo): the objects it created, the
    /// Technologies it edited in place as they were before, and the
    /// references it set on objects older than the read.
    struct ReadRecord
    {
        std::vector<CreatedObject> created;
        std::vector<std::pair<TechnologyId, TechnologyData>> technologies;
        std::vector<std::pair<InstanceId, DesignId>> linked; // reference_design it set
        std::vector<std::pair<DesignId, DesignId>> based;    // base_design it set
    };

    /// @brief Makes a file read all-or-nothing: unless commit() is called,
    /// going out of scope (a failed read, or an exception) deletes every
    /// object the read created, newest first, and puts back the
    /// Technologies it edited in place. Readers only ever create objects
    /// and edit a Technology, except that a Verilog read links older
    /// unresolved Instances and may give an older Design a base_design;
    /// with `tracks_links` those are rolled back too, and recorded by
    /// commit_for_undo(). Copying the Root instead would double peak memory on a large read.
    /// A guard inside another's read leaves the rollback to the outer one.
    class ReadRollback
    {
    public:
        explicit ReadRollback(Root &root, bool tracks_links = false) : root_(root)
        {
            if (root.creation_journal())
                return;
            active_ = true;
            for (const TechnologyId id : root.get_technology_ids())
                if (const TechnologyData *technology = root.get_technology(id))
                    technologies_.emplace_back(id, *technology);
            if (tracks_links)
            {
                root.for_each_instance_id([&](InstanceId id)
                                          {
                    const InstanceData *instance = root.get_instance(id);
                    if (instance && !instance->reference_design.valid())
                        unresolved_instances_.push_back(id); });
                for (const DesignId id : root.get_design_ids())
                    if (const DesignData *design = root.get_design(id); design && !design->base_design.valid())
                        baseless_designs_.push_back(id);
            }
            root.open_creation_journal(&journal_);
        }

        ReadRollback(const ReadRollback &) = delete;
        ReadRollback &operator=(const ReadRollback &) = delete;

        ~ReadRollback() { finish(true); }

        /// @brief The read succeeded: keep what it made.
        void commit() { finish(false); }

        /// @brief commit(), returning what undoing the read needs. Empty
        /// for a guard inside another's read.
        ReadRecord commit_for_undo()
        {
            if (!active_)
                return {};
            finish(false);
            ReadRecord record{.created = std::move(journal_), .technologies = std::move(technologies_)};
            for (const InstanceId id : unresolved_instances_)
                if (const InstanceData *instance = root_.get_instance(id); instance && instance->reference_design.valid())
                    record.linked.emplace_back(id, instance->reference_design);
            for (const DesignId id : baseless_designs_)
                if (const DesignData *design = root_.get_design(id); design && design->base_design.valid())
                    record.based.emplace_back(id, design->base_design);
            return record;
        }

    private:
        void finish(bool roll_back) noexcept
        {
            if (!std::exchange(active_, false))
                return;
            root_.open_creation_journal(nullptr);
            if (!roll_back)
                return;
            // Newest first: children go before their parents, and each is
            // its parent's last child, so removing it is O(1).
            for (auto it = journal_.rbegin(); it != journal_.rend(); ++it)
                root_.delete_created(*it);
            for (auto &[id, data] : technologies_)
                if (TechnologyData *technology = root_.get_technology(id))
                    *technology = std::move(data);
            for (const InstanceId id : unresolved_instances_)
                if (InstanceData *instance = root_.get_instance(id))
                    instance->reference_design = DesignId{};
            for (const DesignId id : baseless_designs_)
                if (DesignData *design = root_.get_design(id))
                    design->base_design = DesignId{};
            root_.saturate_change_log();
        }

        Root &root_;
        bool active_ = false;
        std::vector<CreatedObject> journal_;
        std::vector<std::pair<TechnologyId, TechnologyData>> technologies_;
        std::vector<InstanceId> unresolved_instances_;
        std::vector<DesignId> baseless_designs_;
    };
}
