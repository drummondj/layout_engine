#pragma once
#include "../database/database.hpp"
#include <functional>
#include <optional>
#include <utility>

/// @brief Undo/redo command primitives. An object keeps its id across
/// undo and redo: a recorded delete retires the object (Root::retire, its
/// slot reserved), and undo or redo brings it back with Root::restore at
/// the same id and child-list positions, so every id recorded later or
/// held elsewhere stays valid.
namespace le::editing
{
    /// @brief What Root::retire returns for `IdT`'s class.
    template <typename IdT>
    using RetiredOf = typename decltype(std::declval<Root &>().retire(std::declval<IdT>()))::value_type;

    class ICommand
    {
    public:
        virtual ~ICommand() = default;
        /// @brief False if the step couldn't be applied.
        virtual bool undo(Root &root) = 0;
        /// @brief False if the step couldn't be applied.
        virtual bool redo(Root &root) = 0;
        /// @brief The step is leaving the history: give back any id it
        /// holds retired.
        virtual void release(Root &) {}
    };

    /// @brief Undoes a create by retiring the object; redoes it by
    /// restoring it.
    template <typename IdT>
    class CreateCommand : public ICommand
    {
    public:
        explicit CreateCommand(IdT id) : id_(id) {}

        bool undo(Root &root) override
        {
            retired_ = root.retire(id_);
            return retired_.has_value();
        }

        bool redo(Root &root) override
        {
            if (!retired_ || !root.restore(id_, *retired_))
                return false;
            retired_.reset();
            return true;
        }

        void release(Root &root) override
        {
            if (retired_)
                root.release(id_);
        }

    private:
        IdT id_;
        std::optional<RetiredOf<IdT>> retired_; // set while undone
    };

    /// @brief Undo/redo both replay one full-field snapshot via
    /// `apply_fn`: the before snapshot for undo, the after one for redo.
    template <typename IdT, typename DataT>
    class UpdateCommand : public ICommand
    {
    public:
        UpdateCommand(IdT id, DataT before, DataT after, std::function<bool(Root &, IdT, const DataT &)> apply_fn)
            : id_(id), before_(std::move(before)), after_(std::move(after)), apply_fn_(std::move(apply_fn)) {}

        bool undo(Root &root) override { return apply_fn_(root, id_, before_); }
        bool redo(Root &root) override { return apply_fn_(root, id_, after_); }

    private:
        IdT id_;
        DataT before_;
        DataT after_;
        std::function<bool(Root &, IdT, const DataT &)> apply_fn_;
    };

    /// @brief The mirror image of CreateCommand: undo restores the retired
    /// object, redo retires it again.
    template <typename IdT>
    class DeleteCommand : public ICommand
    {
    public:
        DeleteCommand(IdT id, RetiredOf<IdT> retired) : id_(id), retired_(std::move(retired)) {}

        bool undo(Root &root) override
        {
            if (!retired_ || !root.restore(id_, *retired_))
                return false;
            retired_.reset();
            return true;
        }

        bool redo(Root &root) override
        {
            retired_ = root.retire(id_);
            return retired_.has_value();
        }

        void release(Root &root) override
        {
            if (retired_)
                root.release(id_);
        }

    private:
        IdT id_;
        std::optional<RetiredOf<IdT>> retired_; // set while done
    };
}
