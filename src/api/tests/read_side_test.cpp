#include "views.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>

// Searching, properties, parents and children through ReadView.

namespace
{
    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct ReadSideFixture : public ::testing::Test
    {
        void SetUp() override
        {
            le::WriteView view = session.write();
            ASSERT_TRUE(view.read_lef(fixture_path("testcell.lef"), "testcell"));
            library = view.root().get_library_ids().front();
            cell = view.root().get_design_ids().front();
            abstract = view.root().get_design_abstract(cell);
        }

        le::Session session;
        le::LibraryId library;
        le::DesignId cell;
        le::AbstractId abstract;
    };

    template <typename Id>
    bool contains(const std::vector<Id> &ids, Id id)
    {
        return std::ranges::find(ids, id) != ids.end();
    }
}

TEST_F(ReadSideFixture, TheDefaultScopeIsTheCurrentObjects)
{
    const le::ReadView view = session.read();
    const auto layers = view.get_layers(); // the current Technology's
    ASSERT_TRUE(layers) << layers.error();
    ASSERT_EQ(layers->size(), 1u);
    EXPECT_EQ(view.root().get_layer((*layers)[0])->name, "M1");
}

TEST_F(ReadSideFixture, NamesMatchAsGlobs)
{
    const le::ReadView view = session.read();
    EXPECT_EQ(view.get_designs({.of_library = library, .name = "TEST*"})->size(), 1u);
    EXPECT_TRUE(view.get_designs({.of_library = library, .name = "NOPE*"})->empty());
}

TEST_F(ReadSideFixture, ABadFilterIsAnErrorWithItsReason)
{
    const le::ReadView view = session.read();
    const auto unparsable = view.get_layers({.filter = ".name =="});
    ASSERT_FALSE(unparsable);
    EXPECT_FALSE(unparsable.error().empty());

    const auto unknown = view.get_layers({.filter = ".no_such_field == 1"});
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().find("no_such_field"), std::string::npos);
}

TEST_F(ReadSideFixture, AFilterNarrowsTheResult)
{
    const le::ReadView view = session.read();
    const auto m1 = view.get_layers({.filter = ".name == M1"});
    ASSERT_TRUE(m1) << m1.error();
    EXPECT_EQ(m1->size(), 1u);
    const auto wide = view.get_layers({.filter = ".width > 1.5"}); // microns
    ASSERT_TRUE(wide) << wide.error();
    EXPECT_TRUE(wide->empty());
}

TEST_F(ReadSideFixture, PropertiesAndParents)
{
    const le::ReadView view = session.read();
    const std::vector<le::PropertyValue> properties = view.properties(cell);
    const auto name = std::ranges::find(properties, std::string("name"), &le::PropertyValue::name);
    ASSERT_NE(name, properties.end());
    EXPECT_EQ(name->string_value, "TESTCELL");

    EXPECT_EQ(view.parent(cell), std::optional<le::ObjectId>(library));
    EXPECT_EQ(view.parent(library), std::nullopt);
    EXPECT_EQ(le::object_kind_name(cell), "design");
    EXPECT_EQ(le::object_kind_name(le::ObjectId(le::TerminalPortId{})), "terminal_port");
}

TEST_F(ReadSideFixture, ChildrenFollowThePropertyViewer)
{
    const le::ReadView view = session.read();
    EXPECT_TRUE(contains(view.children(library), le::ObjectId(cell)));
    EXPECT_EQ(view.children(cell), std::vector<le::ObjectId>{le::ObjectId(abstract)});

    const std::vector<le::ObjectId> under_abstract = view.children(abstract);
    ASSERT_FALSE(under_abstract.empty());
    EXPECT_TRUE(std::ranges::any_of(under_abstract, [](const le::ObjectId &id)
                                    { return std::holds_alternative<le::TerminalId>(id); }));
    EXPECT_TRUE(view.children(view.current_technology()).empty()); // not a Property Viewer parent
}

TEST_F(ReadSideFixture, SearchesRunTogetherUnderSharedLocks)
{
    const le::ReadView held = session.read();
    std::future<size_t> other = std::async(std::launch::async, [&]
                                           { return session.read().get_layers()->size(); });
    ASSERT_EQ(other.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(other.get(), 1u);
    EXPECT_EQ(held.get_layers()->size(), 1u);
}
