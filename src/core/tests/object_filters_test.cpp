#include "core/object_filters.hpp"

#include <gtest/gtest.h>

using namespace le;

namespace
{
    struct ObjectFiltersFixture : ::testing::Test
    {
        Root root;
        DesignId inv;
        DesignId filler;
        DesignId untyped;
        DesignId block;

        void SetUp() override
        {
            const LibraryId library = root.create_library(LibraryData{.name = "LIB"});
            inv = root.create_design(DesignData{.library = library, .name = "INV"});
            filler = root.create_design(DesignData{.library = library, .name = "FILL"});
            untyped = root.create_design(DesignData{.library = library, .name = "ODD"});
            block = root.create_design(DesignData{.library = library, .name = "TOP"}); // no Abstract
            root.create_abstract(AbstractData{.design = inv, .type = "CORE"});
            root.create_abstract(AbstractData{.design = filler, .type = "core spacer"});
            root.create_abstract(AbstractData{.design = untyped});
        }
    };
}

TEST(ObjectFilters, FilterValuesAreUpperCaseAndUnsetWhenMissingOrEmpty)
{
    EXPECT_EQ(to_filter_value(std::string("power")), "POWER");
    EXPECT_EQ(to_filter_value(std::nullopt), kUnsetFilterValue);
    EXPECT_EQ(to_filter_value(std::string()), kUnsetFilterValue);
}

TEST_F(ObjectFiltersFixture, PlacementTypeComesFromTheReferenceDesignsAbstract)
{
    EXPECT_EQ(placement_type(root, inv), "CORE");
    EXPECT_EQ(placement_type(root, filler), "CORE SPACER");
    EXPECT_EQ(placement_type(root, untyped), kUnsetFilterValue);
    EXPECT_EQ(placement_type(root, block), kUnsetFilterValue);
}

TEST_F(ObjectFiltersFixture, PlacementTypeValuesAreDistinctSortedWithUnsetLast)
{
    EXPECT_EQ(placement_type_values(root), (std::vector<std::string>{"CORE", "CORE SPACER", "UNSET"}));
}

TEST_F(ObjectFiltersFixture, DesignsOfPlacementTypesMatchesWholeTypeStrings)
{
    const std::unordered_set<DesignId> designs = designs_of_placement_types(root, {"CORE", "UNSET"});
    EXPECT_TRUE(designs.contains(inv));
    EXPECT_FALSE(designs.contains(filler)); // "CORE SPACER" isn't "CORE"
    EXPECT_TRUE(designs.contains(untyped));
    EXPECT_TRUE(designs.contains(block));
    EXPECT_TRUE(designs_of_placement_types(root, {}).empty());
}

TEST(ObjectFilters, RouteUsesDefaultToSignalAndHaveNoUnsetValue)
{
    const std::vector<std::string> &values = route_use_values();
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front(), "SIGNAL");
    EXPECT_EQ(std::ranges::find(values, std::string(kUnsetFilterValue)), values.end());
    EXPECT_EQ(route_use(RouteData{.use = std::string("clock")}), "CLOCK");
    EXPECT_EQ(route_use(RouteData{}), "SIGNAL");
    EXPECT_EQ(route_use(RouteData{.use = std::string()}), "SIGNAL");
}
