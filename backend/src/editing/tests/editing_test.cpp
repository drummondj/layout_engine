#include "../transaction.hpp"
#include "../command_history.hpp"
#include "../../database/database.hpp"
#include <gtest/gtest.h>

using namespace le;
using namespace le::editing;

namespace
{
    TerminalData make_terminal_data(AbstractId abstract, std::string name = "IN0")
    {
        return TerminalData{.abstract = abstract, .name = std::move(name), .direction = SignalDirection::INPUT};
    }

    auto create_terminal_fn = [](Root &r, const TerminalData &d) { return r.create_terminal(d); };
    auto delete_terminal_fn = [](Root &r, TerminalId i) { return r.delete_terminal(i); };

    bool apply_terminal_snapshot(Root &r, TerminalId id, const TerminalData &d)
    {
        return r.update_terminal(id, d.abstract, d.name, d.direction, d.shape, d.use, d.must_join, d.net_expr,
                                  d.leq, d.taper_rule, d.supply_sensitivity, d.ground_sensitivity, d.rise_slew_limit,
                                  d.fall_slew_limit, d.max_load, d.max_delay);
    }

    bool apply_shape_snapshot(Root &r, ShapeId id, const ShapeData &d)
    {
        return r.update_shape(id, d.layer_name, d.paths, d.polygons, d.rects, d.spacing, d.design_rule_width, d.except_pg_net);
    }
}

TEST(Editing, CreateCommandUndoRedoRoundTrip)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});

    Transaction txn("create_terminal");
    const TerminalData data = make_terminal_data(abstract_id);
    const TerminalId created = root.create_terminal(data);
    ASSERT_TRUE(created.valid());
    txn.record_create<TerminalId, TerminalData>(created, data, create_terminal_fn, delete_terminal_fn);

    txn.undo_all(root);
    EXPECT_EQ(root.get_terminal(created), nullptr);

    txn.redo_all(root);
    // Pool::create() always allocates a fresh index/generation - redo
    // never reproduces the original id (see IdCell's own comment).
    const auto cell = txn.id_cell_for(created);
    EXPECT_NE(cell->id, created);
    ASSERT_NE(root.get_terminal(cell->id), nullptr);
    EXPECT_EQ(root.get_terminal(cell->id)->name, "IN0");
}

TEST(Editing, UpdateCommandUndoRedoRoundTrip)
{
    Root root;
    ShapeData before{.layer_name = "M1"};
    before.rects.push_back(Rect{.ll = Point{.x = 0, .y = 0}, .ur = Point{.x = 10, .y = 10}});
    const ShapeId id = root.create_shape(before);

    ShapeData after = *root.get_shape(id);
    after.rects[0] = Rect{.ll = Point{.x = 5, .y = 5}, .ur = Point{.x = 15, .y = 15}};
    ASSERT_TRUE(apply_shape_snapshot(root, id, after));

    Transaction txn("move");
    txn.record_update<ShapeId, ShapeData>(id, before, after, apply_shape_snapshot);

    txn.undo_all(root);
    EXPECT_EQ(root.get_shape(id)->rects[0].ll.x, 0);

    txn.redo_all(root);
    EXPECT_EQ(root.get_shape(id)->rects[0].ll.x, 5);
}

TEST(Editing, DeleteCommandUndoRedoRoundTrip)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});
    const TerminalId id = root.create_terminal(make_terminal_data(abstract_id));

    Transaction txn("delete_terminal");
    const TerminalData snapshot = *root.get_terminal(id);
    ASSERT_TRUE(root.delete_terminal(id));
    txn.record_delete<TerminalId, TerminalData>(id, snapshot, create_terminal_fn, delete_terminal_fn);

    // Undoing a delete step re-creates the object, at a new id.
    txn.undo_all(root);
    const auto cell = txn.id_cell_for(id);
    EXPECT_NE(cell->id, id);
    ASSERT_NE(root.get_terminal(cell->id), nullptr);
    EXPECT_EQ(root.get_terminal(cell->id)->name, "IN0");

    // Redoing deletes it again, following the cell's now-current id.
    txn.redo_all(root);
    EXPECT_EQ(root.get_terminal(cell->id), nullptr);
}

TEST(Editing, IdCellKeepsChainedStepsCorrectAcrossUndoRedo)
{
    // Proves the id-cell indirection: a transaction that creates an
    // object then updates the *same* object later in the same
    // transaction must still apply that update to whatever id the
    // create step most recently produced, even after an undo->redo
    // round trip changes it.
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});

    Transaction txn("create_then_update");

    const TerminalData create_data = make_terminal_data(abstract_id);
    const TerminalId created = root.create_terminal(create_data);
    txn.record_create<TerminalId, TerminalData>(created, create_data, create_terminal_fn, delete_terminal_fn);

    const TerminalData before = *root.get_terminal(created);
    TerminalData after = before;
    after.direction = SignalDirection::OUTPUT;
    ASSERT_TRUE(apply_terminal_snapshot(root, created, after));
    txn.record_update<TerminalId, TerminalData>(created, before, after, apply_terminal_snapshot);

    // Undo the whole transaction (reverse order: update, then create).
    txn.undo_all(root);
    EXPECT_EQ(root.get_terminal(created), nullptr);

    // Redo the whole transaction (forward order: create at a new id,
    // then update - must land on that new id, not the stale original).
    txn.redo_all(root);
    const auto cell = txn.id_cell_for(created);
    ASSERT_NE(cell->id, created);
    const TerminalData *final_data = root.get_terminal(cell->id);
    ASSERT_NE(final_data, nullptr);
    EXPECT_EQ(final_data->direction, SignalDirection::OUTPUT);
}

TEST(Editing, ExternalReferenceNotRepointedAcrossDeleteUndoRedo)
{
    // Documents the accepted limitation from transaction.hpp's own
    // comment: an id held by something *outside* this Transaction isn't
    // retroactively repointed when the transaction deletes, undoes, and
    // redoes that same object - it keeps pointing at the now-stale
    // original id. Not a new failure mode (Scene already documents and
    // tolerates the same class of staleness from ordinary pool-slot
    // reuse) - just a new trigger for it, deliberately left unaddressed
    // this round.
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});
    const TerminalId id = root.create_terminal(make_terminal_data(abstract_id));
    const TerminalId external_reference = id;

    Transaction txn("delete_terminal");
    const TerminalData snapshot = *root.get_terminal(id);
    ASSERT_TRUE(root.delete_terminal(id));
    txn.record_delete<TerminalId, TerminalData>(id, snapshot, create_terminal_fn, delete_terminal_fn);

    txn.undo_all(root); // recreate at a new id
    txn.redo_all(root);  // delete again
    txn.undo_all(root); // recreate at yet another new id

    EXPECT_EQ(root.get_terminal(external_reference), nullptr);

    const auto cell = txn.id_cell_for(id);
    EXPECT_NE(root.get_terminal(cell->id), nullptr);
}

TEST(CommandHistoryTest, EmptyTransactionNeverPushedOntoUndoStack)
{
    CommandHistory history;
    history.begin("noop_read_command");
    history.end(/*succeeded=*/true);
    EXPECT_FALSE(history.can_undo());
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "noop_read_command");
}

TEST(CommandHistoryTest, FailedCommandNotRecordedInRecallLog)
{
    CommandHistory history;
    history.begin("bad_command");
    history.end(/*succeeded=*/false);
    EXPECT_EQ(history.recall_count(), 0u);
}

TEST(CommandHistoryTest, BeginWhileAlreadyRecordingIsANoOp)
{
    CommandHistory history;
    history.begin("outer");
    history.begin("inner"); // no nesting this round - ignored
    history.end(true);
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "outer");
}

TEST(CommandHistoryTest, UndoRedoRoundTripsThroughCommandHistory)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});

    CommandHistory history;
    history.begin("create_terminal IN0");
    const TerminalData data = make_terminal_data(abstract_id);
    const TerminalId id = root.create_terminal(data);
    ASSERT_TRUE(history.is_recording());
    history.current()->record_create<TerminalId, TerminalData>(id, data, create_terminal_fn, delete_terminal_fn);
    history.end(true);

    EXPECT_TRUE(history.can_undo());
    EXPECT_FALSE(history.can_redo());
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "create_terminal IN0");

    EXPECT_TRUE(history.undo(root));
    EXPECT_EQ(root.get_terminal(id), nullptr);
    EXPECT_FALSE(history.can_undo());
    EXPECT_TRUE(history.can_redo());

    EXPECT_TRUE(history.redo(root));
    EXPECT_TRUE(history.can_undo());
    EXPECT_FALSE(history.can_redo());

    // Nothing left to undo/redo beyond the one recorded transaction.
    EXPECT_TRUE(history.undo(root));
    EXPECT_FALSE(history.undo(root));
}
