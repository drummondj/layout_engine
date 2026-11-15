#include "views.hpp"
#include "../../core/themes.hpp"
#include "../../test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

// The Layers panel, view options and settings through the C++ views.

namespace
{
    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    void read_testcell(le::Session &session) { ASSERT_TRUE(session.write().read_lef(fixture_path("testcell.lef"), "testcell")); }

    bool has_row(const std::vector<le::LayerRow> &rows, const std::string &name)
    {
        return std::ranges::any_of(rows, [&](const le::LayerRow &row)
                                   { return row.name == name; });
    }
}

TEST(ViewOptions, HierarchyDepthRoundTrips)
{
    le::Session session;
    session.write().set_hierarchy_depth(3);
    EXPECT_EQ(session.read().hierarchy_depth(), 3);
}

TEST(ViewOptions, LayerRowsAreTheTechnologysLayersAndPseudoRows)
{
    le::Session session;
    read_testcell(session);
    const std::vector<le::LayerRow> rows = session.read().layer_rows();
    const auto m1 = std::ranges::find(rows, std::string("M1"), &le::LayerRow::name);
    ASSERT_NE(m1, rows.end());
    EXPECT_TRUE(m1->has_physical_layer);
    EXPECT_TRUE(std::ranges::any_of(rows, [](const le::LayerRow &row)
                                    { return !row.has_physical_layer; }));
    const std::vector<le::ViewLayerPurpose> purposes = session.read().purposes();
    EXPECT_NE(std::ranges::find(purposes, le::ViewLayerPurpose::TERMINAL), purposes.end());
}

TEST(ViewOptions, ALayerCreatedSinceTheLastRebuildIsAlreadyARow)
{
    le::Session session;
    read_testcell(session);
    ASSERT_FALSE(has_row(session.read().layer_rows(), "M2"));
    {
        le::WriteView view = session.write();
        ASSERT_TRUE(view.create_layer({.technology = view.current_technology(), .name = "M2", .type = "ROUTING"}));
    }
    EXPECT_TRUE(has_row(session.read().layer_rows(), "M2"));
}

TEST(ViewOptions, LayerAndPurposeVisibilityAndSelectabilityRoundTrip)
{
    le::Session session;
    read_testcell(session);
    {
        le::WriteView view = session.write();
        view.set_layer_visible("M1", false);
        view.set_layer_selectable("M1", false);
        view.set_purpose_visible(le::ViewLayerPurpose::ROUTE, false);
        view.set_purpose_selectable(le::ViewLayerPurpose::PLACEMENT, false);
    }
    const le::ReadView view = session.read();
    EXPECT_FALSE(view.layer_visible("M1"));
    EXPECT_FALSE(view.layer_selectable("M1"));
    EXPECT_FALSE(view.purpose_visible(le::ViewLayerPurpose::ROUTE));
    EXPECT_TRUE(view.purpose_selectable(le::ViewLayerPurpose::ROUTE));
    EXPECT_FALSE(view.purpose_selectable(le::ViewLayerPurpose::PLACEMENT));
    EXPECT_TRUE(view.layer_visible("UNKNOWN"));
}

TEST(ViewOptions, ObjectFiltersHideAndUnselectValues)
{
    le::Session session;
    {
        const le::ReadView view = session.read();
        const std::span<const std::string> statuses = view.object_filter_values(le::ObjectFilter::PLACEMENT_STATUS);
        EXPECT_NE(std::ranges::find(statuses, std::string("FIXED")), statuses.end());
        EXPECT_FALSE(view.object_filter_values(le::ObjectFilter::ROUTE_USE).empty());
    }
    {
        le::WriteView view = session.write();
        view.set_object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "FIXED", false);
        view.set_object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER", false);
    }
    const le::ReadView view = session.read();
    EXPECT_FALSE(view.object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "FIXED"));
    EXPECT_TRUE(view.object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "PLACED"));
    EXPECT_FALSE(view.object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER"));
    EXPECT_TRUE(view.object_filter_visible(le::ObjectFilter::ROUTE_USE, "POWER"));
}

TEST(ViewOptions, TheViewportIsClampedTo8192)
{
    le::Session session;
    session.write().set_viewport_size(100000, 300);
    EXPECT_EQ(session.read().view().width_px, 8192);
    EXPECT_EQ(session.read().view().height_px, 300);
}

TEST(ViewOptions, DisplayPointsMirrorWithTheView)
{
    le::Session session;
    const std::array<double, 2> plain = session.read().display_point(le::Point{100, 200});
    session.write().set_view_flip(le::ViewFlip::HORIZONTAL);
    const std::array<double, 2> mirrored = session.read().display_point(le::Point{100, 200});
    EXPECT_EQ(mirrored[0], -plain[0]);
    EXPECT_EQ(mirrored[1], plain[1]);
}

TEST(Settings, ValuesRoundTrip)
{
    le::Session session;
    {
        le::WriteView view = session.write();
        view.set_max_concurrency(1); // at least 2
        view.set_flightline_max_fanout(7);
        view.set_flightline_max_fanout(-1); // ignored
        view.set_confirm_overwrite(false);
        view.set_ruler_label_size(13.0);
        view.set_label_min_size(5.0);
        view.set_label_max_size(40.0);
        view.set_undo_limit(12);
    }
    const le::ReadView view = session.read();
    EXPECT_EQ(view.max_concurrency(), 2);
    EXPECT_EQ(view.flightline_max_fanout(), 7);
    EXPECT_FALSE(view.confirm_overwrite());
    EXPECT_EQ(view.ruler_label_size(), 13.0);
    EXPECT_EQ(view.label_min_size(), 5.0);
    EXPECT_EQ(view.label_max_size(), 40.0);
    EXPECT_EQ(view.undo_limit(), 12u);
}

TEST(Settings, ThemesArePickedByName)
{
    le::Session session;
    EXPECT_FALSE(session.write().set_theme("no such theme"));
    EXPECT_EQ(session.read().theme(), 0u);
    ASSERT_GT(le::kThemes.size(), 1u);
    EXPECT_TRUE(session.write().set_theme(le::kThemes[1].name));
    EXPECT_EQ(session.read().theme(), 1u);
}

TEST(Settings, LayerColorsCanBePickedAndReset)
{
    le::Session session;
    read_testcell(session);
    const std::optional<le::Color> palette = session.read().layer_color("M1");
    ASSERT_TRUE(palette.has_value());
    EXPECT_EQ(session.read().layer_color("UNKNOWN"), std::nullopt);

    session.write().set_layer_color("M1", le::Color{.r = 1, .g = 2, .b = 3, .a = 0});
    EXPECT_EQ(session.read().layer_color("M1"), (le::Color{.r = 1, .g = 2, .b = 3, .a = 255}));
    const std::vector<le::LayerRow> rows = session.read().layer_rows();
    EXPECT_EQ(std::ranges::find(rows, std::string("M1"), &le::LayerRow::name)->color, (le::Color{.r = 1, .g = 2, .b = 3, .a = 255}));

    session.write().reset_layer_color("M1");
    EXPECT_EQ(session.read().layer_color("M1"), palette);
}

TEST(Settings, AGridSetInMicronsBeforeATechnologyWaitsForIt)
{
    le::Session session;
    session.write().set_grid_spacing_um(0.5, 5.0);
    EXPECT_EQ(session.read().grid_spacing_um(false), 0.5);
    EXPECT_EQ(session.read().grid_spacing_um(true), 5.0);

    read_testcell(session); // 1000 dbu per micron
    EXPECT_EQ(session.read().minor_grid_spacing(), 500);
    EXPECT_EQ(session.read().major_grid_spacing(), 5000);

    {
        le::WriteView view = session.write();
        view.set_minor_grid_spacing(250);
        view.set_major_grid_spacing(2500);
    }
    EXPECT_EQ(session.read().grid_spacing_um(false), 0.25);
    EXPECT_EQ(session.read().manufacturing_grid(), std::nullopt); // testcell.lef has none
}

TEST(Settings, SaveAndLoadRoundTripAndTrackUnsavedChanges)
{
    const std::string path = le::test::scratch_path("view_options_test/settings.json");
    le::Session first;
    EXPECT_FALSE(first.read().has_unsaved_settings());
    {
        le::WriteView view = first.write();
        view.set_flightline_max_fanout(9);
        view.set_hierarchy_depth(2);
        view.set_theme(le::kThemes[1].name);
    }
    EXPECT_TRUE(first.read().has_unsaved_settings());
    ASSERT_TRUE(first.write().save_settings(path));
    EXPECT_FALSE(first.read().has_unsaved_settings());

    le::Session second;
    ASSERT_TRUE(second.write().load_settings(path));
    EXPECT_EQ(second.read().flightline_max_fanout(), 9);
    EXPECT_EQ(second.read().theme(), 1u);
    EXPECT_FALSE(second.read().has_unsaved_settings());
}

TEST(Settings, LoadingAMissingFileSaysWhichFile)
{
    le::Session session;
    const std::string path = le::test::scratch_path("view_options_test/missing.json");
    const std::expected<void, std::string> loaded = session.write().load_settings(path);
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().find(path), std::string::npos);
}

TEST(Settings, TheDefaultPathIsUnderHome)
{
    const std::string path = le::default_settings_path();
    ASSERT_FALSE(path.empty());
    EXPECT_NE(path.find(".layout_engine"), std::string::npos);
    EXPECT_TRUE(path.ends_with("settings.json"));
}
