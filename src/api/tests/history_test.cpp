#include "views.hpp"
#include "../../test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <string>

// Undo, the command history, the database file's state and the session's
// cross-thread requests through the C++ API.

TEST(History, UndoAndRedoReportWhetherTheyReplayed)
{
    le::Session session;
    EXPECT_FALSE(session.read().can_undo());
    EXPECT_EQ(session.write().undo(), false); // nothing to undo

    session.write("add lib").create_library({.name = "lib"}).value();
    EXPECT_TRUE(session.read().can_undo());
    EXPECT_FALSE(session.read().can_redo());

    EXPECT_EQ(session.write().undo(), true);
    EXPECT_TRUE(session.read().root().get_library_ids().empty());
    EXPECT_TRUE(session.read().can_redo());

    EXPECT_EQ(session.write().redo(), true);
    EXPECT_EQ(session.read().root().get_library_ids().size(), 1u);
    EXPECT_EQ(session.write().redo(), false);
}

TEST(History, EveryClosedStepIsRecalledInOrder)
{
    le::Session session;
    session.write("first").create_library({.name = "a"}).value();
    {
        le::Transaction transaction = session.transaction("second");
    } // recalled even though it changed nothing
    const le::ReadView view = session.read();
    ASSERT_EQ(view.command_history_size(), 2u);
    EXPECT_EQ(view.command_history_at(0), "first");
    EXPECT_EQ(view.command_history_at(1), "second");
}

TEST(DatabaseFile, TracksUnsavedChangesAndThePathLastWritten)
{
    le::Session session;
    EXPECT_TRUE(session.read().database_is_empty());
    EXPECT_FALSE(session.read().has_unsaved_database());
    EXPECT_EQ(session.read().db_path(), "");

    session.write().create_library({.name = "lib"}).value();
    EXPECT_FALSE(session.read().database_is_empty());
    EXPECT_TRUE(session.read().has_unsaved_database());

    const std::string path = le::test::scratch_path("history_test/saved.led");
    ASSERT_TRUE(session.write().write_db(path));
    EXPECT_FALSE(session.read().has_unsaved_database());
    EXPECT_EQ(session.read().db_path(), path);
}

TEST(DatabaseFile, InfoAndMigrateDescribeAFile)
{
    const std::string path = le::test::scratch_path("history_test/info.led");
    {
        le::Session session;
        session.write().create_library({.name = "lib"}).value();
        ASSERT_TRUE(session.write().write_db(path));
    }
    const std::expected<std::string, std::string> info = le::db_info(path);
    ASSERT_TRUE(info) << info.error();
    EXPECT_NE(info->find("schema version"), std::string::npos);
    EXPECT_NE(info->find("migrations to run: none"), std::string::npos);

    const std::string migrated = le::test::scratch_path("history_test/migrated.led");
    const std::expected<std::string, std::string> report = le::migrate_db(path, migrated);
    ASSERT_TRUE(report) << report.error();
    EXPECT_NE(report->find("migrated"), std::string::npos);
    EXPECT_TRUE(le::db_info(migrated));
}

TEST(DatabaseFile, InfoAndMigrateExplainWhatsMissing)
{
    EXPECT_FALSE(le::db_info(""));
    EXPECT_FALSE(le::db_info(le::test::scratch_path("history_test/missing.led")));
    EXPECT_FALSE(le::migrate_db("", "out.led"));
}

TEST(SessionRequests, GuiRequestsAreTakenOnce)
{
    le::Session session;
    const le::SessionRequests requests = session.requests();
    EXPECT_FALSE(requests.take_show_gui_request());
    requests.request_show_gui();
    EXPECT_TRUE(requests.take_show_gui_request());
    EXPECT_FALSE(requests.take_show_gui_request());

    requests.request_close_gui();
    EXPECT_TRUE(requests.take_close_gui_request());
    EXPECT_FALSE(requests.take_close_gui_request());
}

TEST(SessionRequests, TclCommandsComeOutInOrder)
{
    le::Session session;
    const le::SessionRequests requests = session.requests();
    EXPECT_EQ(requests.take_next_tcl_command(), std::nullopt);
    requests.enqueue_tcl_command("set_hierarchy_depth 2");
    requests.enqueue_tcl_command("fit");
    EXPECT_EQ(requests.take_next_tcl_command(), "set_hierarchy_depth 2");
    EXPECT_EQ(requests.take_next_tcl_command(), "fit");
    EXPECT_EQ(requests.take_next_tcl_command(), std::nullopt);
}
