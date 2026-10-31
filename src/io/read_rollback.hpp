#pragma once
#include "../database/database.hpp"

#include <utility>
#include <vector>

namespace le
{
    /// @brief Makes a file read all-or-nothing: unless commit() is called,
    /// going out of scope (a failed read, or an exception) deletes every
    /// object the read created, newest first, and puts back the
    /// Technologies it edited in place. Readers only ever create objects
    /// and edit a Technology, so that restores the database as it was.
    /// Copying the Root instead would double peak memory on a large read.
    /// A guard inside another's read leaves the rollback to the outer one.
    class ReadRollback
    {
    public:
        explicit ReadRollback(Root &root) : root_(root)
        {
            if (root.creation_journal())
                return;
            active_ = true;
            for (const TechnologyId id : root.get_technology_ids())
                if (const TechnologyData *technology = root.get_technology(id))
                    technologies_.emplace_back(id, *technology);
            root.open_creation_journal(&journal_);
        }

        ReadRollback(const ReadRollback &) = delete;
        ReadRollback &operator=(const ReadRollback &) = delete;

        ~ReadRollback() { finish(true); }

        /// @brief The read succeeded: keep what it made.
        void commit() { finish(false); }

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
            root_.saturate_change_log();
        }

        Root &root_;
        bool active_ = false;
        std::vector<CreatedObject> journal_;
        std::vector<std::pair<TechnologyId, TechnologyData>> technologies_;
    };
}
