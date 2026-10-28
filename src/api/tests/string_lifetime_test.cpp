#include "../api.hpp"
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

// A returned C string stays valid until the same function's next call on
// the same thread, whatever happens to the handle meanwhile.

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
    }

    // Longer than any small-string buffer, so a rename moves the stored
    // name to the heap and overwrites where a short one was.
    const std::string kLongName(64, 'x');

    struct StringLifetime : public ::testing::Test
    {
        LeHandle *handle = le_create();
        ~StringLifetime() override { le_destroy(handle); }

        LeProperty find_property(LeObjectRef ref, const char *name) const
        {
            const int32_t count = le_object_property_count(handle, ref);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeProperty property = le_object_property_at(handle, ref, i);
                if (property.name && std::strcmp(property.name, name) == 0)
                    return property;
            }
            return LeProperty{};
        }
    };

    LeObjectRef library_ref(LeLibraryId id)
    {
        return LeObjectRef{.kind = LE_OBJECT_KIND_LIBRARY, .index = id.index, .generation = id.generation};
    }
}

TEST_F(StringLifetime, NamesSurviveRenameGrowthAndDelete)
{
    const LeLibraryId library = le_create_library(handle, "lib");
    const LeDesignId design = le_create_design(handle, library, "d");
    const char *design_name = le_design_name_by_id(handle, design);
    const char *library_name = le_library_name_by_id(handle, library);
    const LeLibraryInfo library_info = le_library_at(handle, 0);
    const LeDesignInfo design_info = le_library_design_at(handle, 0, 0);
    ASSERT_STREQ(design_name, "d");
    ASSERT_STREQ(library_name, "lib");

    ASSERT_EQ(le_update_design(handle, design, 0, LeLibraryId{}, kLongName.c_str()), 0);
    ASSERT_EQ(le_update_library(handle, library, (kLongName + "_lib").c_str()), 0);
    for (int i = 0; i < 1000; ++i)
        le_create_library(handle, ("grow" + std::to_string(i)).c_str());
    ASSERT_EQ(le_delete_library(handle, library), 0);

    EXPECT_STREQ(design_name, "d");
    EXPECT_STREQ(library_name, "lib");
    EXPECT_STREQ(library_info.name, "lib");
    EXPECT_STREQ(design_info.name, "d");
}

TEST_F(StringLifetime, AnotherThreadsCallsLeaveThisThreadsStringAlone)
{
    const LeLibraryId library = le_create_library(handle, "lib");
    const LeDesignId first = le_create_design(handle, library, "first");
    const LeDesignId second = le_create_design(handle, library, "second");
    const char *name = le_design_name_by_id(handle, first);

    std::thread([&]
                {
                    for (int i = 0; i < 100; ++i)
                        EXPECT_STREQ(le_design_name_by_id(handle, second), "second");
                })
        .join();

    EXPECT_STREQ(name, "first");
}

TEST_F(StringLifetime, LayerRowNameSurvivesALayerRowRebuild)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
    LeLayerRow m1{};
    for (int32_t i = 0; i < le_layer_count(handle) && !m1.name; ++i)
        if (const LeLayerRow row = le_layer_at(handle, i); row.name && std::strcmp(row.name, "M1") == 0)
            m1 = row;
    ASSERT_TRUE(m1.name);

    ASSERT_EQ(le_set_layer_color(handle, "M1", 1, 2, 3), 0);
    le_reset_layer_color(handle, "M1");
    ASSERT_EQ(le_read_lef(handle, fixture_path("many_routing_layers.lef").c_str(), "many"), 0);
    ASSERT_GT(le_layer_count(handle), 0);

    EXPECT_STREQ(m1.name, "M1");
}

TEST_F(StringLifetime, PropertyRowSurvivesAnEditAndTheNextCallSeesIt)
{
    const LeLibraryId library = le_create_library(handle, "lib");
    const LeProperty before = find_property(library_ref(library), "name");
    ASSERT_STREQ(before.string_value, "lib");

    ASSERT_EQ(le_update_library(handle, library, kLongName.c_str()), 0);

    EXPECT_STREQ(before.string_value, "lib");
    EXPECT_STREQ(find_property(library_ref(library), "name").string_value, kLongName.c_str());

    // The generated per-class table behaves the same way.
    LeProperty generated{};
    for (int32_t i = 0; i < le_library_property_count(handle, library); ++i)
        if (const LeProperty row = le_library_property_at(handle, library, i); row.name && std::strcmp(row.name, "name") == 0)
            generated = row;
    ASSERT_STREQ(generated.string_value, kLongName.c_str());
    ASSERT_EQ(le_update_library(handle, library, "short"), 0);
    EXPECT_STREQ(generated.string_value, kLongName.c_str());
    EXPECT_STREQ(le_library_property_path(handle, library, ".name").string_value, "short");
}

TEST_F(StringLifetime, PropertyTablesArePerThread)
{
    const LeLibraryId mine = le_create_library(handle, "mine");
    const LeLibraryId theirs = le_create_library(handle, "theirs");
    const LeProperty row = find_property(library_ref(mine), "name");
    ASSERT_STREQ(row.string_value, "mine");

    std::thread([&]
                {
                    EXPECT_STREQ(find_property(library_ref(theirs), "name").string_value, "theirs");
                    EXPECT_STREQ(le_library_property_path(handle, theirs, ".name").string_value, "theirs");
                })
        .join();

    EXPECT_STREQ(row.string_value, "mine");
}

TEST_F(StringLifetime, PropertyPathFailureIsPerThread)
{
    const LeLibraryId library = le_create_library(handle, "lib");
    le_library_property_path(handle, library, "no_such_field");
    ASSERT_EQ(le_property_path_failed(handle), 1);

    std::thread([&]
                { EXPECT_EQ(le_property_path_failed(handle), 0); })
        .join();
    EXPECT_EQ(le_property_path_failed(handle), 1);
}

TEST_F(StringLifetime, CommandHistoryReadsWhileAnotherThreadRecordsCommands)
{
    std::atomic<bool> done{false};
    std::thread reader([&]
                       {
                           while (!done)
                           {
                               le_can_undo(handle);
                               le_can_redo(handle);
                               const int32_t count = le_command_history_count(handle);
                               if (count > 0)
                               {
                                   const char *command = le_command_history_at(handle, count - 1);
                                   ASSERT_NE(command, nullptr);
                                   EXPECT_EQ(std::string(command).rfind("create_library ", 0), 0u) << command;
                               }
                           }
                       });
    for (int i = 0; i < 300; ++i)
    {
        const std::string command = "create_library lib" + std::to_string(i);
        le_begin_command(handle, command.c_str());
        le_create_library(handle, ("lib" + std::to_string(i)).c_str());
        le_end_command(handle, 1);
    }
    done = true;
    reader.join();

    ASSERT_EQ(le_command_history_count(handle), 300);
    EXPECT_STREQ(le_command_history_at(handle, 299), "create_library lib299");
}
