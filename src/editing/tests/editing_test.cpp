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

    bool apply_terminal_snapshot(Root &r, TerminalId id, const TerminalData &d)
    {
        return r.update_terminal(id, d.abstract, d.name, d.direction, d.shape, d.use, d.must_join, d.net_expr,
                                  d.leq, d.taper_rule, d.supply_sensitivity, d.ground_sensitivity, d.rise_slew_limit,
                                  d.fall_slew_limit, d.max_load, d.max_delay);
    }

    bool apply_shape_snapshot(Root &r, ShapeId id, const ShapeData &d)
    {
        return r.update_shape(id, d.layer, d.purpose, d.paths, d.polygons, d.rects, d.spacing, d.design_rule_width, d.except_pg_net, d.routing_status, d.route_shape, d.shield_index, d.is_special);
    }

    struct Cell
    {
        Root root;
        AbstractId abstract;

        Cell()
        {
            const LibraryId library = root.create_library(LibraryData{.name = "LIB"});
            const DesignId design = root.create_design(DesignData{.library = library, .name = "CELL"});
            abstract = root.create_abstract(AbstractData{.design = design});
        }

        std::vector<std::string> terminal_names() const
        {
            std::vector<std::string> names;
            for (const TerminalId id : root.get_abstract_terminals(abstract))
                names.push_back(root.get_terminal(id)->name);
            return names;
        }
    };

    // Creates a terminal inside its own recorded command.
    TerminalId create_recorded(CommandHistory &history, Cell &cell, std::string name)
    {
        history.begin("create_terminal " + name);
        const TerminalId id = cell.root.create_terminal(make_terminal_data(cell.abstract, std::move(name)));
        history.current()->record_create(id);
        history.end(true);
        return id;
    }

    void delete_recorded(CommandHistory &history, Cell &cell, TerminalId id)
    {
        history.begin("delete_terminal");
        ASSERT_TRUE(delete_object(cell.root, history.current(), id));
        history.end(true);
    }
}

TEST(Editing, CreateCommandUndoRedoKeepsTheId)
{
    Cell cell;
    Transaction txn("create_terminal");
    const TerminalId created = cell.root.create_terminal(make_terminal_data(cell.abstract));
    ASSERT_TRUE(created.valid());
    txn.record_create(created);

    EXPECT_EQ(txn.undo_all(cell.root), 0u);
    EXPECT_EQ(cell.root.get_terminal(created), nullptr);
    EXPECT_TRUE(cell.terminal_names().empty());
    EXPECT_FALSE(cell.root.get_terminal_by_name(cell.abstract, "IN0").valid());

    EXPECT_EQ(txn.redo_all(cell.root), 0u);
    ASSERT_NE(cell.root.get_terminal(created), nullptr);
    EXPECT_EQ(cell.root.get_terminal(created)->name, "IN0");
    EXPECT_EQ(cell.root.get_terminal_by_name(cell.abstract, "IN0"), created);
    EXPECT_EQ(cell.terminal_names(), std::vector<std::string>{"IN0"});
}

TEST(Editing, UpdateCommandUndoRedoRoundTrip)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LayerId m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
    ShapeData before{.layer = m1};
    before.rects.push_back(Rect{.ll = Point{.x = 0, .y = 0}, .ur = Point{.x = 10, .y = 10}});
    const ShapeId id = root.create_shape(before);

    ShapeData after = *root.get_shape(id);
    after.rects[0] = Rect{.ll = Point{.x = 5, .y = 5}, .ur = Point{.x = 15, .y = 15}};
    ASSERT_TRUE(apply_shape_snapshot(root, id, after));

    Transaction txn("move");
    txn.record_update<ShapeId, ShapeData>(id, before, after, apply_shape_snapshot);

    EXPECT_EQ(txn.undo_all(root), 0u);
    EXPECT_EQ(root.get_shape(id)->rects[0].ll.x, 0);

    EXPECT_EQ(txn.redo_all(root), 0u);
    EXPECT_EQ(root.get_shape(id)->rects[0].ll.x, 5);
}

TEST(Editing, DeleteCommandUndoRedoKeepsTheId)
{
    Cell cell;
    const TerminalId id = cell.root.create_terminal(make_terminal_data(cell.abstract));

    Transaction txn("delete_terminal");
    ASSERT_TRUE(delete_object(cell.root, &txn, id));
    EXPECT_EQ(cell.root.get_terminal(id), nullptr);
    EXPECT_EQ(cell.root.pool_terminal().alive_count(), 0u);

    EXPECT_EQ(txn.undo_all(cell.root), 0u);
    ASSERT_NE(cell.root.get_terminal(id), nullptr);
    EXPECT_EQ(cell.root.get_terminal(id)->name, "IN0");
    EXPECT_EQ(cell.root.get_terminal_by_name(cell.abstract, "IN0"), id);
    EXPECT_EQ(cell.root.pool_terminal().alive_count(), 1u);

    EXPECT_EQ(txn.redo_all(cell.root), 0u);
    EXPECT_EQ(cell.root.get_terminal(id), nullptr);
    EXPECT_TRUE(cell.terminal_names().empty());
}

TEST(Editing, ChainedStepsInOneTransactionSurviveUndoRedo)
{
    Cell cell;
    Transaction txn("create_then_update");

    const TerminalId created = cell.root.create_terminal(make_terminal_data(cell.abstract));
    txn.record_create(created);

    const TerminalData before = *cell.root.get_terminal(created);
    TerminalData after = before;
    after.direction = SignalDirection::OUTPUT;
    ASSERT_TRUE(apply_terminal_snapshot(cell.root, created, after));
    txn.record_update<TerminalId, TerminalData>(created, before, after, apply_terminal_snapshot);

    EXPECT_EQ(txn.undo_all(cell.root), 0u);
    EXPECT_EQ(cell.root.get_terminal(created), nullptr);

    EXPECT_EQ(txn.redo_all(cell.root), 0u);
    const TerminalData *final_data = cell.root.get_terminal(created);
    ASSERT_NE(final_data, nullptr);
    EXPECT_EQ(final_data->direction, SignalDirection::OUTPUT);
}

TEST(Editing, AnIdHeldOutsideTheHistoryStaysValidAcrossDeleteUndoRedo)
{
    Cell cell;
    const TerminalId external_reference = cell.root.create_terminal(make_terminal_data(cell.abstract));

    Transaction txn("delete_terminal");
    ASSERT_TRUE(delete_object(cell.root, &txn, external_reference));
    txn.undo_all(cell.root);
    txn.redo_all(cell.root);
    txn.undo_all(cell.root);

    ASSERT_NE(cell.root.get_terminal(external_reference), nullptr);
    EXPECT_EQ(cell.root.get_terminal(external_reference)->name, "IN0");
}

TEST(Editing, UndoPutsADeletedObjectBackAtItsListPosition)
{
    Cell cell;
    for (const char *name : {"A", "B", "C"})
        cell.root.create_terminal(make_terminal_data(cell.abstract, name));
    const TerminalId b = cell.root.get_terminal_by_name(cell.abstract, "B");

    Transaction txn("delete B");
    ASSERT_TRUE(delete_object(cell.root, &txn, b));
    EXPECT_EQ(cell.terminal_names(), (std::vector<std::string>{"A", "C"}));

    txn.undo_all(cell.root);
    EXPECT_EQ(cell.terminal_names(), (std::vector<std::string>{"A", "B", "C"}));
}

TEST(Editing, UndoingSeveralDeletesRestoresTheOriginalOrder)
{
    Cell cell;
    for (const char *name : {"A", "B", "C", "D"})
        cell.root.create_terminal(make_terminal_data(cell.abstract, name));
    const std::vector<TerminalId> ids = cell.root.get_abstract_terminals(cell.abstract);

    // A cascade's order: every child front to back.
    Transaction txn("delete all");
    for (const TerminalId id : ids)
        ASSERT_TRUE(delete_object(cell.root, &txn, id));
    EXPECT_TRUE(cell.terminal_names().empty());

    txn.undo_all(cell.root);
    EXPECT_EQ(cell.root.get_abstract_terminals(cell.abstract), ids);
    txn.redo_all(cell.root);
    txn.undo_all(cell.root);
    EXPECT_EQ(cell.root.get_abstract_terminals(cell.abstract), ids);
}

TEST(Editing, ARetiredSlotIsNotReusedUntilReleased)
{
    // Create L; delete L; create M; undo; undo - the free list is LIFO, so
    // M would take L's slot if a recorded delete freed it.
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId l = create_recorded(history, cell, "L");
    delete_recorded(history, cell, l);
    const TerminalId m = create_recorded(history, cell, "M");
    EXPECT_NE(m.index, l.index);

    EXPECT_EQ(history.undo().failed_steps, 0u); // M's create
    EXPECT_EQ(history.undo().failed_steps, 0u); // L's delete
    ASSERT_NE(cell.root.get_terminal(l), nullptr);
    EXPECT_EQ(cell.root.get_terminal(l)->name, "L");
    EXPECT_EQ(cell.root.get_terminal(m), nullptr);

    EXPECT_EQ(history.redo().failed_steps, 0u);
    EXPECT_EQ(history.redo().failed_steps, 0u);
    EXPECT_EQ(cell.root.get_terminal(l), nullptr);
    ASSERT_NE(cell.root.get_terminal(m), nullptr);
    EXPECT_EQ(cell.terminal_names(), std::vector<std::string>{"M"});
}

TEST(Editing, UndoDeleteThenUndoCreateLeavesNothingBehind)
{
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId t = create_recorded(history, cell, "T");
    delete_recorded(history, cell, t);

    history.undo();
    ASSERT_NE(cell.root.get_terminal(t), nullptr);
    history.undo();
    EXPECT_EQ(cell.root.get_terminal(t), nullptr);
    EXPECT_TRUE(cell.terminal_names().empty());
    EXPECT_EQ(cell.root.pool_terminal().alive_count(), 0u);
}

TEST(Editing, AnUpdateRecordedAfterARedoStillApplies)
{
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId t = create_recorded(history, cell, "T");
    history.undo();
    history.redo();

    history.begin("update_terminal");
    const TerminalData before = *cell.root.get_terminal(t);
    TerminalData after = before;
    after.direction = SignalDirection::OUTPUT;
    ASSERT_TRUE(apply_terminal_snapshot(cell.root, t, after));
    history.current()->record_update<TerminalId, TerminalData>(t, before, after, apply_terminal_snapshot);
    history.end(true);

    // Undo both, then redo both: the update lands on the same object.
    history.undo();
    history.undo();
    EXPECT_EQ(history.redo().failed_steps, 0u);
    EXPECT_EQ(history.redo().failed_steps, 0u);
    ASSERT_NE(cell.root.get_terminal(t), nullptr);
    EXPECT_EQ(cell.root.get_terminal(t)->direction, SignalDirection::OUTPUT);
}

TEST(Editing, ANewEditReleasesTheRedoStacksRetiredIds)
{
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId t = create_recorded(history, cell, "T");
    history.undo();
    EXPECT_TRUE(cell.root.pool_terminal().is_retired(t));

    // The new edit drops the undone create; its slot comes back for the
    // next create, under a new generation.
    create_recorded(history, cell, "U");
    EXPECT_FALSE(history.can_redo());
    EXPECT_FALSE(cell.root.pool_terminal().is_retired(t));
    const TerminalId v = cell.root.create_terminal(make_terminal_data(cell.abstract, "V"));
    EXPECT_EQ(v.index, t.index);
    EXPECT_NE(v.generation, t.generation);
    EXPECT_EQ(cell.root.get_terminal(t), nullptr);
    EXPECT_EQ(cell.terminal_names(), (std::vector<std::string>{"U", "V"}));
}

TEST(Editing, ClearingTheHistoryReleasesRetiredIds)
{
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId t = create_recorded(history, cell, "T");
    delete_recorded(history, cell, t);
    EXPECT_TRUE(cell.root.pool_terminal().is_retired(t));

    history.clear_undo_redo();
    EXPECT_FALSE(cell.root.pool_terminal().is_retired(t));
    const TerminalId reused = cell.root.create_terminal(make_terminal_data(cell.abstract, "R"));
    EXPECT_EQ(reused.index, t.index);
    EXPECT_EQ(cell.root.get_terminal(t), nullptr);
}

TEST(Editing, AStepThatCannotBeRestoredIsReported)
{
    Cell cell;
    CommandHistory history(cell.root);
    create_recorded(history, cell, "T");
    history.undo();
    // An unrecorded edit takes the name, so redo's restore would clash.
    const TerminalId other = cell.root.create_terminal(make_terminal_data(cell.abstract, "T"));

    const ReplayResult redone = history.redo();
    EXPECT_TRUE(redone.replayed);
    EXPECT_EQ(redone.steps, 1u);
    EXPECT_EQ(redone.failed_steps, 1u);
    EXPECT_EQ(redone.label, "create_terminal T");
    EXPECT_EQ(cell.root.get_terminal_by_name(cell.abstract, "T"), other);
    EXPECT_EQ(cell.terminal_names(), std::vector<std::string>{"T"});
}

TEST(Pool, EraseAndRetireResetTheSlotsValue)
{
    Cell cell;
    const TerminalId erased = cell.root.create_terminal(make_terminal_data(cell.abstract, "ERASED"));
    const TerminalId retired = cell.root.create_terminal(make_terminal_data(cell.abstract, "RETIRED"));
    ASSERT_TRUE(cell.root.delete_terminal(erased));
    ASSERT_TRUE(cell.root.retire(retired).has_value());
    const auto &slots = cell.root.pool_terminal().slots();
    EXPECT_TRUE(slots[erased.index].value.name.empty());
    EXPECT_TRUE(slots[retired.index].value.name.empty());
    EXPECT_EQ(cell.root.pool_terminal().alive_count(), 0u);
}

TEST(CommandHistoryTest, EmptyTransactionNeverPushedOntoUndoStack)
{
    Root root;
    CommandHistory history(root);
    history.begin("noop_read_command");
    history.end(/*succeeded=*/true);
    EXPECT_FALSE(history.can_undo());
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "noop_read_command");
}

TEST(CommandHistoryTest, FailedCommandIsStillRecordedInRecallLog)
{
    // A failed command is exactly the one a user most wants back, to
    // recall and edit into a working one.
    Root root;
    CommandHistory history(root);
    history.begin("bad_command");
    history.end(/*succeeded=*/false);
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "bad_command");
    EXPECT_FALSE(history.can_undo()); // still recorded zero undoable steps
}

TEST(CommandHistoryTest, BeginWhileAlreadyRecordingIsANoOp)
{
    Root root;
    CommandHistory history(root);
    history.begin("outer");
    history.begin("inner"); // no nesting - ignored
    history.end(true);
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "outer");
}

TEST(CommandHistoryTest, UndoRedoRoundTripsThroughCommandHistory)
{
    Cell cell;
    CommandHistory history(cell.root);
    const TerminalId id = create_recorded(history, cell, "IN0");

    EXPECT_TRUE(history.can_undo());
    EXPECT_FALSE(history.can_redo());
    ASSERT_EQ(history.recall_count(), 1u);
    EXPECT_EQ(history.recall_at(0), "create_terminal IN0");

    EXPECT_TRUE(history.undo().replayed);
    EXPECT_EQ(cell.root.get_terminal(id), nullptr);
    EXPECT_FALSE(history.can_undo());
    EXPECT_TRUE(history.can_redo());

    EXPECT_TRUE(history.redo().replayed);
    EXPECT_NE(cell.root.get_terminal(id), nullptr);
    EXPECT_TRUE(history.can_undo());
    EXPECT_FALSE(history.can_redo());

    // Nothing left to undo/redo beyond the one recorded transaction.
    EXPECT_TRUE(history.undo().replayed);
    EXPECT_FALSE(history.undo().replayed);
}

namespace
{
    // One transaction that creates a terminal named `name`.
    TerminalId create_in_transaction(Cell &cell, CommandHistory &history, const std::string &name)
    {
        history.begin("create " + name);
        const TerminalId id = cell.root.create_terminal(make_terminal_data(cell.abstract, name));
        history.current()->record_create(id);
        history.end(true);
        return id;
    }
}

TEST(CommandHistoryTest, TheUndoStackKeepsOnlyTheNewestUndoLimitTransactions)
{
    Cell cell;
    CommandHistory history(cell.root);
    EXPECT_EQ(history.undo_limit(), CommandHistory::kDefaultUndoLimit);
    history.set_undo_limit(3);
    for (int i = 0; i < 5; ++i)
        create_in_transaction(cell, history, "T" + std::to_string(i));

    std::vector<std::string> undone;
    for (ReplayResult result = history.undo(); result.replayed; result = history.undo())
        undone.push_back(result.label);
    EXPECT_EQ(undone, (std::vector<std::string>{"create T4", "create T3", "create T2"}));
    // The trimmed transactions' edits stand.
    EXPECT_EQ(cell.terminal_names(), (std::vector<std::string>{"T0", "T1"}));

    // Lowering the limit trims at once, newest kept.
    for (int i = 0; i < 3; ++i)
        history.redo();
    history.set_undo_limit(1);
    EXPECT_EQ(history.undo().label, "create T4");
    EXPECT_FALSE(history.can_undo());
}

TEST(CommandHistoryTest, ATrimmedTransactionReleasesItsRetiredSlots)
{
    Cell cell;
    CommandHistory history(cell.root);
    history.set_undo_limit(1);
    const TerminalId first = create_in_transaction(cell, history, "A");

    history.begin("delete A");
    ASSERT_TRUE(delete_object(cell.root, history.current(), first));
    history.end(true);
    // Retired: undo could still bring A back, so its slot isn't reused.
    EXPECT_NE(create_in_transaction(cell, history, "B").index, first.index);
    // That create trimmed "delete A" (limit 1), releasing A's slot.
    EXPECT_EQ(create_in_transaction(cell, history, "C").index, first.index);
}

TEST(CommandHistoryTest, TheRecallLogKeepsTheNewestLines)
{
    Cell cell;
    CommandHistory history(cell.root);
    for (size_t i = 0; i < CommandHistory::kRecallLimit + 5; ++i)
    {
        history.begin("cmd " + std::to_string(i));
        history.end(true);
    }
    ASSERT_EQ(history.recall_count(), CommandHistory::kRecallLimit);
    EXPECT_EQ(history.recall_at(0), "cmd 5");
    EXPECT_EQ(history.recall_at(CommandHistory::kRecallLimit - 1), "cmd " + std::to_string(CommandHistory::kRecallLimit + 4));
}
