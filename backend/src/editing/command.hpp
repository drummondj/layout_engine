#pragma once
#include "id_cell.hpp"
#include "../database/database.hpp"
#include <functional>
#include <utility>

/// @brief Generic undo/redo command primitives (UPDATES.md item 21).
/// Every generated Root::create_x/update_x/delete_x is uniform enough
/// (Root::create_x(XxxData) -> XxxId, Root::delete_x(XxxId) -> bool, and
/// an update reduced to one apply_<snake>_snapshot(Root&, XxxId, const
/// XxxData&) helper - see the codegen changes in schema.py) that these
/// three templates need no per-class C++ at all: a caller supplies the
/// two/three function pointers/lambdas plus the value(s) involved, once,
/// per class, at the codegen level.
namespace le::editing
{
    class ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void undo(Root &root) = 0;
        virtual void redo(Root &root) = 0;
    };

    /// @brief Undoes a create by deleting; redoes by re-creating (which
    /// allocates a *new* id - written back into `cell` so anything else
    /// in the same Transaction referencing this object stays correct
    /// across the round trip - see IdCell's own comment).
    template <typename IdT, typename DataT>
    class CreateCommand : public ICommand
    {
    public:
        CreateCommand(IdCellPtr<IdT> cell, DataT data,
                      std::function<IdT(Root &, const DataT &)> create_fn,
                      std::function<bool(Root &, IdT)> delete_fn)
            : cell_(std::move(cell)), data_(std::move(data)),
              create_fn_(std::move(create_fn)), delete_fn_(std::move(delete_fn)) {}

        void undo(Root &root) override { delete_fn_(root, cell_->id); }
        void redo(Root &root) override { cell_->id = create_fn_(root, data_); }

    private:
        IdCellPtr<IdT> cell_;
        DataT data_;
        std::function<IdT(Root &, const DataT &)> create_fn_;
        std::function<bool(Root &, IdT)> delete_fn_;
    };

    /// @brief Undo/redo both replay one full-field snapshot via
    /// `apply_fn` (the before snapshot for undo, the after snapshot for
    /// redo) - this is what update_api_body()'s generated hook and
    /// MoveCommand's own commit path both use.
    template <typename IdT, typename DataT>
    class UpdateCommand : public ICommand
    {
    public:
        UpdateCommand(IdCellPtr<IdT> cell, DataT before, DataT after,
                      std::function<bool(Root &, IdT, const DataT &)> apply_fn)
            : cell_(std::move(cell)), before_(std::move(before)), after_(std::move(after)),
              apply_fn_(std::move(apply_fn)) {}

        void undo(Root &root) override { apply_fn_(root, cell_->id, before_); }
        void redo(Root &root) override { apply_fn_(root, cell_->id, after_); }

    private:
        IdCellPtr<IdT> cell_;
        DataT before_;
        DataT after_;
        std::function<bool(Root &, IdT, const DataT &)> apply_fn_;
    };

    /// @brief The mirror image of CreateCommand: undo re-creates (new id
    /// into `cell`), redo deletes. `create_fn` may itself read a *different*
    /// object's own IdCell at call time to repoint a stale parent-id field
    /// captured in `data` - see the 4 hand-written le_delete_* call sites
    /// in api.cpp for why this matters (cascading deletes).
    template <typename IdT, typename DataT>
    class DeleteCommand : public ICommand
    {
    public:
        DeleteCommand(IdCellPtr<IdT> cell, DataT data,
                      std::function<IdT(Root &, const DataT &)> create_fn,
                      std::function<bool(Root &, IdT)> delete_fn)
            : cell_(std::move(cell)), data_(std::move(data)),
              create_fn_(std::move(create_fn)), delete_fn_(std::move(delete_fn)) {}

        void undo(Root &root) override { cell_->id = create_fn_(root, data_); }
        void redo(Root &root) override { delete_fn_(root, cell_->id); }

    private:
        IdCellPtr<IdT> cell_;
        DataT data_;
        std::function<IdT(Root &, const DataT &)> create_fn_;
        std::function<bool(Root &, IdT)> delete_fn_;
    };
}
