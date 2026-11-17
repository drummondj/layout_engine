#include "session_test.hpp"
#include "../le_handle.hpp"
#include "../../io/native_format.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include "../../test_support/scratch_dir.hpp"
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

// write_db / read_db / db_info - the native database file through the
// views: session state after a load, the empty-session rule, and the
// unsaved-changes flag.

namespace
{
    namespace fs = std::filesystem;

    using le::test::fixture_path;

    // db_info's description, or "error: " and why.
    std::string db_info_text(const std::string &path)
    {
        const auto info = le::db_info(path);
        return info ? *info : "error: " + info.error();
    }

    // migrate_db's summary, or "error: " and why.
    std::string migrate_db_text(const std::string &in, const std::string &out)
    {
        const auto summary = le::migrate_db(in, out);
        return summary ? *summary : "error: " + summary.error();
    }

    struct NativeDbApi : public ::testing::Test
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_native_db_api");
            fs::create_directories(dir);
            path = (dir / "testcell.led").string();
            ASSERT_TRUE(source.read_lef(fixture_path("testcell.lef").c_str(), "testcell"));
            ASSERT_TRUE(source.read_def(fixture_path("testcell.def").c_str(), "testcell"));
        }
        void TearDown() override
        {
            fs::remove_all(dir);
        }

        fs::path dir;
        std::string path;
        le::test::SessionHelpers source;
    };
}

TEST_F(NativeDbApi, WriteThenReadIntoAFreshSessionRestoresTheDesign)
{
    ASSERT_TRUE(source.write().write_db(path, true).has_value());

    le::test::SessionHelpers loaded;
    ASSERT_TRUE(loaded.write().read_db(path, true).has_value());
    EXPECT_EQ(static_cast<int>(loaded.design_count()), static_cast<int>(source.design_count()));
    EXPECT_EQ(loaded.read().layer_rows().size(), source.read().layer_rows().size());
    ASSERT_EQ(loaded.library_design_count(0), 1);
    const le::test::SessionHelpers::DesignInfo design = loaded.library_design_at(0, 0);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX);
    EXPECT_NE(design.layout_id.index, UINT32_MAX);

    // A freshly loaded file is saved, with nothing to undo.
    EXPECT_EQ(loaded.read().has_unsaved_database(), 0);
    EXPECT_EQ(loaded.read().can_undo(), 0);
}

TEST_F(NativeDbApi, WriteSavesPendingEdits)
{
    source.write().create_library({.name = "EDITLIB"}).value_or(le::LibraryId{});
    EXPECT_EQ(source.read().has_unsaved_database(), 1);
    EXPECT_FALSE(source.write().write_db((dir / "no_such_dir" / "x.led").string(), true).has_value());
    EXPECT_EQ(source.read().has_unsaved_database(), 1); // a failed write saves nothing
    ASSERT_TRUE(source.write().write_db(path, true).has_value());
    EXPECT_EQ(source.read().has_unsaved_database(), 0);
}

TEST_F(NativeDbApi, ReadIntoANonEmptySessionFailsAndChangesNothing)
{
    ASSERT_TRUE(source.write().write_db(path, true).has_value());
    const int32_t designs = static_cast<int>(source.design_count());
    EXPECT_FALSE(source.write().read_db(path, true).has_value());
    EXPECT_EQ(static_cast<int>(source.design_count()), designs);
}

TEST_F(NativeDbApi, MissingOrCorruptFilesFail)
{
    le::test::SessionHelpers loaded;
    EXPECT_FALSE(loaded.write().read_db((dir / "absent.led").string(), true).has_value());
    EXPECT_FALSE(loaded.write().read_db(fixture_path("testcell.def"), true).has_value());
    EXPECT_EQ(static_cast<int>(loaded.design_count()), 0);
}

TEST_F(NativeDbApi, WriteFailureIsReported)
{
    EXPECT_FALSE(source.write().write_db((dir / "no_such_dir" / "x.led").string(), true).has_value());
    EXPECT_FALSE(source.write().write_db("", true).has_value());
}

TEST_F(NativeDbApi, DbInfoDescribesTheFile)
{
    ASSERT_TRUE(source.write().write_db(path, true).has_value());
    const std::string info = db_info_text(path);
    EXPECT_NE(info.find("same schema"), std::string::npos) << info;
    EXPECT_NE(info.find("Technology"), std::string::npos) << info;
    EXPECT_NE(info.find("Layout"), std::string::npos) << info;
    EXPECT_NE(info.find("migrations to run: none"), std::string::npos) << info;

    const std::string missing = db_info_text((dir / "absent.led").string());
    EXPECT_EQ(missing.rfind("error: ", 0), 0u) << missing;
}

// migrate_db rewrites a file without touching any session's database.
TEST_F(NativeDbApi, MigrateDbRewritesAFile)
{
    ASSERT_TRUE(source.write().write_db(path, true).has_value());
    const std::string out = (dir / "migrated.led").string();
    const std::string summary = migrate_db_text(path, out);
    EXPECT_EQ(summary.rfind("migrated " + path, 0), 0u) << summary;
    EXPECT_NE(std::string(db_info_text(out)).find("session: yes"), std::string::npos);

    le::test::SessionHelpers loaded;
    EXPECT_TRUE(loaded.write().read_db(out, true).has_value());
    EXPECT_EQ(static_cast<int>(loaded.design_count()), static_cast<int>(source.design_count()));

    EXPECT_EQ(std::string(migrate_db_text((dir / "absent.led").string(), out)).rfind("error: ", 0), 0u);
    EXPECT_EQ(std::string(migrate_db_text(path, "")).rfind("error: ", 0), 0u);
}

// The session saved beside the database: the open view, viewport, current
// objects and layer/purpose/filter visibility come back with the file.
TEST_F(NativeDbApi, TheSessionComesBackWithTheFile)
{
    ASSERT_TRUE(source.open_layout(source.design_at(source.design_count() - 1)));
    source.handle->set_pan(le::Point{1234, -567});
    source.handle->set_scale(0.125);
    source.write().set_view_flip(le::ViewFlip::VERTICAL);
    source.write().set_layer_visible("M1", false);
    source.write().set_layer_selectable("M1", 0);
    const auto routes = le::purpose_from_label("route");
    ASSERT_TRUE(routes.has_value());
    source.write().set_purpose_visible(*routes, 0);
    source.write().set_object_filter_visible(le::ObjectFilter::ROUTE_USE, "CLOCK", 0);
    source.write().set_object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "FOLLOWPIN", 0);
    source.write().set_object_filter_selectable(le::ObjectFilter::ROUTING_STATUS, "FIXED", 0);
    source.write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "COVER", 0);
    const le::LayoutId layout = source.handle->current_layout();
    ASSERT_TRUE(source.handle->root.get_layout(layout));
    ASSERT_TRUE(source.write().write_db(path, true).has_value());
    EXPECT_EQ(source.read().db_path(), path);

    le::test::SessionHelpers loaded;
    EXPECT_TRUE(loaded.read().database_is_empty());
    ASSERT_TRUE(loaded.write().read_db(path, true).has_value());
    EXPECT_FALSE(loaded.read().database_is_empty());
    EXPECT_EQ(loaded.read().db_path(), path);
    ASSERT_TRUE(loaded.handle->root.get_layout(loaded.handle->current_layout()));
    EXPECT_EQ(loaded.handle->root.get_layout(loaded.handle->current_layout())->design, source.handle->root.get_layout(layout)->design);
    EXPECT_EQ(loaded.handle->current_layout_id, loaded.handle->current_layout());
    EXPECT_EQ(loaded.handle->pan().x, 1234);
    EXPECT_EQ(loaded.handle->pan().y, -567);
    EXPECT_DOUBLE_EQ(loaded.handle->scale(), 0.125);
    EXPECT_EQ(loaded.read().view().flip, le::ViewFlip::VERTICAL);
    EXPECT_FALSE(loaded.read().layer_visible("M1"));
    EXPECT_EQ(loaded.read().layer_selectable("M1"), 0);
    EXPECT_EQ(loaded.read().purpose_visible(*routes), 0);
    EXPECT_EQ(loaded.read().object_filter_visible(le::ObjectFilter::ROUTE_USE, "CLOCK"), 0);
    EXPECT_EQ(loaded.read().object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "FOLLOWPIN"), 0);
    EXPECT_EQ(loaded.read().object_filter_selectable(le::ObjectFilter::ROUTING_STATUS, "FIXED"), 0);
    EXPECT_EQ(loaded.read().object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "COVER"), 0);
    EXPECT_EQ(loaded.read().has_unsaved_database(), 0) << "restoring the session isn't an edit";

    // Read without the session: the defaults.
    le::test::SessionHelpers without_session;
    ASSERT_TRUE(without_session.write().read_db(path, false).has_value());
    EXPECT_FALSE(without_session.handle->root.get_layout(without_session.handle->current_layout()));
    EXPECT_TRUE(without_session.read().purpose_visible(*routes));
    EXPECT_EQ(without_session.read().view().flip, le::ViewFlip::NONE);
}

TEST_F(NativeDbApi, AMistypedSessionLoadsTheDesignAndSkipsWhatItCantRead)
{
    // Not object references, so the save stores them as they are.
    const std::string session = R"({"format": "one",
        "current": {"layout": {"index": 0, "generation": "g"}},
        "view": {"layout": {"index": "zero"}, "abstract": {"index": 99999999999}},
        "viewport": {"pan": ["x", 1], "scale": "big", "flip": 3},
        "layers": {"visible": {"M1": "no"}}})";
    ASSERT_TRUE(le::persistence::save_native(source.handle->root, path, le::persistence::SaveOptions{.session_json = session}).ok());

    le::test::SessionHelpers loaded;
    ASSERT_TRUE(loaded.write().read_db(path, true).has_value());
    EXPECT_EQ(static_cast<int>(loaded.design_count()), static_cast<int>(source.design_count()));
    EXPECT_FALSE(loaded.handle->root.get_layout(loaded.handle->current_layout()));
    EXPECT_TRUE(loaded.read().layer_visible("M1"));
    EXPECT_EQ(loaded.read().view().flip, le::ViewFlip::NONE);
}

TEST_F(NativeDbApi, NoSessionSavesTheDatabaseOnly)
{
    ASSERT_TRUE(source.open_layout(source.design_at(source.design_count() - 1)));
    source.write().set_layer_visible("M1", false);
    ASSERT_TRUE(source.write().write_db(path, false).has_value());
    EXPECT_NE(std::string(db_info_text(path)).find("session: no"), std::string::npos);

    le::test::SessionHelpers loaded;
    ASSERT_TRUE(loaded.write().read_db(path, true).has_value());
    EXPECT_FALSE(loaded.handle->root.get_layout(loaded.handle->current_layout()));
    EXPECT_TRUE(loaded.read().layer_visible("M1"));
}

// The vendored LEF/DEF parsers and writers keep process-wide state: two
// handles reading and writing at once must give what one at a time does.
TEST(LefDefAcrossHandles, ConcurrentReadsAndWritesMatchSequentialOnes)
{
    const fs::path dir = le::test::scratch_path("le_lefdef_threads");
    fs::create_directories(dir);
    const auto read_text = [](const fs::path &path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    // Reads the testcell fixtures into a fresh handle and writes them back.
    const auto round_trip = [&](const std::string &tag) {
        le::test::SessionHelpers session;
        bool ok = session.read_lef(fixture_path("testcell.lef"), "testcell") && session.read_def(fixture_path("testcell.def"), "testcell");
        const le::test::SessionHelpers::DesignInfo design = session.library_design_at(0, 0);
        const le::LibraryId library = session.library_at(0).id;
        ok = ok && session.write().write_lef((dir / (tag + ".lef")).string(), {}, library, le::LefLayerWriteMode::INCLUDE_WITH_ABSTRACT).has_value();
        ok = ok && session.write().write_def((dir / (tag + ".def")).string(), design.layout_id).has_value();
        return ok;
    };
    ASSERT_TRUE(round_trip("expected"));

    constexpr int kThreads = 4, kRounds = 10;
    std::vector<std::thread> threads;
    std::atomic<int> failures = 0;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r)
                if (!round_trip("t" + std::to_string(t) + "_" + std::to_string(r)))
                    ++failures;
        });
    for (std::thread &thread : threads)
        thread.join();
    EXPECT_EQ(failures.load(), 0);
    const std::string lef = read_text(dir / "expected.lef"), def = read_text(dir / "expected.def");
    for (int t = 0; t < kThreads; ++t)
        for (int r = 0; r < kRounds; ++r)
        {
            const std::string tag = "t" + std::to_string(t) + "_" + std::to_string(r);
            EXPECT_EQ(read_text(dir / (tag + ".lef")), lef) << tag;
            EXPECT_EQ(read_text(dir / (tag + ".def")), def) << tag;
        }
    fs::remove_all(dir);
}
