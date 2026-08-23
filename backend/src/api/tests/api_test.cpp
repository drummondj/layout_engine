#include "../api.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
    }

    // A moderately-sized (not stress_data.hpp's full 1M-shape) generated
    // LEF - one PIN per shape, matching stress_data.hpp's "fresh LAYER
    // before every geometry item" trick so shapes_from_parser finalizes
    // many separate Shape entries - giving Pipeline::filter_by_layer_
    // visibility's grouping loop (the exact std::map mutation that raced
    // in the reported crash) enough iterations per call to take
    // measurable time, without making this test itself slow. Written to
    // a scratch temp file rather than a checked-in fixture, since it's
    // generated, not hand-authored.
    std::string generate_concurrency_stress_lef(int shape_count)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "le_concurrency_stress.lef";

        std::ofstream out(path, std::ios::trunc);
        out << "VERSION 5.8 ;\nBUSBITCHARS \"<>\" ;\nDIVIDERCHAR \"/\" ;\n\n";
        out << "UNITS\n   DATABASE MICRONS 1000 ;\nEND UNITS\n\n";
        out << "LAYER M1\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION HORIZONTAL ;\nEND M1\n\n";
        out << "MACRO CONCURRENCYSTRESS\n   CLASS CORE ;\n   SIZE 100000 BY 100000 ;\n\n";

        for (int i = 0; i < shape_count; ++i)
        {
            const double x = (i % 1000) * 100.0;
            const double y = (i / 1000) * 100.0;
            out << "   PIN P" << i << "\n      DIRECTION INPUT ;\n      PORT\n         LAYER M1 ;\n"
                << "         RECT " << x << " " << y << " " << (x + 1.0) << " " << (y + 1.0) << " ;\n      END\n   END P" << i << "\n";
        }

        out << "END CONCURRENCYSTRESS\n";
        return path.string();
    }

    // ROUTING layers (e.g. the pin's M1) render with a tiled FillPattern
    // (diagonal stripes) rather than a flat fill - unlike a flat fill, a
    // single fixed pixel can land in a transparent gap by coincidence of
    // the pattern's phase. Scans a region instead of trusting one point.
    bool region_has_opaque_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[3] > 0)
                    return true;
            }
        return false;
    }

    // The mouse-snap cursor box is drawn pure opaque red (see
    // Renderer::kCursorBoxColor) - distinct from the grid's gray/white
    // dots and any layer fill color, so "clearly red-dominant and opaque"
    // is a reliable way to detect it without depending on exact stroke
    // antialiasing.
    bool region_has_red_cursor_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] < 50 && p[2] < 50 && p[3] > 200)
                    return true;
            }
        return false;
    }

    // The hover outline is drawn pure opaque yellow (see
    // Renderer::kHoverOutlineColor) - distinct from the cursor box (red),
    // origin marker (amber), and grid (gray/white), so "clearly
    // yellow-dominant and opaque" reliably detects it without depending
    // on exact stroke antialiasing.
    bool region_has_yellow_hover_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] > 200 && p[2] < 50 && p[3] > 200)
                    return true;
            }
        return false;
    }

    // The selection outline is drawn pure opaque white (see
    // Renderer::kSelectionOutlineColor) - distinct from every other
    // overlay/grid color and from every default layer palette color (none
    // of which are pure white), so "R/G/B all near max and opaque"
    // reliably detects it without depending on exact stroke antialiasing.
    bool region_has_white_selection_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 200)
                    return true;
            }
        return false;
    }

    struct ApiFixture : public ::testing::Test
    {
        void SetUp() override { handle = le_create(); }
        void TearDown() override { le_destroy(handle); }

        LeHandle *handle = nullptr;
    };
}

TEST(Api, CreateNeverReturnsNull)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    le_destroy(handle);
}

TEST(Api, DestroyNullHandleDoesNotCrash)
{
    le_destroy(nullptr);
}

TEST_F(ApiFixture, ReadLefWithMissingFileReturnsNonzeroAndLoadsNoDesigns)
{
    EXPECT_NE(le_read_lef(handle, "/does/not/exist.lef"), 0);
    EXPECT_EQ(le_design_count(handle), 0);
}

TEST_F(ApiFixture, ReadLefWithNullHandleOrPathReturnsNonzero)
{
    EXPECT_NE(le_read_lef(nullptr, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_read_lef(handle, nullptr), 0);
}

TEST_F(ApiFixture, ReadLefWithValidFileSucceedsAndPopulatesOneDesign)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_design_count(handle), 1);

    const char *name = le_design_name(handle, 0);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "TESTCELL");
}

// le_message_count/le_message_at (UPDATES.md item 3) - the queue
// le_read_lef drains LEFReader::messages() into, persisting across
// calls on the same handle for the GUI to poll.
TEST_F(ApiFixture, MessageCountAndMessageAtAreZeroAndNullBeforeAnyReadLefCall)
{
    EXPECT_EQ(le_message_count(handle), 0);
    EXPECT_EQ(le_message_at(handle, 0), nullptr);
}

TEST_F(ApiFixture, ReadLefWithMissingFileAppendsAnErrorMessage)
{
    ASSERT_NE(le_read_lef(handle, "/does/not/exist.lef"), 0);
    ASSERT_GT(le_message_count(handle), 0);
    const char *msg = le_message_at(handle, 0);
    ASSERT_NE(msg, nullptr);
    EXPECT_NE(std::string(msg).find("ERROR"), std::string::npos);
}

TEST_F(ApiFixture, ReadLefWithMalformedContentAppendsAnErrorMessage)
{
    ASSERT_NE(le_read_lef(handle, fixture_path("malformed.lef").c_str()), 0);
    ASSERT_GT(le_message_count(handle), 0);
    const char *msg = le_message_at(handle, 0);
    ASSERT_NE(msg, nullptr);
    EXPECT_NE(std::string(msg).find("ERROR"), std::string::npos);
}

TEST_F(ApiFixture, ReadLefWithAWarningProducingFileSucceedsAndAppendsAWarningMessage)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("warning_currentden.lef").c_str()), 0);
    ASSERT_GT(le_message_count(handle), 0);

    bool found_warning = false;
    for (int32_t i = 0; i < le_message_count(handle); ++i)
    {
        const char *msg = le_message_at(handle, i);
        ASSERT_NE(msg, nullptr);
        if (std::string(msg).find("WARNING") != std::string::npos)
            found_warning = true;
    }
    EXPECT_TRUE(found_warning);
}

TEST_F(ApiFixture, SuccessfulReadLefWithNoDiagnosticsAppendsNoMessages)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_message_count(handle), 0);
}

TEST_F(ApiFixture, MessagesAccumulateAcrossMultipleReadLefCalls)
{
    // warning_currentden.lef - a valid LEF that still produces a WARNING
    // (CURRENTDEN is obsolete on any version >= 5.2) - used twice so
    // each read independently contributes at least one message.
    ASSERT_EQ(le_read_lef(handle, fixture_path("warning_currentden.lef").c_str()), 0);
    const int32_t count_after_first = le_message_count(handle);
    ASSERT_GT(count_after_first, 0);
    const std::string first_message = le_message_at(handle, 0);

    ASSERT_EQ(le_read_lef(handle, fixture_path("warning_currentden.lef").c_str()), 0);
    const int32_t count_after_second = le_message_count(handle);
    EXPECT_GT(count_after_second, count_after_first);

    // Earlier entries keep their original text - not overwritten by the
    // second call.
    ASSERT_NE(le_message_at(handle, 0), nullptr);
    EXPECT_EQ(std::string(le_message_at(handle, 0)), first_message);
}

TEST_F(ApiFixture, MessageAtOutOfRangeOrNullHandleReturnsNull)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("warning_currentden.lef").c_str()), 0);
    const int32_t count = le_message_count(handle);
    ASSERT_GT(count, 0);

    EXPECT_EQ(le_message_at(handle, count), nullptr);
    EXPECT_EQ(le_message_at(handle, -1), nullptr);
    EXPECT_EQ(le_message_at(nullptr, 0), nullptr);
    EXPECT_EQ(le_message_count(nullptr), 0);
}

// le_tooltip_message (UPDATES.md item 7.3).
TEST_F(ApiFixture, TooltipMessageReturnsTheSelectModeInstructions)
{
    const char *tooltip = le_tooltip_message(handle);
    ASSERT_NE(tooltip, nullptr);
    EXPECT_NE(std::string(tooltip).find("Left click to select"), std::string::npos);
}

TEST_F(ApiFixture, TooltipMessageWithNullHandleReturnsNull)
{
    EXPECT_EQ(le_tooltip_message(nullptr), nullptr);
}

TEST_F(ApiFixture, TooltipMessageReflectsEditMode)
{
    le_set_mode(handle, LE_MODE_EDIT);
    const char *tooltip = le_tooltip_message(handle);
    ASSERT_NE(tooltip, nullptr);
    EXPECT_EQ(std::string(tooltip).find("Left click to select"), std::string::npos);
}

TEST_F(ApiFixture, TooltipMessageReflectsRulerMode)
{
    le_set_mode(handle, LE_MODE_RULER);
    const char *tooltip = le_tooltip_message(handle);
    ASSERT_NE(tooltip, nullptr);
    EXPECT_NE(std::string(tooltip).find("Esc"), std::string::npos);
}

// le_get_mode/le_set_mode (UPDATES.md item 11).
TEST_F(ApiFixture, GetModeDefaultsToSelectMode)
{
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);
}

TEST_F(ApiFixture, SetModeThenGetModeRoundTrips)
{
    le_set_mode(handle, LE_MODE_EDIT);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_EDIT);

    le_set_mode(handle, LE_MODE_SELECT);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);
}

TEST_F(ApiFixture, GetModeWithNullHandleReturnsSelectMode)
{
    EXPECT_EQ(le_get_mode(nullptr), LE_MODE_SELECT);
}

TEST_F(ApiFixture, SetModeWithNullHandleDoesNotCrash)
{
    le_set_mode(nullptr, LE_MODE_EDIT);
}

TEST_F(ApiFixture, SetModeToRulerDoesNotEagerlyCreateARuler)
{
    le_set_mode(handle, LE_MODE_RULER);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_RULER);
    // Rulers start lazily on the first click (Scene::add_ruler_point) -
    // entering Ruler mode alone doesn't create an empty one.
    EXPECT_EQ(le_ruler_count(handle), 0);
}

TEST_F(ApiFixture, LeRulerCountWithNullHandleReturnsZero)
{
    EXPECT_EQ(le_ruler_count(nullptr), 0);
}

TEST_F(ApiFixture, LeRulerPointCountWithNullHandleOrOutOfRangeReturnsZero)
{
    le_set_mode(handle, LE_MODE_RULER);
    EXPECT_EQ(le_ruler_point_count(nullptr, 0), 0);
    EXPECT_EQ(le_ruler_point_count(handle, -1), 0);
    EXPECT_EQ(le_ruler_point_count(handle, 5), 0);
}

TEST_F(ApiFixture, LeRulerPointAtWithNullHandleOrOutOfRangeReturnsZeroPoint)
{
    le_set_mode(handle, LE_MODE_RULER);
    const LeRulerPoint p1 = le_ruler_point_at(nullptr, 0, 0);
    EXPECT_DOUBLE_EQ(p1.x_um, 0.0);
    EXPECT_DOUBLE_EQ(p1.y_um, 0.0);

    const LeRulerPoint p2 = le_ruler_point_at(handle, 0, 0); // no points yet
    EXPECT_DOUBLE_EQ(p2.x_um, 0.0);
    EXPECT_DOUBLE_EQ(p2.y_um, 0.0);
}

TEST_F(ApiFixture, LeFinishRulerWithNullHandleDoesNotCrash)
{
    le_finish_ruler(nullptr);
}

TEST_F(ApiFixture, LeClearRulersWithNullHandleDoesNotCrash)
{
    le_clear_rulers(nullptr);
}

TEST_F(ApiFixture, DesignCountAndNameAreZeroOrNullForNullHandle)
{
    EXPECT_EQ(le_design_count(nullptr), 0);
    EXPECT_EQ(le_design_name(nullptr, 0), nullptr);
}

TEST_F(ApiFixture, DesignNameOutOfRangeReturnsNull)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_design_name(handle, 1), nullptr);
    EXPECT_EQ(le_design_name(handle, -1), nullptr);
}

TEST_F(ApiFixture, SetCurrentDesignOutOfRangeReturnsNonzero)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_set_current_design_abstract(handle, 1), 0);
    EXPECT_NE(le_set_current_design_abstract(handle, -1), 0);
    EXPECT_NE(le_set_current_design_abstract(nullptr, 0), 0);
}

TEST_F(ApiFixture, SetCurrentDesignValidIndexSucceeds)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_set_current_design_abstract(handle, 0), 0);
}

TEST_F(ApiFixture, RenderPixelBufferWithNullHandleReturnsAllZero)
{
    LePixelBuffer buffer = le_render_pixel_buffer(nullptr);
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
    EXPECT_EQ(buffer.row_bytes, 0);
}

TEST_F(ApiFixture, RenderPixelBufferBeforeAnySetupDoesNotCrashAndStaysWithinRequestedViewport)
{
    // No LEF loaded, no Design selected, but a real viewport size - the
    // degenerate "nothing to draw yet" case should still produce a valid,
    // correctly-sized buffer rather than crash.
    le_set_viewport_size(handle, 50, 50);
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_NE(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 50);
    EXPECT_EQ(buffer.height, 50);
    EXPECT_GE(buffer.row_bytes, 50 * 4);
}

TEST_F(ApiFixture, RenderPixelBufferWithZeroSizedViewportDoesNotCrash)
{
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
}

TEST_F(ApiFixture, RenderPixelBufferProducesTheRequestedDimensions)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);

    // Scene starts at scale 1.0 / pan (0, 0) - le_zoom to scale 10.0 (10
    // px/dbu-micron-ish, matches DATABASE MICRONS 1000 -> 1000 dbu/micron)
    // anchored at image pixel (0, 200) (bottom-left corner, i.e. dbu (0,
    // 0) at the starting pan/scale) keeps pan pinned at (0, 0) exactly.
    le_zoom(handle, 9.0, 0, 200);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 200);
    EXPECT_EQ(buffer.height, 200);
}

TEST_F(ApiFixture, SubPixelShapeRendersAsASinglePixelDotAndIsNotSelectable)
{
    // UPDATES.md item 6: a shape too small to render normally should still
    // show as a single-pixel dot instead of silently vanishing, but that
    // dot must not be clickable - see TinyShapeDot's own comment for why
    // Pipeline::hit_test_point/hit_test_rect never see it.
    ASSERT_EQ(le_read_lef(handle, fixture_path("tiny_shape.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);

    // Scene starts at scale 1.0 / pan (0, 0). PIN A's RECT is exactly 1x1
    // dbu - at scale 1.0 that's exactly at (not below) the sub-pixel
    // threshold, so it renders normally there. Zooming out to scale 0.5,
    // anchored at pixel (0, 100) (dbu (0, 0) at the starting pan/scale -
    // see RenderPixelBufferProducesTheRequestedDimensions's own comment
    // for why this anchor keeps pan pinned exactly at (0, 0)), doubles the
    // threshold to 2 dbu, putting the shape below it.
    le_zoom(handle, -0.5, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);

    // TinyShapeDot::location is the shape's bbox center: dbu (10, 10)
    // (integer-division midpoint of (10,10)-(11,11)). Pre-flip pixel =
    // dbu * scale = (5, 5); rasterize_tiny_shapes_frame's whole-canvas
    // Y-flip maps that to screen pixel (5, height - 5) = (5, 95).
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 3, 93, 7, 97));

    le_mouse_down(handle, 5, 95);
    le_mouse_up(handle, 5, 95);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, FitSceneWithNullHandleDoesNotCrash)
{
    le_fit_scene(nullptr, 10);
}

TEST_F(ApiFixture, FitSceneWithNoDesignSelectedFallsBackToDefaultScaleAndPan)
{
    le_set_viewport_size(handle, 100, 100);
    le_fit_scene(handle, 10);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
}

TEST_F(ApiFixture, FitSceneFillsTheViewportWithThePinVisible)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);
    le_fit_scene(handle, 10);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);

    // The macro's content should now fill most of the 200x200 viewport -
    // scan a block around the center, which sits inside both the 10x10
    // micron macro boundary and PIN A's (2,2)-(8,8) micron RECT. PIN A is
    // on M1 (a ROUTING layer), so its fill is a tiled diagonal-stripe
    // pattern rather than a flat fill - a single fixed pixel could land in
    // a transparent gap by coincidence of the pattern's phase.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, FitSceneInLayoutViewFramesTheDiereaNotTheOrigin)
{
    // Regression test: fit_scene_unlocked used to always call
    // generate_shapes against scene.current_abstract(), which is invalid
    // in a Layout view (Phase C's own convention: the two "current view"
    // trackers are mutually exclusive) - generate_shapes on an invalid
    // AbstractId returns nothing, so fit_to_content(nullopt, ...) reset to
    // scale=1.0/pan={0,0} instead of framing the Layout's own content.
    // Places the diearea (and the placement whose pin should end up
    // visible) far from the origin - with the old bug, scale=1/pan={0,0}
    // against a 100x100px viewport can't reach dbu coordinates in the
    // millions no matter what, so this is a direct, robust proof the fix
    // actually frames the real content rather than replicating fit's own
    // pan/scale math by hand (no direct pan/scale getter exists in the
    // C API to assert against directly).
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeDesignInfo testcell_design = le_library_design_at(handle, 0, 0);

    const LeLibraryId top_library = le_create_library(handle, "TOPLIB");
    const LeDesignId top_design = le_create_design(handle, top_library, "TOP");
    const LeLayoutId top_layout = le_create_layout(handle, top_design);

    const double diearea_um[4] = {1000.0, 1000.0, 1100.0, 1100.0};
    le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, top_layout, LeAbstractId{.index = UINT32_MAX, .generation = 0}, LeLayerId{.index = UINT32_MAX, .generation = 0}, "BOUNDARY", 0, nullptr, 0, 0, nullptr, 0, 1, diearea_um, 4, 0, 0.0, 0, 0.0, 0);

    // TESTCELL placed at (1010,1010)um - well inside the diearea above -
    // so its own PIN A rect ((2,2)-(8,8)um local) lands at world
    // (1012,1012)-(1018,1018)um, still far from the origin.
    ASSERT_NE(le_create_placement(handle, top_layout, testcell_design.id, "U1", "PLACED", /*has_location=*/1, /*location_x_um=*/1010.0, /*location_y_um=*/1010.0, "N", /*has_weight=*/0, 0.0, nullptr).index, UINT32_MAX);

    ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
    le_set_hierarchy_depth(handle, 1); // remaining_depth 0 - the placement falls back straight to TESTCELL's own Abstract

    le_set_viewport_size(handle, 100, 100);
    le_fit_scene(handle, 10); // no manual pan/zoom - fit_scene itself must compute a real view

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 0, 0, 100, 100));
}

TEST_F(ApiFixture, LibraryCountAndAtAreZeroOrInvalidForNullHandle)
{
    EXPECT_EQ(le_library_count(nullptr), 0);

    const LeLibraryInfo info = le_library_at(nullptr, 0);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, LibraryAtOutOfRangeReturnsInvalidRow)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeLibraryInfo info = le_library_at(handle, 1);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, EachLefReadCreatesItsOwnLibrary)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_read_lef(handle, fixture_path("othercell.lef").c_str()), 0);

    ASSERT_EQ(le_library_count(handle), 2);

    const LeLibraryInfo lib0 = le_library_at(handle, 0);
    ASSERT_NE(lib0.name, nullptr);
    EXPECT_STREQ(lib0.name, "testcell");
    EXPECT_TRUE(lib0.id.index != UINT32_MAX);

    const LeLibraryInfo lib1 = le_library_at(handle, 1);
    ASSERT_NE(lib1.name, nullptr);
    EXPECT_STREQ(lib1.name, "othercell");
    EXPECT_NE(lib1.id.index, lib0.id.index);
}

TEST_F(ApiFixture, LibraryDesignCountAndAtAreZeroOrInvalidForNullHandleOrBadIndex)
{
    EXPECT_EQ(le_library_design_count(nullptr, 0), 0);

    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_library_design_count(handle, 1), 0);  // out-of-range library
    EXPECT_EQ(le_library_design_count(handle, -1), 0); // negative

    const LeDesignInfo info = le_library_design_at(handle, 1, 0);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.library_id.index, UINT32_MAX);
    EXPECT_EQ(info.abstract_id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, LibraryDesignAtReturnsTheDesignAndItsAbstractId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    ASSERT_EQ(le_library_design_count(handle, 0), 1);

    const LeLibraryInfo library = le_library_at(handle, 0);
    const LeDesignInfo design = le_library_design_at(handle, 0, 0);

    ASSERT_NE(design.name, nullptr);
    EXPECT_STREQ(design.name, "TESTCELL");
    EXPECT_EQ(design.library_id.index, library.id.index);
    EXPECT_EQ(design.library_id.generation, library.id.generation);
    EXPECT_NE(design.id.index, UINT32_MAX);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX); // every read Design gets an Abstract view
}

TEST_F(ApiFixture, SetCurrentDesignByIdWithNullHandleOrUnknownIdReturnsNonzero)
{
    EXPECT_NE(le_set_current_design_abstract_by_id(nullptr, LeDesignId{0, 0}), 0);

    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_set_current_design_abstract_by_id(handle, LeDesignId{UINT32_MAX, 0}), 0);
    EXPECT_NE(le_set_current_design_abstract_by_id(handle, LeDesignId{99, 0}), 0);
}

TEST_F(ApiFixture, SetCurrentDesignByIdAlsoSetsTheGeneratedCurrentAbstract)
{
    // le_set_current_design_abstract_by_id is the shared entry point both a
    // Dart-driven GUI (LeProvider.openDesign) and a TCL script
    // (open_design) select a Design through - both should mean the same
    // thing: get_terminals/get_shapes/etc.'s own default -of-omitted
    // scope (le_current_abstract - the generated has_current_access
    // state, not Scene::current_abstract()) has to move too, not just
    // whatever Scene renders.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeDesignInfo design = le_library_design_at(handle, 0, 0);
    EXPECT_EQ(le_current_abstract(handle).index, UINT32_MAX); // nothing selected yet

    ASSERT_EQ(le_set_current_design_abstract_by_id(handle, design.id), 0);

    const LeAbstractId current = le_current_abstract(handle);
    EXPECT_EQ(current.index, design.abstract_id.index);
    EXPECT_EQ(current.generation, design.abstract_id.generation);
}

TEST_F(ApiFixture, SetCurrentDesignAlsoSetsTheGeneratedCurrentAbstract)
{
    // Same as SetCurrentDesignByIdAlsoSetsTheGeneratedCurrentAbstract
    // above, for the index-addressed sibling entry point.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeDesignInfo design = le_library_design_at(handle, 0, 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    const LeAbstractId current = le_current_abstract(handle);
    EXPECT_EQ(current.index, design.abstract_id.index);
    EXPECT_EQ(current.generation, design.abstract_id.generation);
}

TEST_F(ApiFixture, SetCurrentDesignByIdSelectsTheSameDesignAsSetCurrentDesign)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeDesignInfo design = le_library_design_at(handle, 0, 0);
    ASSERT_EQ(le_set_current_design_abstract_by_id(handle, design.id), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    // PIN A is on M1 (ROUTING), so its fill is a tiled diagonal-stripe
    // pattern - scan the pin's own region rather than trusting one pixel.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect visible, same as le_set_current_design_abstract(handle, 0) would give
}

// --- Layout view / hierarchy depth (Migration Step 3 Phase C) ---

TEST_F(ApiFixture, HierarchyDepthDefaultsToZeroAndRoundTrips)
{
    EXPECT_EQ(le_hierarchy_depth(handle), 0);
    le_set_hierarchy_depth(handle, 3);
    EXPECT_EQ(le_hierarchy_depth(handle), 3);
}

TEST_F(ApiFixture, SetHierarchyDepthRejectsNegativeValues)
{
    le_set_hierarchy_depth(handle, 2);
    ASSERT_EQ(le_hierarchy_depth(handle), 2);
    le_set_hierarchy_depth(handle, -1);
    EXPECT_EQ(le_hierarchy_depth(handle), 2);
}

TEST_F(ApiFixture, HierarchyDepthFunctionsWithNullHandleDoNotCrash)
{
    EXPECT_EQ(le_hierarchy_depth(nullptr), 0);
    le_set_hierarchy_depth(nullptr, 5); // no crash
}

TEST_F(ApiFixture, SetCurrentDesignLayoutRendersThePlacedInstancesOwnContent)
{
    // TOP's own Layout places TESTCELL (read via LEF, real M1 pin
    // geometry - see testcell.lef) at its own local origin with identity
    // orientation, so the placement's world dbu rect exactly matches
    // TESTCELL's own local PIN A rect (2,2)-(8,8) um = (2000,2000)-
    // (8000,8000) dbu - same viewport/zoom setup as
    // SetCurrentDesignByIdSelectsTheSameDesignAsSetCurrentDesign above,
    // so the expected pixel region is identical, letting this test lean
    // on that one's already-proven math instead of re-deriving it.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeDesignInfo testcell_design = le_library_design_at(handle, 0, 0);

    const LeLibraryId top_library = le_create_library(handle, "TOPLIB");
    const LeDesignId top_design = le_create_design(handle, top_library, "TOP");
    const LeLayoutId top_layout = le_create_layout(handle, top_design);
    ASSERT_NE(le_create_placement(handle, top_layout, testcell_design.id, "U1", "PLACED", /*has_location=*/1, /*location_x_um=*/0.0, /*location_y_um=*/0.0, "N", /*has_weight=*/0, 0.0, nullptr).index, UINT32_MAX);

    ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
    le_set_hierarchy_depth(handle, 1); // remaining_depth 0 - the placement falls back straight to TESTCELL's own Abstract

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // same scale=0.01 setup as the Abstract-view test above

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79));
}

TEST_F(ApiFixture, SetCurrentDesignLayoutClearsTheAbstractViewAndViceVersa)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeDesignInfo testcell_design = le_library_design_at(handle, 0, 0);

    const LeLibraryId top_library = le_create_library(handle, "TOPLIB");
    const LeDesignId top_design = le_create_design(handle, top_library, "TOP");
    const LeLayoutId top_layout = le_create_layout(handle, top_design);
    le_create_placement(handle, top_layout, testcell_design.id, "U1", "PLACED", 1, 0.0, 0.0, "N", 0, 0.0, nullptr);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    // Abstract view first - the pin is visible directly.
    ASSERT_EQ(le_set_current_design_abstract_by_id(handle, testcell_design.id), 0);
    LePixelBuffer abstract_buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(abstract_buffer.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(abstract_buffer, 21, 21, 79, 79));

    // Switching to the Layout view must stop rendering the old Abstract
    // (scene.current_abstract() clears) - render_pixel_buffer now takes
    // the InstanceRenderer path entirely instead.
    ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
    le_set_hierarchy_depth(handle, 1);
    LePixelBuffer layout_buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(layout_buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(layout_buffer, 21, 21, 79, 79)); // same pin, now reached via the placement

    // Switching back to the Abstract view must stop rendering the Layout.
    ASSERT_EQ(le_set_current_design_abstract_by_id(handle, testcell_design.id), 0);
    LePixelBuffer back_to_abstract = le_render_pixel_buffer(handle);
    ASSERT_NE(back_to_abstract.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(back_to_abstract, 21, 21, 79, 79));
}

TEST_F(ApiFixture, SetCurrentDesignLayoutWithZeroHierarchyDepthStillRendersOwnPlacement)
{
    // hierarchy_depth defaults to 0 - remaining_depth is still max(0, 0-1)
    // == 0, so a placement still falls back to its own Abstract (0 means
    // "no further recursion into nested Layouts", not "don't render
    // placements at all").
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeDesignInfo testcell_design = le_library_design_at(handle, 0, 0);

    const LeLibraryId top_library = le_create_library(handle, "TOPLIB");
    const LeDesignId top_design = le_create_design(handle, top_library, "TOP");
    const LeLayoutId top_layout = le_create_layout(handle, top_design);
    le_create_placement(handle, top_layout, testcell_design.id, "U1", "PLACED", 1, 0.0, 0.0, "N", 0, 0.0, nullptr);

    ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
    ASSERT_EQ(le_hierarchy_depth(handle), 0); // default, never set

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79));
}

TEST_F(ApiFixture, LayerCountAndAtAreZeroOrInvalidForNullHandleOrNoViewLayerSetYet)
{
    EXPECT_EQ(le_layer_count(nullptr), 0);
    EXPECT_EQ(le_layer_count(handle), 0); // no LEF read yet - no ViewLayerSet built

    const LeLayerRow row = le_layer_at(handle, 0);
    EXPECT_EQ(row.name, nullptr);
}

TEST_F(ApiFixture, LayerAtOutOfRangeReturnsInvalidRow)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeLayerRow row = le_layer_at(handle, 6);
    EXPECT_EQ(row.name, nullptr);
}

TEST_F(ApiFixture, LayerAtIncludesBoundaryAsAnOrdinaryRowAfterEveryPhysicalLayer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    // testcell.lef declares one physical Layer (M1) - the API doesn't
    // special-case BOUNDARY, it's just another row, so the count is
    // M1 + ROW + GCELLGRID + PLACEMENT_BLOCKAGE + REGION + BOUNDARY = 6
    // (Migration Step 2/3 - see view_style.hpp's ViewLayerSet::
    // build_for_technology).
    ASSERT_EQ(le_layer_count(handle), 6);

    const LeLayerRow m1_row = le_layer_at(handle, 0);
    ASSERT_NE(m1_row.name, nullptr);
    EXPECT_STREQ(m1_row.name, "M1");

    // M1 is the first ROUTING layer declared - first slot of the bright
    // ROUTING/CUT palette, red (255, 0, 0) - see view_style.hpp.
    EXPECT_EQ(m1_row.color_r, 255);
    EXPECT_EQ(m1_row.color_g, 0);
    EXPECT_EQ(m1_row.color_b, 0);

    const LeLayerRow boundary_row = le_layer_at(handle, 5);
    ASSERT_NE(boundary_row.name, nullptr);
    EXPECT_STREQ(boundary_row.name, "BOUNDARY");
}

TEST_F(ApiFixture, PurposeCountAndAtAreZeroOrInvalidForNullHandleOrNoViewLayerSetYet)
{
    EXPECT_EQ(le_purpose_count(nullptr), 0);
    EXPECT_EQ(le_purpose_count(handle), 0); // no LEF read yet - no ViewLayerSet built

    EXPECT_EQ(le_purpose_at(handle, 0), -1);
    EXPECT_EQ(le_purpose_at(nullptr, 0), -1);
}

TEST_F(ApiFixture, PurposeAtOutOfRangeReturnsInvalid)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_EQ(le_purpose_at(handle, 10), -1);
    EXPECT_EQ(le_purpose_at(handle, -1), -1);
}

TEST_F(ApiFixture, PurposeAtListsTerminalObstructionThenBoundary)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    // The "columns" axis - row-independent, not scoped to M1 or any other
    // specific layer (see ViewLayerSet::purposes()'s own comment). Migration
    // Step 2 added TRACK/ROUTING_BLOCKAGE (per-Layer, right after
    // TERMINAL/OBSTRUCTION) and ROW/GCELLGRID/PLACEMENT_BLOCKAGE (their own
    // pseudo-rows); Step 3 added ROUTE (per-Layer, right after
    // ROUTING_BLOCKAGE) and REGION (its own pseudo-row) - all before
    // BOUNDARY, which stays last - see view_style.hpp's ViewLayerPurpose
    // declaration order.
    ASSERT_EQ(le_purpose_count(handle), 10);
    EXPECT_EQ(le_purpose_at(handle, 0), 0); // TERMINAL
    EXPECT_EQ(le_purpose_at(handle, 1), 1); // OBSTRUCTION
    EXPECT_EQ(le_purpose_at(handle, 2), 3); // TRACK
    EXPECT_EQ(le_purpose_at(handle, 3), 4); // ROUTING_BLOCKAGE
    EXPECT_EQ(le_purpose_at(handle, 4), 8); // ROUTE
    EXPECT_EQ(le_purpose_at(handle, 5), 5); // ROW
    EXPECT_EQ(le_purpose_at(handle, 6), 6); // GCELLGRID
    EXPECT_EQ(le_purpose_at(handle, 7), 7); // PLACEMENT_BLOCKAGE
    EXPECT_EQ(le_purpose_at(handle, 8), 9); // REGION
    EXPECT_EQ(le_purpose_at(handle, 9), 2); // BOUNDARY
}

TEST_F(ApiFixture, LayerNameVisibilityDefaultsTrueAndRoundTrips)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    // Unknown-to-null-handle/name default matches Scene's own default.
    EXPECT_NE(le_is_layer_name_visible(nullptr, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, nullptr), 0);

    le_set_layer_name_visible(handle, "M1", 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "M1"), 0);
    // A different, never-toggled layer name is unaffected.
    EXPECT_NE(le_is_layer_name_visible(handle, "BOUNDARY"), 0);

    le_set_layer_name_visible(handle, "M1", 1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
}

TEST_F(ApiFixture, ReadLefDefaultsNonRoutingCutLayersToHidden)
{
    // mixed_layer_types.lef: M1 (ROUTING), V1 (CUT), OVERLAP (OVERLAP),
    // SLICE (MASTERSLICE) - UPDATES.md 10 says only ROUTING/CUT/BOUNDARY
    // should default visible.
    ASSERT_EQ(le_read_lef(handle, fixture_path("mixed_layer_types.lef").c_str()), 0);

    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V1"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "OVERLAP"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "SLICE"), 0);
    // BOUNDARY isn't a physical layer (no LayerId of its own), so it's
    // untouched by the new default-hiding pass and stays visible via
    // Scene's own default-true-until-toggled behavior.
    EXPECT_NE(le_is_layer_name_visible(handle, "BOUNDARY"), 0);
}

TEST_F(ApiFixture, ReadLefDefaultHidingOnlyAppliesToNewlyIntroducedLayers)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("mixed_layer_types.lef").c_str()), 0);
    ASSERT_EQ(le_is_layer_name_visible(handle, "OVERLAP"), 0);

    le_set_layer_name_visible(handle, "OVERLAP", 1); // user explicitly reveals it via the layer manager
    ASSERT_NE(le_is_layer_name_visible(handle, "OVERLAP"), 0);

    // A second le_read_lef call (e.g. a macro file with its own inline
    // LAYER declarations) must not re-default OVERLAP back to hidden -
    // only layers newly introduced by *this* read get defaulted (see
    // le_read_lef's own comment). via_pairing.lef doesn't declare an
    // OVERLAP layer at all.
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "OVERLAP"), 0);
}

TEST_F(ApiFixture, SetLayerNameVisibleWithNullHandleOrNullNameDoesNotCrash)
{
    le_set_layer_name_visible(nullptr, "M1", 0);
    le_set_layer_name_visible(handle, nullptr, 0);
}

TEST_F(ApiFixture, DigitKeysToggleTheNthRoutingLayerAndPairTheCutLayerBetweenThem)
{
    // via_pairing.lef: M1 (ROUTING, index 0 -> LE_KEY_1), V1 (CUT, between
    // M1/M2), M2 (ROUTING, index 1 -> LE_KEY_2). No MACRO needed - the
    // Technology's own layer list is populated straight from LAYER
    // statements (see LEFReader::lefrLayerCbkFn), independent of any
    // Library/Design.
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0);

    ASSERT_NE(le_is_layer_name_visible(handle, "M1"), 0); // default-visible
    ASSERT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "V1"), 0);

    le_key_down(handle, LE_KEY_1); // M1 -> hidden
    EXPECT_EQ(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V1"), 0); // M1/M2 no longer both visible

    le_key_down(handle, LE_KEY_2); // M2 -> hidden too (M1 already hidden)
    EXPECT_EQ(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V1"), 0); // still not both visible

    le_key_down(handle, LE_KEY_1); // M1 -> visible again; M2 still hidden
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V1"), 0);

    le_key_down(handle, LE_KEY_2); // M2 -> visible again; now both M1/M2 visible
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V1"), 0); // V1 paired on

    le_key_down(handle, LE_KEY_1); // M1 -> hidden again
    EXPECT_EQ(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V1"), 0); // V1 paired back off
}

TEST_F(ApiFixture, ManualLayerVisibilityDoesNotTriggerViaPairing)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0);

    le_set_layer_name_visible(handle, "M1", 0);
    le_set_layer_name_visible(handle, "M2", 0);
    le_set_layer_name_visible(handle, "V1", 0);
    ASSERT_EQ(le_is_layer_name_visible(handle, "V1"), 0);

    // Manually re-visible-ing both routing layers through the direct API
    // (as a layer-manager click would) must not re-trigger the pairing
    // logic - that's only wired into the LE_KEY_1..LE_KEY_9 path.
    le_set_layer_name_visible(handle, "M1", 1);
    le_set_layer_name_visible(handle, "M2", 1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V1"), 0); // still hidden - no pairing happened
}

TEST_F(ApiFixture, DigitKeyWithNoNthRoutingLayerIsANoOp)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0);

    // Only two ROUTING layers (M1, M2) exist - LE_KEY_3..LE_KEY_9 have no
    // 3rd+ routing layer to toggle.
    le_key_down(handle, LE_KEY_3);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V1"), 0);
}

TEST_F(ApiFixture, DigitKey0TogglesTheTenthRoutingLayer)
{
    // many_routing_layers.lef: M1..M12 ROUTING, with V10 between M10/M11
    // and V11 between M11/M12 - LE_KEY_0 addresses M10 (the 10th ROUTING
    // layer, routing_index 9), unconditional on LE_KEY_CTRL.
    ASSERT_EQ(le_read_lef(handle, fixture_path("many_routing_layers.lef").c_str()), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "M10"), 0); // default-visible

    le_key_down(handle, LE_KEY_0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "M10"), 0);

    le_key_down(handle, LE_KEY_0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M10"), 0);
}

TEST_F(ApiFixture, CtrlPlusDigitKeysToggleTheEleventhAndTwelfthRoutingLayersAndPairTheirCutLayers)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("many_routing_layers.lef").c_str()), 0);

    ASSERT_NE(le_is_layer_name_visible(handle, "M10"), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "M11"), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "M12"), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "V10"), 0);
    ASSERT_NE(le_is_layer_name_visible(handle, "V11"), 0);

    // Without Ctrl held, LE_KEY_1/LE_KEY_2 address M1/M2, not M11/M12 -
    // M10/M11/M12 stay untouched.
    le_key_down(handle, LE_KEY_1);
    le_key_down(handle, LE_KEY_2);
    EXPECT_NE(le_is_layer_name_visible(handle, "M11"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M12"), 0);
    le_key_down(handle, LE_KEY_1); // toggle M1/M2 back off, restoring the fixture to its default state
    le_key_down(handle, LE_KEY_2);

    // Ctrl held: LE_KEY_1 -> M11 (hides it), pairing re-check drops both
    // V10 (M10/M11 no longer both visible) and V11 (M11/M12 no longer
    // both visible).
    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_1);
    EXPECT_EQ(le_is_layer_name_visible(handle, "M11"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V10"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V11"), 0);

    // LE_KEY_1 again (Ctrl still held) -> M11 visible again; both pairs
    // are back to both-visible, so both cut layers reappear.
    le_key_down(handle, LE_KEY_1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M11"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V10"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V11"), 0);

    // LE_KEY_2 (Ctrl held) -> M12 (hides it); only the M11/M12 pair (V11)
    // is affected - M10/M11 (V10) is untouched.
    le_key_down(handle, LE_KEY_2);
    EXPECT_EQ(le_is_layer_name_visible(handle, "M12"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V10"), 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "V11"), 0);
}

TEST_F(ApiFixture, DigitKey0WithNoTenthRoutingLayerIsANoOp)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0); // only M1, M2 ROUTING

    le_key_down(handle, LE_KEY_0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V1"), 0);
}

TEST_F(ApiFixture, CtrlPlusDigitKeyWithNoLayerAtThatPositionIsANoOp)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0); // only M1, M2 ROUTING - no 11th

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, "V1"), 0);
}

TEST_F(ApiFixture, DigitKey0WithNullHandleDoesNotCrash)
{
    le_key_down(nullptr, LE_KEY_0);
}

TEST_F(ApiFixture, DigitKeysWithShiftHeldAreANoOp)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_pairing.lef").c_str()), 0);

    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0); // unchanged

    le_key_down(handle, LE_KEY_0);
    EXPECT_NE(le_is_layer_name_visible(handle, "M2"), 0); // unchanged

    le_key_down(handle, LE_KEY_CTRL); // Ctrl+Shift both held
    le_key_down(handle, LE_KEY_1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0); // still unchanged
}

TEST_F(ApiFixture, DigitKeyWithNoTechnologyReadYetDoesNotCrash)
{
    le_key_down(handle, LE_KEY_1); // no le_read_lef call at all
}

TEST_F(ApiFixture, DigitKeyWithNullHandleDoesNotCrash)
{
    le_key_down(nullptr, LE_KEY_1);
}

TEST_F(ApiFixture, PurposeVisibilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(le_is_purpose_visible(handle, 1), 0); // OBSTRUCTION
    EXPECT_NE(le_is_purpose_visible(nullptr, 1), 0);

    le_set_purpose_visible(handle, 1, 0);
    EXPECT_EQ(le_is_purpose_visible(handle, 1), 0);
    // A different, never-toggled purpose is unaffected.
    EXPECT_NE(le_is_purpose_visible(handle, 0), 0); // TERMINAL

    le_set_purpose_visible(handle, 1, 1);
    EXPECT_NE(le_is_purpose_visible(handle, 1), 0);
}

TEST_F(ApiFixture, SetPurposeVisibleWithNullHandleDoesNotCrash)
{
    le_set_purpose_visible(nullptr, 1, 0);
}

TEST_F(ApiFixture, LayerNameSelectabilityDefaultsTrueAndRoundTrips)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_NE(le_is_layer_name_selectable(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_selectable(nullptr, "M1"), 0);
    EXPECT_NE(le_is_layer_name_selectable(handle, nullptr), 0);

    le_set_layer_name_selectable(handle, "M1", 0);
    EXPECT_EQ(le_is_layer_name_selectable(handle, "M1"), 0);

    le_set_layer_name_selectable(handle, "M1", 1);
    EXPECT_NE(le_is_layer_name_selectable(handle, "M1"), 0);
}

TEST_F(ApiFixture, SetLayerNameSelectableWithNullHandleOrNullNameDoesNotCrash)
{
    le_set_layer_name_selectable(nullptr, "M1", 0);
    le_set_layer_name_selectable(handle, nullptr, 0);
}

TEST_F(ApiFixture, PurposeSelectabilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(le_is_purpose_selectable(handle, 1), 0); // OBSTRUCTION
    EXPECT_NE(le_is_purpose_selectable(nullptr, 1), 0);

    le_set_purpose_selectable(handle, 1, 0);
    EXPECT_EQ(le_is_purpose_selectable(handle, 1), 0);

    le_set_purpose_selectable(handle, 1, 1);
    EXPECT_NE(le_is_purpose_selectable(handle, 1), 0);
}

TEST_F(ApiFixture, SetPurposeSelectableWithNullHandleDoesNotCrash)
{
    le_set_purpose_selectable(nullptr, 1, 0);
}

TEST_F(ApiFixture, HidingALayerByNameRemovesItFromTheRenderedBuffer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A visible

    le_set_layer_name_visible(handle, "M1", 0); // hides both M1/TERMINAL and M1/OBSTRUCTION

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // M1 is hidden now
}

TEST_F(ApiFixture, HidingATerminalPurposeRemovesItFromTheRenderedBuffer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A (TERMINAL) visible

    le_set_purpose_visible(handle, 0, 0); // TERMINAL off, across every layer

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // PIN A's TERMINAL column is hidden now
}

TEST_F(ApiFixture, GridSpacingDefaultsMatchScene)
{
    EXPECT_EQ(le_minor_grid_spacing(handle), 5);
    EXPECT_EQ(le_major_grid_spacing(handle), 50);
}

TEST_F(ApiFixture, GridSpacingRoundTrips)
{
    le_set_minor_grid_spacing(handle, 10);
    le_set_major_grid_spacing(handle, 100);

    EXPECT_EQ(le_minor_grid_spacing(handle), 10);
    EXPECT_EQ(le_major_grid_spacing(handle), 100);
}

TEST_F(ApiFixture, GridSpacingIgnoresNonPositiveValues)
{
    le_set_minor_grid_spacing(handle, 10);
    le_set_minor_grid_spacing(handle, 0);
    le_set_minor_grid_spacing(handle, -5);
    EXPECT_EQ(le_minor_grid_spacing(handle), 10); // unchanged

    le_set_major_grid_spacing(handle, 100);
    le_set_major_grid_spacing(handle, 0);
    le_set_major_grid_spacing(handle, -5);
    EXPECT_EQ(le_major_grid_spacing(handle), 100); // unchanged
}

TEST_F(ApiFixture, GridSpacingWithNullHandleReturnsZeroAndDoesNotCrash)
{
    EXPECT_EQ(le_minor_grid_spacing(nullptr), 0);
    EXPECT_EQ(le_major_grid_spacing(nullptr), 0);
    le_set_minor_grid_spacing(nullptr, 10);
    le_set_major_grid_spacing(nullptr, 100);
}

TEST_F(ApiFixture, RulerLabelSizeDefaultsMatchesScene)
{
    EXPECT_DOUBLE_EQ(le_ruler_label_size(handle), 11.0);
}

TEST_F(ApiFixture, RulerLabelSizeRoundTrips)
{
    le_set_ruler_label_size(handle, 20.0);
    EXPECT_DOUBLE_EQ(le_ruler_label_size(handle), 20.0);
}

TEST_F(ApiFixture, RulerLabelSizeIgnoresNonPositiveValues)
{
    le_set_ruler_label_size(handle, 20.0);
    le_set_ruler_label_size(handle, 0.0);
    le_set_ruler_label_size(handle, -5.0);
    EXPECT_DOUBLE_EQ(le_ruler_label_size(handle), 20.0); // unchanged
}

TEST_F(ApiFixture, RulerLabelSizeWithNullHandleReturnsZeroAndDoesNotCrash)
{
    EXPECT_DOUBLE_EQ(le_ruler_label_size(nullptr), 0.0);
    le_set_ruler_label_size(nullptr, 20.0);
}

TEST_F(ApiFixture, MousePositionWithNullHandleDoesNotCrash)
{
    le_set_mouse_position(nullptr, 10, 10);
    le_clear_mouse_position(nullptr);
}

TEST_F(ApiFixture, SnappedMousePositionWithNullHandleReturnsNoPosition)
{
    const LeSnappedMousePosition result = le_snapped_mouse_position(nullptr);
    EXPECT_EQ(result.has_position, 0);
    EXPECT_EQ(result.x_um, 0.0);
    EXPECT_EQ(result.y_um, 0.0);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionUntilMouseIsSet)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    le_set_viewport_size(handle, 100, 100);
    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    EXPECT_EQ(result.has_position, 0);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionWithoutATechnologyLoaded)
{
    // No le_read_lef call at all - degrades gracefully instead of dividing
    // by a Technology::database_units_microns that doesn't exist yet.
    le_set_viewport_size(handle, 100, 100);
    le_set_mouse_position(handle, 50, 50);

    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    EXPECT_EQ(result.has_position, 0);
}

TEST_F(ApiFixture, SnappedMousePositionReturnsGridSnappedMicronCoordinates)
{
    // testcell.lef declares DATABASE MICRONS 1000 - 1000 dbu per micron.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    le_set_viewport_size(handle, 100, 100);
    le_set_minor_grid_spacing(handle, 10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70
    // -> 0.02um and 0.07um at 1000 dbu/um.
    le_set_mouse_position(handle, 23, 27);

    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    ASSERT_EQ(result.has_position, 1);
    EXPECT_DOUBLE_EQ(result.x_um, 0.02);
    EXPECT_DOUBLE_EQ(result.y_um, 0.07);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionAfterClear)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    le_set_viewport_size(handle, 100, 100);
    le_set_mouse_position(handle, 50, 50);
    ASSERT_EQ(le_snapped_mouse_position(handle).has_position, 1);

    le_clear_mouse_position(handle);
    EXPECT_EQ(le_snapped_mouse_position(handle).has_position, 0);
}

TEST_F(ApiFixture, RenderPixelBufferShowsRedCursorBoxAtSnappedMousePosition)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100); // scale 1.0 -> 2.0, anchored at dbu (0,0) so pan stays (0,0) - default minor spacing (5dbu*2=10px) clears the density floor

    // Screen pixel (50,50), top-left origin/y-down (same convention as
    // le_zoom's x/y) -> dbu (25,25), already a multiple of the default
    // 5dbu minor grid, so it snaps to itself and the box lands centered
    // on screen pixel (50,50) after compose_with_overlays's Y-flip.
    le_set_mouse_position(handle, 50, 50);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, RenderPixelBufferHasNoCursorBoxWhenNoMousePositionSet)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ClearMousePositionRemovesTheCursorBoxFromSubsequentRenders)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100);
    le_set_mouse_position(handle, 50, 50);

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_red_cursor_pixel(before, 40, 40, 60, 60));

    le_clear_mouse_position(handle);
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(after, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ZoomWithNullHandleDoesNotCrash)
{
    le_zoom(nullptr, 0.5, 10, 10);
}

TEST_F(ApiFixture, ZoomWithDegenerateFactorLeavesScaleAndPanUnchanged)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    // A factor <= -1.0 would make new_scale non-positive - must be
    // ignored entirely (same guard as Scene::set_scale), not clamp to
    // some fallback value.
    le_zoom(handle, -1.0, 50, 50);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect still visible exactly where it was
}

TEST_F(ApiFixture, ZoomKeepsTheAnchorPixelFixedOnScreen)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    // A small window around (50, 50) rather than that one exact pixel -
    // PIN A's ROUTING-layer fill is a tiled stripe pattern, so a single
    // point could land in a transparent gap by coincidence of phase.
    ASSERT_TRUE(region_has_opaque_pixel(before, 43, 43, 57, 57)); // baseline: pin rect visible around (50, 50)

    // Zoom in 2x anchored at the same pixel being sampled - the dbu point
    // under (50, 50) must still be under (50, 50) afterward, so it should
    // still show the same pin rect content there.
    le_zoom(handle, 1.0, 50, 50);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 43, 43, 57, 57));
}

TEST_F(ApiFixture, PanWithNullHandleDoesNotCrash)
{
    le_pan(nullptr, 1.0, 1.0);
}

TEST_F(ApiFixture, PanShiftsContentOutOfView)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: pin rect visible

    // Pan by 2 full viewport widths in dbu x - the whole 10000-dbu-wide
    // macro (well under one viewport width at this scale) ends up entirely
    // out of view.
    le_pan(handle, 2.0, 0.0);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownZoomZoomsInAnchoredAtTheCurrentMousePosition)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_opaque_pixel(before, 13, 48, 17, 52)); // just outside the pin's left edge

    le_set_mouse_position(handle, 50, 50); // center of the macro/pin
    le_key_down(handle, LE_KEY_ZOOM);

    // Zooming in (factor +0.3, scale 0.01 -> 0.013) grows the pin's
    // half-width from 30px to 39px around the same (50,50) anchor - the
    // sample window that was just outside it before is now inside.
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 13, 48, 17, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithShiftHeldZoomsOutInstead)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 23, 48, 27, 52)); // inside the pin, near its left edge

    le_set_mouse_position(handle, 50, 50);
    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_ZOOM);

    // Zooming out (factor -0.3, scale 0.01 -> 0.007) shrinks the pin's
    // half-width from 30px to 21px around the same anchor - the sample
    // window that was inside it before is now outside. Sampled at x:18-22
    // rather than reusing the "before" window (23-27): the mouse position
    // set above also puts the pin under hover, and its outline's stroke
    // antialiasing extends a pixel or two past the pin's own new edge
    // (~x=29) toward the old window, so 23-27 isn't a clean miss anymore.
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 18, 48, 22, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithoutAMousePositionSetDoesNotCrash)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);

    le_key_down(handle, LE_KEY_ZOOM); // no le_set_mouse_position call first - anchors at the default (0,0)

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_NE(buffer.data, nullptr);
}

TEST_F(ApiFixture, ZoomDragFitsTheDraggedRectToTheViewport)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);

    // Scene starts at pan (0,0)/scale 1.0. Drag from pixel (0,100) [dbu
    // (0,0), via pixel_to_dbu] to pixel (10000,-9900) [dbu (10000,10000)] -
    // chosen so the resulting drag rect is exactly the macro's own
    // (0,0)-(10000,10000) bbox. Feeding that into fit_to_content (padding
    // 0, 100x100 viewport) lands on scale 0.01 / pan (0,0) - the same
    // state the existing le_zoom(handle, 100.0/10000.0-1.0, 0, 100)-based
    // zoom tests use - so this reuses their already-verified pixel
    // assertions (PIN A at device (20,20)-(80,80)) as proof the rect-zoom
    // math matches Scene::fit_to_content exactly.
    le_zoom_drag_down(handle, 0, 100);
    le_mouse_up(handle, 10000, -9900);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 23, 48, 27, 52)); // inside the pin, near its left edge
    EXPECT_FALSE(region_has_opaque_pixel(buffer, 13, 48, 17, 52)); // just outside the pin's left edge
}

TEST_F(ApiFixture, ClickSizedZoomDragDoesNotChangeTheView)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    le_zoom_drag_down(handle, 50, 50);
    le_mouse_up(handle, 51, 51); // dx=1,dy=1 - well under kClickDragThresholdPx, so end_drag() with no fit_to_content call

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 23, 48, 27, 52));
    EXPECT_FALSE(region_has_opaque_pixel(buffer, 13, 48, 17, 52));
}

TEST_F(ApiFixture, ZoomDragWithoutAMouseUpLeavesTheSceneStillDragging)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);

    le_zoom_drag_down(handle, 10, 10);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_NE(buffer.data, nullptr); // no crash while a zoom drag is in progress but never released
}

TEST_F(ApiFixture, SelectDragRectangleIsBlueZoomDragRectangleIsGreen)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);

    // Default pan (0,0)/scale 1.0 puts the visible dbu range at
    // (0,0)-(100,100) - far smaller than PIN A's (2000,2000)-(8000,8000)
    // rect, so nothing from the design itself renders here; the grid dots
    // (see draw_grid) are grayscale (R==G==B) and contribute equally to
    // every channel, so comparing the drag rect's own green vs. blue
    // channel is robust regardless of whether a grid dot lands on the
    // sampled pixel.
    le_mouse_down(handle, 10, 10);
    le_set_mouse_position(handle, 90, 90); // Scene::drag_rect_dbu() needs a stored mouse position, not just the down-event x/y

    LePixelBuffer select_buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(select_buffer.data, nullptr);
    const uint8_t *select_pixel = select_buffer.data + static_cast<size_t>(50) * static_cast<size_t>(select_buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(select_pixel[3], 0);
    EXPECT_GT(select_pixel[2], select_pixel[1]); // kDragRectFillColor = {80,160,255,60} - blue > green

    le_mouse_up(handle, 90, 90);

    le_zoom_drag_down(handle, 10, 10);
    le_set_mouse_position(handle, 90, 90);

    LePixelBuffer zoom_buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(zoom_buffer.data, nullptr);
    const uint8_t *zoom_pixel = zoom_buffer.data + static_cast<size_t>(50) * static_cast<size_t>(zoom_buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(zoom_pixel[3], 0);
    EXPECT_GT(zoom_pixel[1], zoom_pixel[2]); // kZoomDragRectFillColor = {80,255,160,60} - green > blue
}

TEST_F(ApiFixture, ZoomDragDownWithNullHandleDoesNotCrash)
{
    le_zoom_drag_down(nullptr, 10, 10);
}

TEST_F(ApiFixture, KeyDownFitFitsTheViewportToContent)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);
    le_key_down(handle, LE_KEY_FIT);

    // Mirrors FitSceneFillsTheViewportWithThePinVisible, triggered via
    // le_key_down instead of le_fit_scene directly.
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, KeyDownPanLeftShiftsContentOutOfView)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    // kKeyPanFactor (0.25) per press - 8 presses covers 2 full viewport
    // widths, the same margin PanShiftsContentOutOfView uses directly.
    for (int i = 0; i < 8; ++i)
        le_key_down(handle, LE_KEY_PAN_LEFT);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownPanDirectionsAreEachOthersOpposite)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    le_key_down(handle, LE_KEY_PAN_LEFT);
    le_key_down(handle, LE_KEY_PAN_RIGHT); // undoes the left pan
    le_key_down(handle, LE_KEY_PAN_UP);
    le_key_down(handle, LE_KEY_PAN_DOWN); // undoes the up pan

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownPanWithCtrlOrShiftHeldIsANoOp)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    le_key_down(handle, LE_KEY_CTRL);
    for (int i = 0; i < 8; ++i)
        le_key_down(handle, LE_KEY_PAN_LEFT);
    le_key_down(handle, LE_KEY_SHIFT); // Ctrl+Shift both held
    for (int i = 0; i < 8; ++i)
        le_key_down(handle, LE_KEY_PAN_LEFT);
    le_key_up(handle, LE_KEY_CTRL);
    for (int i = 0; i < 8; ++i)
        le_key_down(handle, LE_KEY_PAN_LEFT); // Shift alone

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // unchanged
}

// LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE (UPDATES.md item 11).
TEST_F(ApiFixture, KeyDownEditModeSwitchesToEditMode)
{
    ASSERT_EQ(le_get_mode(handle), LE_MODE_SELECT);
    le_key_down(handle, LE_KEY_EDIT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_EDIT);
}

TEST_F(ApiFixture, KeyDownSelectModeSwitchesBackToSelectMode)
{
    le_key_down(handle, LE_KEY_EDIT_MODE);
    ASSERT_EQ(le_get_mode(handle), LE_MODE_EDIT);

    le_key_down(handle, LE_KEY_SELECT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);
}

TEST_F(ApiFixture, KeyDownEditModeWithCtrlOrShiftHeldIsANoOp)
{
    // Regression: 'e'/'s'/'r' used to switch modes even with a modifier
    // held (e.g. Ctrl-S), stealing the keystroke from whatever the
    // modifier combo actually meant. Mode-switch keys are bare-only now.
    ASSERT_EQ(le_get_mode(handle), LE_MODE_SELECT);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_EDIT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);

    le_key_down(handle, LE_KEY_SHIFT); // Ctrl+Shift both held
    le_key_down(handle, LE_KEY_EDIT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);

    le_key_up(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_EDIT_MODE); // Shift alone
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);
}

TEST_F(ApiFixture, KeyDownSelectModeWithCtrlOrShiftHeldIsANoOp)
{
    le_key_down(handle, LE_KEY_EDIT_MODE);
    ASSERT_EQ(le_get_mode(handle), LE_MODE_EDIT);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_EDIT);

    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_SELECT_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_EDIT);
}

TEST_F(ApiFixture, KeyDownRulerModeWithCtrlOrShiftHeldIsANoOp)
{
    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_RULER_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);

    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_RULER_MODE);
    EXPECT_EQ(le_get_mode(handle), LE_MODE_SELECT);

    le_key_up(handle, LE_KEY_CTRL);
    le_key_up(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_RULER_MODE); // bare - actually switches
    EXPECT_EQ(le_get_mode(handle), LE_MODE_RULER);
}

TEST_F(ApiFixture, RenderPixelBufferDrawsThePinRectAtItsExpectedLocation)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    // MACRO SIZE is 10x10 microns, PIN A's RECT is (2,2)-(8,8) microns.
    // DATABASE MICRONS 1000 -> 1 micron = 1000 dbu. Scale chosen so the
    // whole 10x10 micron (10000x10000 dbu) macro fills a 100x100px buffer.
    // Scene starts at scale 1.0 / pan (0, 0) - le_zoom to that scale
    // anchored at image pixel (0, 100) (dbu (0, 0) at the starting
    // pan/scale) keeps pan pinned at (0, 0) exactly.
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);

    // Pin rect (2,2)-(8,8) microns -> pixel (20,20)-(80,80) before
    // rasterize()'s Y-flip -> device (20, 100-80)-(80, 100-20) after it.
    // PIN A is on M1 (ROUTING), so its fill is a tiled diagonal-stripe
    // pattern, not a flat fill - scan a region inside it (inset from the
    // outline hairline) rather than trusting one exact pixel.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // fill pattern - something was actually drawn here

    // Well outside the pin rect, but still inside the macro boundary
    // outline - the boundary is stroke-only (no fill), so this should be
    // fully transparent.
    const uint8_t *outside = buffer.data + static_cast<size_t>(5) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(5) * 4;
    EXPECT_EQ(outside[3], 0);
}

TEST_F(ApiFixture, MouseMoveOverASelectableShapeShowsAYellowHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    // Same pan/scale as RenderPixelBufferDrawsThePinRectAtItsExpectedLocation -
    // PIN A's rect ends up at device pixel (20,20)-(80,80).
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50); // well inside PIN A's rect

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52)); // left edge of the hovered pin's outline
}

TEST_F(ApiFixture, MouseMoveOverAShapeInRulerModeDoesNotShowAHoverOutline)
{
    // Regression: the hover outline is a Select-mode-only affordance -
    // it was left on unconditionally, so it kept highlighting shapes
    // under the cursor while placing ruler points too.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mode(handle, LE_MODE_RULER);
    le_set_mouse_position(handle, 50, 50); // well inside PIN A's rect

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, SwitchingToRulerModeClearsAnAlreadyShownHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50); // over the pin, in Select mode
    ASSERT_TRUE(region_has_yellow_hover_pixel(le_render_pixel_buffer(handle), 18, 48, 22, 52));

    le_set_mode(handle, LE_MODE_RULER); // no further mouse movement
    EXPECT_FALSE(region_has_yellow_hover_pixel(le_render_pixel_buffer(handle), 18, 48, 22, 52));
}

TEST_F(ApiFixture, MouseMoveAwayFromAnyShapeClearsTheHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50); // over the pin
    le_set_mouse_position(handle, 95, 95); // now off every shape

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, ClearMousePositionAlsoClearsTheHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50);
    le_clear_mouse_position(handle);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, MouseMoveOverAnUnselectableLayerNeverShowsAHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_set_mouse_position(handle, 50, 50); // over the pin, but M1 is unselectable

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, KeyDownThenIsKeyHeldReturnsTrue)
{
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_key_down(handle, LE_KEY_SHIFT);
    EXPECT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpClearsAHeldKey)
{
    le_key_down(handle, LE_KEY_SHIFT);
    ASSERT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpWithoutAPrecedingKeyDownIsANoOp)
{
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyStateFunctionsWithNullHandleDoNotCrash)
{
    le_key_down(nullptr, LE_KEY_SHIFT);
    le_key_up(nullptr, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(nullptr, LE_KEY_SHIFT), 0);
    le_clear_all_keys(nullptr);
}

TEST_F(ApiFixture, ClearAllKeysReleasesAHeldShift)
{
    le_key_down(handle, LE_KEY_SHIFT);
    ASSERT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_clear_all_keys(handle);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}


namespace
{
    // Shared by every click/drag-select test below: reads two_shapes.lef
    // (MACRO TWOSHAPES, PIN A at (1,1)-(4,4) micron, PIN B at
    // (16,16)-(19,19) micron - see the fixture file), and zooms a 200x200
    // viewport to scale 0.01 (10px/micron) with pan pinned at (0,0), same
    // "anchor at the bottom-left image corner" trick as other tests in
    // this file. At that scale/pan (device/image pixel space, top-left
    // origin, y down):
    //   - PIN A occupies device (10,160)-(40,190) - center ~(25,175).
    //   - PIN B occupies device (160,10)-(190,40) - center ~(175,25).
    //   - (100,100) device is empty space (10,10 micron - inside the
    //     macro, outside both pins).
    void load_two_shapes_at_known_scale(LeHandle *handle)
    {
        ASSERT_EQ(le_read_lef(handle, fixture_path("two_shapes.lef").c_str()), 0);
        ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

        le_set_viewport_size(handle, 200, 200);
        le_zoom(handle, 0.01 - 1.0, 0, 200);
    }
}

TEST_F(ApiFixture, CtrlSelectAllSelectsEveryShapeRegardlessOfViewport)
{
    load_two_shapes_at_known_scale(handle);

    // Zoom in tight on PIN A alone (huge factor, anchored at its own
    // center) so PIN B - way off in the opposite corner of the macro - is
    // no longer inside the viewport. select_all_unlocked bypasses
    // filter_by_viewport_and_size entirely (see its own comment in
    // api.cpp), so both shapes should still get selected.
    le_zoom(handle, 5.0, 25, 175);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, SelectAllWithoutCtrlHeldIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_SELECT_ALL); // Ctrl never pressed

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, SelectAllSkipsUnselectableLayers)
{
    load_two_shapes_at_known_scale(handle);
    le_set_layer_name_selectable(handle, "M1", 0); // both PIN A and PIN B are on M1

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, SelectAllIsCappedAt10000AndWarns)
{
    const std::string path = generate_concurrency_stress_lef(10050);
    ASSERT_EQ(le_read_lef(handle, path.c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

    ASSERT_EQ(le_message_count(handle), 0);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 10000);

    ASSERT_GT(le_message_count(handle), 0);
    const std::string message = le_message_at(handle, 0);
    EXPECT_NE(message.find("capped"), std::string::npos);
}

TEST_F(ApiFixture, SelectAllWithNullHandleDoesNotCrash)
{
    le_key_down(nullptr, LE_KEY_SELECT_ALL);
}

TEST_F(ApiFixture, SelectAllWithCtrlAndShiftBothHeldIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_SELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, CtrlDDeselectAllClearsTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);
    ASSERT_EQ(le_selection_count(handle), 2);

    le_key_down(handle, LE_KEY_DESELECT_ALL); // Ctrl still held from above
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, DeselectAllWithoutCtrlHeldIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_DESELECT_ALL); // Ctrl never pressed
    EXPECT_EQ(le_selection_count(handle), 1);
}

TEST_F(ApiFixture, DeselectAllWhenNothingIsSelectedIsANoOp)
{
    load_two_shapes_at_known_scale(handle);
    ASSERT_EQ(le_selection_count(handle), 0);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_DESELECT_ALL);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, DeselectAllWithNullHandleDoesNotCrash)
{
    le_key_down(nullptr, LE_KEY_DESELECT_ALL);
}

TEST_F(ApiFixture, DeselectAllWithCtrlAndShiftBothHeldIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);
    ASSERT_EQ(le_selection_count(handle), 2);

    le_key_down(handle, LE_KEY_SHIFT); // Ctrl+Shift both held
    le_key_down(handle, LE_KEY_DESELECT_ALL);
    EXPECT_EQ(le_selection_count(handle), 2); // unchanged
}

TEST_F(ApiFixture, CtrlFFitsTheViewportToOnlyTheSelectedShape)
{
    load_two_shapes_at_known_scale(handle);

    // (100,100) device is empty space at the baseline scale (see
    // load_two_shapes_at_known_scale's own comment) - neither pin reaches
    // the viewport center.
    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_opaque_pixel(before, 80, 80, 120, 120));

    le_mouse_down(handle, 25, 175); // PIN A only
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_FIT);

    // PIN A's own (1,1)-(4,4) micron bbox now fills the viewport (with
    // kKeyFitPaddingPx of margin) - the same center window that was empty
    // at the whole-design baseline scale is now well inside it.
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 80, 80, 120, 120));
}

TEST_F(ApiFixture, FitSelectedWithNoSelectionLeavesTheViewUnchanged)
{
    load_two_shapes_at_known_scale(handle);
    ASSERT_EQ(le_selection_count(handle), 0);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_FIT);

    // Nothing to fit to - view stays at load_two_shapes_at_known_scale's
    // own baseline (see its comment for these exact device positions).
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 20, 170, 30, 180)); // PIN A
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 170, 20, 180, 30)); // PIN B
}

TEST_F(ApiFixture, PlainFKeyStillFitsTheWholeSceneEvenWithASelectionActive)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A only
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_FIT); // Ctrl not held - whole-design fit, not Ctrl-F's fit-selected

    // Both pins should still be visible (generous quadrant windows -
    // le_fit_scene's own computed scale/pan differs slightly from
    // load_two_shapes_at_known_scale's synthetic baseline, since it fits
    // with kKeyFitPaddingPx rather than that helper's own zoom trick, but
    // PIN A stays in the bottom-left quadrant and PIN B in the top-right
    // either way).
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 0, 140, 60, 200)); // PIN A's quadrant
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 140, 0, 200, 60)); // PIN B's quadrant
}

TEST_F(ApiFixture, MouseDownThenUpAsAClickSelectsTheHitShape)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);

    EXPECT_EQ(le_selection_count(handle), 1);
}

TEST_F(ApiFixture, SelectionVersionBumpsOnlyOnAnActualSelectionChange)
{
    load_two_shapes_at_known_scale(handle);

    const int64_t baseline = le_selection_version(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);
    const int64_t after_select = le_selection_version(handle);
    EXPECT_NE(after_select, baseline);

    // Reselecting the exact same shape (no shift, so it clears first then
    // reselects the same one) still changes it, since clear+reselect is
    // two real selection_ mutations even though the end state looks the
    // same - le_selection_version reflects Scene::selection_version()
    // directly, not a "did the final state differ" comparison.
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    EXPECT_NE(le_selection_version(handle), after_select);
    const int64_t after_reselect = le_selection_version(handle);

    // A pure mouse-move (no selection change) must not bump it - this is
    // the whole point of exposing this counter (see BENCHMARKS.md).
    le_set_mouse_position(handle, 30, 170);
    EXPECT_EQ(le_selection_version(handle), after_reselect);
}

TEST_F(ApiFixture, SelectionVersionWithNullHandleDoesNotCrash)
{
    EXPECT_EQ(le_selection_version(nullptr), 0);
}

TEST_F(ApiFixture, SmallMovementBetweenDownAndUpIsStillTreatedAsAClick)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 26, 176); // well under the click/drag threshold

    EXPECT_EQ(le_selection_count(handle), 1);
}

TEST_F(ApiFixture, ClickOnEmptySpaceClearsTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 100, 100); // empty space
    le_mouse_up(handle, 100, 100);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, PlainClickReplacesThePreviousSelectionRatherThanAddingToIt)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 175, 25); // PIN B, no shift
    le_mouse_up(handle, 175, 25);
    EXPECT_EQ(le_selection_count(handle), 1); // still just one - B replaced A, not added
}

TEST_F(ApiFixture, ShiftClickAddsToTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 175, 25); // PIN B, with shift
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 175, 25);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, ShiftClickOnEmptySpaceIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 100, 100); // empty space, with shift
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 100, 100);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 1); // unchanged
}

TEST_F(ApiFixture, ClickOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(handle);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, DragSelectEnclosesEverySelectableShapeInTheRectangle)
{
    load_two_shapes_at_known_scale(handle);

    // Full-viewport drag - dbu (0,0) to (20,20) micron, the whole macro,
    // enclosing both pins.
    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0);

    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, DragSelectExcludesAShapeThatIsOnlyPartiallyEnclosed)
{
    load_two_shapes_at_known_scale(handle);

    // dbu (0,0)-(2.5,5) micron - clips through the left portion of PIN A
    // (1,1)-(4,4) micron without fully enclosing it, and nowhere near
    // PIN B.
    le_mouse_down(handle, 0, 150);
    le_mouse_up(handle, 25, 200);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, ShiftDragAddsToTheExistingSelectionRatherThanReplacingIt)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, plain click
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    // Small rect tightly around just PIN B, with shift.
    le_mouse_down(handle, 155, 5);
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 195, 45);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, DragSelectOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(handle);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0);
    EXPECT_EQ(le_selection_count(handle), 0);
}

// Edit mode gates le_mouse_up's selection changes (UPDATES.md item 11).
TEST_F(ApiFixture, ClickInEditModeDoesNotChangeSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, in Select mode
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_set_mode(handle, LE_MODE_EDIT);
    le_mouse_down(handle, 175, 25); // PIN B, in Edit mode
    le_mouse_up(handle, 175, 25);
    EXPECT_EQ(le_selection_count(handle), 1); // still just PIN A - unchanged
}

TEST_F(ApiFixture, DragSelectInEditModeDoesNotChangeSelection)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_EDIT);

    // Same full-viewport drag rectangle that enclosed both pins in Select
    // mode (see DragSelectEnclosesEverySelectableShapeInTheRectangle).
    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, MouseUpInEditModeStillEndsDragging)
{
    // No public le_is_dragging accessor exists, so this is verified
    // indirectly: le_mouse_up's own guard (`if (!is_dragging()) return`,
    // api.cpp) makes a *second* mouse-up with no preceding mouse-down a
    // no-op only if the first mouse-up's end_drag() actually ran despite
    // the Edit-mode gate skipping the selection block around it.
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_EDIT);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175); // end_drag() must run even though selection is skipped

    le_set_mode(handle, LE_MODE_SELECT);
    le_mouse_up(handle, 175, 25); // no preceding mouse-down - a no-op if dragging really ended
    EXPECT_EQ(le_selection_count(handle), 0);
}

// Ruler mode (UPDATES.md item 13).
TEST_F(ApiFixture, ClickInRulerModeCommitsAPointAndLeavesSelectionUntouched)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, in Select mode
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_set_mode(handle, LE_MODE_RULER);
    le_set_mouse_position(handle, 100, 100); // real UI drives this via hover/move events before a click
    le_mouse_down(handle, 100, 100);
    le_mouse_up(handle, 100, 100);

    EXPECT_EQ(le_ruler_point_count(handle, 0), 1);
    EXPECT_EQ(le_selection_count(handle), 1); // unchanged
}

TEST_F(ApiFixture, DragInRulerModeDoesNotCommitAPoint)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 0, 200);
    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0); // well beyond the click/drag threshold

    EXPECT_EQ(le_ruler_point_count(handle, 0), 0);
}

TEST_F(ApiFixture, ShiftHeldClickInRulerModeAllowsANonOrthogonalPoint)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 20, 180);
    le_mouse_down(handle, 20, 180);
    le_mouse_up(handle, 20, 180);
    ASSERT_EQ(le_ruler_point_count(handle, 0), 1);

    // Without shift, this would snap orthogonal (whichever axis moved
    // more wins, per Scene::ruler_next_point).
    le_key_down(handle, LE_KEY_SHIFT);
    le_set_mouse_position(handle, 60, 140);
    le_mouse_down(handle, 60, 140);
    le_mouse_up(handle, 60, 140);
    le_key_up(handle, LE_KEY_SHIFT);

    ASSERT_EQ(le_ruler_point_count(handle, 0), 2);
    const LeRulerPoint p0 = le_ruler_point_at(handle, 0, 0);
    const LeRulerPoint p1 = le_ruler_point_at(handle, 0, 1);
    EXPECT_NE(p0.x_um, p1.x_um);
    EXPECT_NE(p0.y_um, p1.y_um);
}

TEST_F(ApiFixture, RulerPointsComeBackMicronConverted)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 25, 175); // same device position as PIN A's center
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);

    ASSERT_EQ(le_ruler_point_count(handle, 0), 1);
    const LeRulerPoint p = le_ruler_point_at(handle, 0, 0);
    // load_two_shapes_at_known_scale's own comment: device (25,175) is
    // ~2.5 micron in from the macro's origin - just check it landed in
    // a sane, non-zero range rather than pin an exact grid-snapped value.
    EXPECT_GT(p.x_um, 0.0);
    EXPECT_GT(p.y_um, 0.0);
    EXPECT_LT(p.x_um, 10.0);
    EXPECT_LT(p.y_um, 10.0);
}

TEST_F(ApiFixture, EscKeyFinishesTheRulerKeepingEveryCommittedPoint)
{
    // UPDATES.md item 13 - Esc (LE_KEY_FINISH_RULER) finishes the active
    // ruler without touching any of its already-committed points, unlike
    // the earlier double-click design this replaced.
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 20, 180);
    le_mouse_down(handle, 20, 180);
    le_mouse_up(handle, 20, 180);
    le_set_mouse_position(handle, 150, 30);
    le_mouse_down(handle, 150, 30);
    le_mouse_up(handle, 150, 30);
    ASSERT_EQ(le_ruler_point_count(handle, 0), 2);

    le_key_down(handle, LE_KEY_FINISH_RULER);

    EXPECT_EQ(le_ruler_point_count(handle, 0), 2); // both points survive
    EXPECT_EQ(le_ruler_count(handle), 1); // still just the one ruler
}

TEST_F(ApiFixture, EscKeyWithNoActiveRulerIsANoOp)
{
    le_key_down(handle, LE_KEY_FINISH_RULER); // no crash, nothing to finish
    EXPECT_EQ(le_ruler_count(handle), 0);
}

TEST_F(ApiFixture, FinishRulerThenNewRulerRequiresTheMinimumDistanceGuard)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 20, 180);
    le_mouse_down(handle, 20, 180);
    le_mouse_up(handle, 20, 180);
    ASSERT_EQ(le_ruler_point_count(handle, 0), 1);

    le_finish_ruler(handle);
    ASSERT_EQ(le_ruler_count(handle), 1);

    // Only 1 device px away - well under the distance guard.
    le_set_mouse_position(handle, 21, 180);
    le_mouse_down(handle, 21, 180);
    le_mouse_up(handle, 21, 180);
    EXPECT_EQ(le_ruler_count(handle), 1);
    EXPECT_EQ(le_ruler_point_count(handle, 0), 1);

    // Far enough away - starts a new ruler, leaving the first untouched.
    le_set_mouse_position(handle, 60, 180);
    le_mouse_down(handle, 60, 180);
    le_mouse_up(handle, 60, 180);
    EXPECT_EQ(le_ruler_count(handle), 2);
    EXPECT_EQ(le_ruler_point_count(handle, 0), 1);
    EXPECT_EQ(le_ruler_point_count(handle, 1), 1);
}

TEST_F(ApiFixture, ZoomStillWorksInRulerModeMidRulerAndDoesNotMoveCommittedPoints)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_RULER);

    le_set_mouse_position(handle, 20, 180);
    le_mouse_down(handle, 20, 180);
    le_mouse_up(handle, 20, 180);
    ASSERT_EQ(le_ruler_point_count(handle, 0), 1);
    const LeRulerPoint before = le_ruler_point_at(handle, 0, 0);

    le_set_mouse_position(handle, 20, 180);
    le_key_down(handle, LE_KEY_ZOOM); // zooms in, anchored at the current mouse position

    const LeRulerPoint after = le_ruler_point_at(handle, 0, 0);
    EXPECT_DOUBLE_EQ(before.x_um, after.x_um);
    EXPECT_DOUBLE_EQ(before.y_um, after.y_um);
}

TEST_F(ApiFixture, MouseDownAloneDoesNotChangeTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0); // nothing committed until mouse-up
}

TEST_F(ApiFixture, MouseUpWithoutAPrecedingMouseDownIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_up(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, MouseDownAndUpWithNullHandleDoNotCrash)
{
    le_mouse_down(nullptr, 25, 175);
    le_mouse_up(nullptr, 25, 175);
    EXPECT_EQ(le_selection_count(nullptr), 0);
}

TEST_F(ApiFixture, ClickSelectingAShapeShowsAWhiteOutlineInTheRenderedBuffer)
{
    load_two_shapes_at_known_scale(handle);

    // Regression: render once *before* selecting anything, so
    // rasterize_frame/compose_with_overlays's caches are already warm at
    // {AbstractId, viewport_version, visibility_version} - none of which
    // change when a selection is made. Their cache keys used to stop
    // there, so this exact sequence (render, then select, then render
    // again) returned the pre-selection frame unchanged: build_picture
    // correctly produced a new SkPicture with the outline baked in, but
    // downstream, CachedStage::get only ever compares the key tuple, not
    // the SkPicture argument itself, so the stale frame from the first
    // render won. selection_version now closes that gap.
    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_white_selection_pixel(before, 8, 170, 12, 180));

    le_mouse_down(handle, 25, 175); // PIN A, device (10,160)-(40,190)
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(after, 8, 170, 12, 180)); // PIN A's left edge
}

TEST_F(ApiFixture, ClearingTheSelectionRemovesTheWhiteOutline)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    // Render with the selection active first (and warm every downstream
    // cache at this selection_version), matching a real frontend that
    // redraws after every gesture - not just once at the very end.
    LePixelBuffer selected = le_render_pixel_buffer(handle);
    ASSERT_NE(selected.data, nullptr);
    ASSERT_TRUE(region_has_white_selection_pixel(selected, 8, 170, 12, 180));

    le_mouse_down(handle, 100, 100); // empty space - clears the selection
    le_mouse_up(handle, 100, 100);
    ASSERT_EQ(le_selection_count(handle), 0);

    LePixelBuffer cleared = le_render_pixel_buffer(handle);
    ASSERT_NE(cleared.data, nullptr);
    EXPECT_FALSE(region_has_white_selection_pixel(cleared, 8, 170, 12, 180));
}

TEST_F(ApiFixture, SwitchingToADifferentDesignClearsTheSelection)
{
    load_two_shapes_at_known_scale(handle); // design 0 = TWOSHAPES
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0); // design 1 = TESTCELL

    le_mouse_down(handle, 25, 175); // PIN A on TWOSHAPES
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    ASSERT_EQ(le_set_current_design_abstract(handle, 1), 0); // switch to TESTCELL
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, SwitchingToADifferentDesignClearsRulers)
{
    // Regression: rulers used to leak across Abstracts - drawn in one
    // Design's abstract view, they'd keep showing up (at the same raw
    // dbu coordinates) after switching to a different Design entirely.
    load_two_shapes_at_known_scale(handle); // design 0 = TWOSHAPES
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0); // design 1 = TESTCELL

    le_set_mode(handle, LE_MODE_RULER);
    le_set_mouse_position(handle, 25, 175);
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_ruler_count(handle), 1);

    ASSERT_EQ(le_set_current_design_abstract(handle, 1), 0); // switch to TESTCELL
    EXPECT_EQ(le_ruler_count(handle), 0);
}

TEST_F(ApiFixture, ClearAllKeysMakesSubsequentClicksReplaceRatherThanAddAgain)
{
    // The exact reported bug: a stuck shift (e.g. from a missed key-up on
    // focus loss) made every later plain click behave like a shift-click.
    // le_clear_all_keys is the escape hatch - simulates a focus-loss
    // recovery.
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_SHIFT); // shift held, then "stuck" (no key-up)

    le_mouse_down(handle, 25, 175); // PIN A, shift-click
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_clear_all_keys(handle); // e.g. called from a focus-loss handler

    le_mouse_down(handle, 175, 25); // PIN B, plain click - should replace, not add
    le_mouse_up(handle, 175, 25);
    EXPECT_EQ(le_selection_count(handle), 1);
}

namespace
{
    // Shared by every selection-properties test below (UPDATES.md 7.2):
    // reads pin_and_obstruction.lef (MACRO PINOBS, PIN A at (1,1)-(4,4)
    // micron on M1, one OBS rect at (10,10)-(15,15) micron on M1 - see the
    // fixture file), and zooms a 200x200 viewport to scale 0.01 with pan
    // pinned at (0,0), same trick as load_two_shapes_at_known_scale. At
    // that scale/pan (device/image pixel space, top-left origin, y down):
    //   - PIN A occupies device (10,160)-(40,190) - center ~(25,175).
    //   - The OBS rect occupies device (100,50)-(150,100) - center ~(125,75).
    void load_pin_and_obstruction_at_known_scale(LeHandle *handle)
    {
        ASSERT_EQ(le_read_lef(handle, fixture_path("pin_and_obstruction.lef").c_str()), 0);
        ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);

        le_set_viewport_size(handle, 200, 200);
        le_zoom(handle, 0.01 - 1.0, 0, 200);
    }

    // Scans an LeObjectRef's full property table into a name -> row map,
    // so a test can look up "does this property exist and does it have
    // this value" without hard-coding property order.
    std::map<std::string, LeProperty> object_properties(LeHandle *handle, LeObjectRef ref)
    {
        std::map<std::string, LeProperty> properties;
        const int32_t count = le_object_property_count(handle, ref);
        for (int32_t i = 0; i < count; ++i)
        {
            const LeProperty property = le_object_property_at(handle, ref, i);
            properties.emplace(property.name, property);
        }
        return properties;
    }

    // Same, but starting from a selection index - the shape-level ref
    // le_selected_object_ref() reports for it.
    std::map<std::string, LeProperty> selected_object_properties(LeHandle *handle, int32_t selection_index)
    {
        return object_properties(handle, le_selected_object_ref(handle, selection_index));
    }
}

TEST_F(ApiFixture, ClickSelectingAShapeReportsExactlyTheSamePropertiesAsGetPropertiesOnItsShapeId)
{
    // The originally reported bug's own regression test: clicking a shape
    // must report the *exact* same rows le_shape_property_at (TCL's
    // get_properties shape:<id>) already shows for that same ShapeId -
    // not a pipeline-merged/derived summary. Selection is shape-granular
    // (le_selected_object_ref always reports LE_OBJECT_KIND_SHAPE), so
    // this compares le_object_property_at's LE_OBJECT_KIND_SHAPE branch
    // directly against le_shape_property_at for the identical id.
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    const LeObjectRef ref = le_selected_object_ref(handle, 0);
    EXPECT_EQ(ref.kind, LE_OBJECT_KIND_SHAPE);
    ASSERT_NE(ref.index, UINT32_MAX);

    const LeShapeId shape_id{.index = ref.index, .generation = ref.generation};
    const int32_t via_selection_count = le_object_property_count(handle, ref);
    const int32_t via_shape_id_count = le_shape_property_count(handle, shape_id);
    ASSERT_EQ(via_selection_count, via_shape_id_count);

    for (int32_t i = 0; i < via_selection_count; ++i)
    {
        const LeProperty via_selection = le_object_property_at(handle, ref, i);
        const LeProperty via_shape_id = le_shape_property_at(handle, shape_id, i);
        EXPECT_STREQ(via_selection.name, via_shape_id.name);
        EXPECT_EQ(via_selection.type, via_shape_id.type);
        switch (via_selection.type)
        {
        case LE_PROPERTY_TYPE_STRING:
            EXPECT_STREQ(via_selection.string_value, via_shape_id.string_value);
            break;
        case LE_PROPERTY_TYPE_INT:
            EXPECT_EQ(via_selection.int_value, via_shape_id.int_value);
            break;
        case LE_PROPERTY_TYPE_DOUBLE:
            EXPECT_DOUBLE_EQ(via_selection.double_value, via_shape_id.double_value);
            break;
        }
    }
}

TEST_F(ApiFixture, SelectedShapesParentChainReportsTerminalPortThenTerminal)
{
    // Selection reports the exact Shape hit (see the regression test
    // above) - a Property Viewer walks *up* from there via le_object_parent
    // to reach the owning TerminalPort/Terminal's own name/direction/
    // port_count, rather than those rows being folded into the Shape's
    // own property list the way the old selection-index path used to.
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    const LeObjectRef shape_ref = le_selected_object_ref(handle, 0);
    ASSERT_EQ(shape_ref.kind, LE_OBJECT_KIND_SHAPE);

    const LeObjectRef port_ref = le_object_parent(handle, shape_ref);
    EXPECT_EQ(port_ref.kind, LE_OBJECT_KIND_TERMINAL_PORT);
    ASSERT_NE(port_ref.index, UINT32_MAX);

    const LeObjectRef terminal_ref = le_object_parent(handle, port_ref);
    EXPECT_EQ(terminal_ref.kind, LE_OBJECT_KIND_TERMINAL);
    ASSERT_NE(terminal_ref.index, UINT32_MAX);

    const std::map<std::string, LeProperty> terminal_properties = object_properties(handle, terminal_ref);
    ASSERT_TRUE(terminal_properties.contains("name"));
    EXPECT_STREQ(terminal_properties.at("name").string_value, "A");
    ASSERT_TRUE(terminal_properties.contains("direction"));
    EXPECT_STREQ(terminal_properties.at("direction").string_value, "INPUT");
    ASSERT_TRUE(terminal_properties.contains("ports_count"));
    EXPECT_EQ(terminal_properties.at("ports_count").int_value, 1);

    // Library has no parent - the chain terminates gracefully.
    const LeObjectRef abstract_ref = le_object_parent(handle, terminal_ref);
    EXPECT_EQ(abstract_ref.kind, LE_OBJECT_KIND_ABSTRACT);
    const LeObjectRef design_ref = le_object_parent(handle, abstract_ref);
    EXPECT_EQ(design_ref.kind, LE_OBJECT_KIND_DESIGN);
    const LeObjectRef library_ref = le_object_parent(handle, design_ref);
    EXPECT_EQ(library_ref.kind, LE_OBJECT_KIND_LIBRARY);
    const LeObjectRef no_parent = le_object_parent(handle, library_ref);
    EXPECT_EQ(no_parent.index, UINT32_MAX);
}

TEST_F(ApiFixture, SelectedTerminalReportsItsRectConvertedToMicrons)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, (1,1)-(4,4) micron
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_EQ(properties.at("rects").type, LE_PROPERTY_TYPE_STRING);
    EXPECT_STREQ(properties.at("rects").string_value, "{{1 1} {4 4}}");
    ASSERT_TRUE(properties.contains("polygons"));
    EXPECT_STREQ(properties.at("polygons").string_value, "");
    ASSERT_TRUE(properties.contains("paths"));
    EXPECT_STREQ(properties.at("paths").string_value, "");
}

TEST_F(ApiFixture, SelectedTerminalRectTrimsTrailingZerosInWholeGroupsOfThree)
{
    // MACRO FRACPIN's PIN A is a RECT at (0.34,0.34)-(5.34,5.34) micron
    // (see fractional_pin.lef) - std::to_string would format each as 6
    // decimal digits ("0.340000"/"5.340000"); the trailing zeros should
    // trim down to the last *significant* group of three ("0.340"), not
    // strip further into the significant "340" group and not leave a
    // partial group like "0.34".
    ASSERT_EQ(le_read_lef(handle, fixture_path("fractional_pin.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 0.01 - 1.0, 0, 100);

    le_mouse_down(handle, 28, 72); // inside the rect - dbu (2800,2800)
    le_mouse_up(handle, 28, 72);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_STREQ(properties.at("rects").string_value, "{{0.340 0.340} {5.340 5.340}}");
}

TEST_F(ApiFixture, SelectedTerminalRectKeepsFullPrecisionWhenNotAMultipleOfAThousand)
{
    // format_coordinate_um's "leave the value untouched" branch, otherwise
    // untested - every other rects test uses DATABASE MICRONS 1000,
    // where um = dbu/1000 is always exactly 3-decimal-precise, so the
    // first trailing-zero-group check always succeeds there. This
    // fixture uses DATABASE MICRONS 2000 (full_precision_pin.lef) with a
    // RECT at (0.0005,0.0005)-(5.0005,5.0005) micron - a value that
    // genuinely needs all 6 decimal digits, so the trim loop's first
    // check ("500" isn't "000") must fail immediately and leave it alone.
    ASSERT_EQ(le_read_lef(handle, fixture_path("full_precision_pin.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 0.01 - 1.0, 0, 100);

    le_mouse_down(handle, 50, 50); // inside the rect - dbu (5000,5000)
    le_mouse_up(handle, 50, 50);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_STREQ(properties.at("rects").string_value, "{{0.000500 0.000500} {5.000500 5.000500}}");
}

TEST_F(ApiFixture, SelectedObstructionPieceReportsItsRectThenParentReportsShapesCount)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 125, 75); // the OBS rect, (10,10)-(15,15) micron
    le_mouse_up(handle, 125, 75);
    ASSERT_EQ(le_selection_count(handle), 1);

    const LeObjectRef shape_ref = le_selected_object_ref(handle, 0);
    EXPECT_EQ(shape_ref.kind, LE_OBJECT_KIND_SHAPE);

    const std::map<std::string, LeProperty> shape_properties = object_properties(handle, shape_ref);
    ASSERT_TRUE(shape_properties.contains("rects"));
    EXPECT_STREQ(shape_properties.at("rects").string_value, "{{10 10} {15 15}}");
    // Terminal/Obstruction-level property, should not leak onto a Shape's table.
    EXPECT_FALSE(shape_properties.contains("name"));
    EXPECT_FALSE(shape_properties.contains("shapes_count"));

    const LeObjectRef obstruction_ref = le_object_parent(handle, shape_ref);
    EXPECT_EQ(obstruction_ref.kind, LE_OBJECT_KIND_OBSTRUCTION);
    const std::map<std::string, LeProperty> obstruction_properties = object_properties(handle, obstruction_ref);
    ASSERT_TRUE(obstruction_properties.contains("shapes_count"));
    EXPECT_EQ(obstruction_properties.at("shapes_count").type, LE_PROPERTY_TYPE_INT);
    EXPECT_EQ(obstruction_properties.at("shapes_count").int_value, 1);
}

TEST_F(ApiFixture, SelectedObjectPropertiesDistinguishTwoSelectedObjectsByIndex)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_down(handle, 125, 75); // OBS, shift-click adds
    le_mouse_up(handle, 125, 75);
    ASSERT_EQ(le_selection_count(handle), 2);

    // Both selections are shape-level (LE_OBJECT_KIND_SHAPE) - "distinguish
    // by index" now means each index's own Shape properties (own rect),
    // with name/shapes_count reachable one hop up via le_object_parent.
    const LeObjectRef first_ref = le_selected_object_ref(handle, 0);
    const LeObjectRef second_ref = le_selected_object_ref(handle, 1);
    EXPECT_EQ(first_ref.kind, LE_OBJECT_KIND_SHAPE);
    EXPECT_EQ(second_ref.kind, LE_OBJECT_KIND_SHAPE);

    const LeObjectRef terminal_ref = le_object_parent(handle, le_object_parent(handle, first_ref));
    EXPECT_EQ(terminal_ref.kind, LE_OBJECT_KIND_TERMINAL);
    const std::map<std::string, LeProperty> terminal_properties = object_properties(handle, terminal_ref);
    EXPECT_STREQ(terminal_properties.at("name").string_value, "A");

    const LeObjectRef obstruction_ref = le_object_parent(handle, second_ref);
    EXPECT_EQ(obstruction_ref.kind, LE_OBJECT_KIND_OBSTRUCTION);
    const std::map<std::string, LeProperty> obstruction_properties = object_properties(handle, obstruction_ref);
    EXPECT_EQ(obstruction_properties.at("shapes_count").int_value, 1);
}

TEST_F(ApiFixture, SelectedObjectRefAndPropertiesAreOutOfRangeSafe)
{
    load_pin_and_obstruction_at_known_scale(handle);

    EXPECT_EQ(le_selected_object_ref(handle, 0).index, UINT32_MAX); // nothing selected
    EXPECT_EQ(le_object_property_count(handle, le_object_invalid_ref()), 0);

    const LeProperty invalid = le_object_property_at(handle, le_object_invalid_ref(), 0);
    EXPECT_EQ(invalid.name, nullptr);

    le_mouse_down(handle, 25, 175); // select PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    EXPECT_EQ(le_selected_object_ref(handle, 1).index, UINT32_MAX);  // out of range
    EXPECT_EQ(le_selected_object_ref(handle, -1).index, UINT32_MAX);

    const LeObjectRef ref = le_selected_object_ref(handle, 0);
    const LeProperty out_of_range_property = le_object_property_at(handle, ref, 9999);
    EXPECT_EQ(out_of_range_property.name, nullptr);
}

TEST_F(ApiFixture, SelectedObjectRefAndPropertiesWithNullHandleDoNotCrash)
{
    EXPECT_EQ(le_selected_object_ref(nullptr, 0).index, UINT32_MAX);
    EXPECT_EQ(le_object_property_count(nullptr, le_object_invalid_ref()), 0);
    const LeProperty property = le_object_property_at(nullptr, le_object_invalid_ref(), 0);
    EXPECT_EQ(property.name, nullptr);
    EXPECT_EQ(le_object_parent(nullptr, le_object_invalid_ref()).index, UINT32_MAX);
}

TEST_F(ApiFixture, ClickSelectingOnePortOfATwoPortTerminalReportsOnlyThatPortsRectAndLayer)
{
    // PIN B has two ports on M1 - (16,1)-(19,4) and (16,16)-(19,19)
    // micron (see pin_and_obstruction.lef) - device (10px/micron,
    // pan (0,0)): port 1 occupies device (160,160)-(190,190) (center
    // ~(175,175)), port 2 occupies device (160,10)-(190,40) (center
    // ~(175,25)).
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 175, 175); // PIN B's first port only
    le_mouse_up(handle, 175, 175);
    ASSERT_EQ(le_selection_count(handle), 1);
    const LeObjectRef shape_ref = le_selected_object_ref(handle, 0);
    ASSERT_EQ(shape_ref.kind, LE_OBJECT_KIND_SHAPE);

    const std::map<std::string, LeProperty> properties = object_properties(handle, shape_ref);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_STREQ(properties.at("rects").string_value, "{{16 1} {19 4}}"); // just the clicked port, not the union of both

    // Shape.layer is a reference field now (LeLayerId, not a plain str),
    // so the generic property table's own "layer" row just shows a raw
    // debug id (le::to_string(LayerId) - see codegen's own wrap_with_
    // to_display_property() comment; to_properties() has no Root to
    // resolve a friendly name from) - le_shape_layer_name is the real,
    // resolved accessor to check against instead.
    EXPECT_STREQ(le_shape_layer_name(handle, LeShapeId{.index = shape_ref.index, .generation = shape_ref.generation}), "M1");

    // Parent-level context (Terminal's own name/port_count) is one hop up
    // via le_object_parent, not folded into the Shape's own row list.
    const LeObjectRef terminal_ref = le_object_parent(handle, le_object_parent(handle, shape_ref));
    ASSERT_EQ(terminal_ref.kind, LE_OBJECT_KIND_TERMINAL);
    const std::map<std::string, LeProperty> terminal_properties = object_properties(handle, terminal_ref);
    ASSERT_TRUE(terminal_properties.contains("name"));
    EXPECT_STREQ(terminal_properties.at("name").string_value, "B");
    ASSERT_TRUE(terminal_properties.contains("ports_count"));
    EXPECT_EQ(terminal_properties.at("ports_count").int_value, 2);
}

TEST_F(ApiFixture, DragSelectingATwoPortTerminalSelectsBothPortsIndependently)
{
    // Regression: rectangle-selecting a Terminal/Obstruction with several
    // disjoint pieces must select each enclosed piece independently
    // (mirroring shift-click's own behavior - see
    // ShiftClickingTwoPiecesOfTheSameTerminalSelectsBothIndependently),
    // not collapse to one whole-object selection - which, for the
    // reported stress-test case, meant a 2-piece drag on a ~900,000-piece
    // Obstruction highlighted every single piece belonging to it.
    load_pin_and_obstruction_at_known_scale(handle);

    // Encloses both of PIN B's ports (device x:150-200 -> dbu 15-20
    // micron) without touching PIN A or the OBS rect.
    le_mouse_down(handle, 150, 0);
    le_mouse_up(handle, 200, 200);
    ASSERT_EQ(le_selection_count(handle), 2);
    EXPECT_EQ(le_selected_object_ref(handle, 0).kind, LE_OBJECT_KIND_SHAPE);
    EXPECT_EQ(le_selected_object_ref(handle, 1).kind, LE_OBJECT_KIND_SHAPE);

    const LeObjectRef first_ref = le_selected_object_ref(handle, 0);
    const std::map<std::string, LeProperty> first_properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(first_properties.contains("rects"));
    const std::string first_rects = first_properties.at("rects").string_value;

    const std::map<std::string, LeProperty> second_properties = selected_object_properties(handle, 1);
    ASSERT_TRUE(second_properties.contains("rects"));
    const std::string second_rects = second_properties.at("rects").string_value;

    EXPECT_NE(first_rects, second_rects); // each port's own rect, not the aggregate
    EXPECT_EQ(first_rects, "{{16 1} {19 4}}");
    EXPECT_EQ(second_rects, "{{16 16} {19 19}}");
    // Shape.layer is a reference field now (LeLayerId, not a plain str) -
    // le_shape_layer_name is the real, resolved accessor (see the click-
    // select test's own comment on why the generic property table's own
    // "layer" row isn't checked directly here).
    EXPECT_STREQ(le_shape_layer_name(handle, LeShapeId{.index = first_ref.index, .generation = first_ref.generation}), "M1"); // a piece-level selection always reports its own layer
}

TEST_F(ApiFixture, ShiftClickingTwoPiecesOfTheSameTerminalSelectsBothIndependently)
{
    // The actual reported bug: shift-clicking a second shape within the
    // same Terminal must add a second, independently-selected/reportable
    // entry, not replace or no-op against the first one.
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 175, 175); // PIN B's first port
    le_mouse_up(handle, 175, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_down(handle, 175, 25); // PIN B's second port
    le_mouse_up(handle, 175, 25);
    le_key_up(handle, LE_KEY_SHIFT);
    ASSERT_EQ(le_selection_count(handle), 2); // both pieces, not one

    EXPECT_EQ(le_selected_object_ref(handle, 0).kind, LE_OBJECT_KIND_SHAPE);
    EXPECT_EQ(le_selected_object_ref(handle, 1).kind, LE_OBJECT_KIND_SHAPE);

    // Copy each rects value out as its own std::string immediately -
    // LeProperty::string_value is only valid until the next
    // le_object_property_at()/_count() call *for the same ref* (see its
    // doc comment); querying a different index (as `second`'s query
    // below does) invalidates `first`'s pointers.
    const std::map<std::string, LeProperty> first_properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(first_properties.contains("rects"));
    const std::string first_rects = first_properties.at("rects").string_value;

    const std::map<std::string, LeProperty> second_properties = selected_object_properties(handle, 1);
    ASSERT_TRUE(second_properties.contains("rects"));
    const std::string second_rects = second_properties.at("rects").string_value;

    EXPECT_NE(first_rects, second_rects); // genuinely distinct pieces
    EXPECT_EQ(first_rects, "{{16 1} {19 4}}");
    EXPECT_EQ(second_rects, "{{16 16} {19 19}}");
}

TEST_F(ApiFixture, ConcurrentRenderAndMousePositionCallsOnTheSameHandleDoNotCrash)
{
    // Regression: le_render_pixel_buffer (called by Flutter's own raster
    // thread, via FlutterTexture.copyPixelBuffer(), once per frame) and
    // le_set_mouse_position/le_zoom (called by the platform thread on
    // every pointer event) both run Pipeline::run() on the same handle -
    // a real crash (concurrent, unsynchronized std::map mutation inside
    // Pipeline::filter_by_layer_visibility) shipped from exactly this
    // pattern. Every LeHandle-touching function now locks the handle's
    // own mutex; this drives both call paths concurrently, repeatedly,
    // and must complete without crashing, deadlocking, or hanging.
    ASSERT_EQ(le_read_lef(handle, generate_concurrency_stress_lef(3000).c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    le_set_viewport_size(handle, 200, 200);

    constexpr int kIterations = 50;
    std::thread render_thread([&]
                               {
        for (int i = 0; i < kIterations; ++i)
        {
            // Alternates a no-op-ish zoom so viewport_version() keeps
            // changing, forcing Pipeline::run() to actually recompute
            // (not just return an already-cached result) on most calls -
            // the same "real work, not a cache hit" condition the
            // original crash needed to manifest.
            le_zoom(handle, (i % 2 == 0) ? 0.001 : -0.001, 100, 100);
            const LePixelBuffer buffer = le_render_pixel_buffer(handle);
            EXPECT_NE(buffer.data, nullptr);
        } });

    for (int i = 0; i < kIterations; ++i)
        le_set_mouse_position(handle, i % 200, (i * 7) % 200);

    render_thread.join();
}

// --- Terminal CRUD + filter-search (UPDATES.md item 15 / TCL_EXPLORATION.md
// Phase 4) ---

namespace
{
    LeAbstractId testcell_abstract_id(LeHandle *handle)
    {
        return le_library_design_at(handle, 0, 0).abstract_id;
    }
}

TEST_F(ApiFixture, CreateTerminalWithNullHandleOrNameReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    EXPECT_EQ(le_create_terminal(nullptr, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0).index, UINT32_MAX);
    EXPECT_EQ(le_create_terminal(handle, abstract_id, nullptr, "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0).index, UINT32_MAX);
}

TEST_F(ApiFixture, CreateTerminalWithUnknownAbstractIdReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId bogus{.index = UINT32_MAX, .generation = 0};

    EXPECT_EQ(le_create_terminal(handle, bogus, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0).index, UINT32_MAX);
}

TEST_F(ApiFixture, CreateTerminalSucceedsAndIsReadableViaProperties)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    const LeTerminalId id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(id.index, UINT32_MAX);

    const int32_t count = le_terminal_property_count(handle, id);
    ASSERT_GT(count, 0);

    bool found_name = false, found_direction = false, found_port_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const LeProperty property = le_terminal_property_at(handle, id, i);
        ASSERT_NE(property.name, nullptr);
        if (std::string(property.name) == "name")
        {
            found_name = true;
            EXPECT_STREQ(property.string_value, "IN0");
        }
        else if (std::string(property.name) == "direction")
        {
            found_direction = true;
            EXPECT_STREQ(property.string_value, "INPUT");
        }
        else if (std::string(property.name) == "ports_count")
        {
            found_port_count = true;
            EXPECT_EQ(property.int_value, 0); // no ports created yet
        }
    }
    EXPECT_TRUE(found_name);
    EXPECT_TRUE(found_direction);
    EXPECT_TRUE(found_port_count);
}

TEST_F(ApiFixture, CreateTerminalWithADuplicateNameOnTheSameAbstractFailsButDifferentAbstractsBothSucceed)
{
    // Root::create_terminal is fallible (Terminal.name is unique_per_parent,
    // scoped to Abstract) - le_create_terminal surfaces that as an invalid
    // id, same as any other failure. Two different Abstracts legitimately
    // reuse the same pin name (e.g. VDD/IN0 across a library's cells), so
    // that must keep succeeding.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_read_lef(handle, fixture_path("othercell.lef").c_str()), 0);
    const LeAbstractId testcell_id = testcell_abstract_id(handle);
    const LeAbstractId othercell_id = le_library_design_at(handle, 1, 0).abstract_id;
    ASSERT_NE(testcell_id.index, UINT32_MAX);
    ASSERT_NE(othercell_id.index, UINT32_MAX);

    const LeTerminalId first = le_create_terminal(handle, testcell_id, "DUPTEST", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(first.index, UINT32_MAX);

    const LeTerminalId duplicate = le_create_terminal(handle, testcell_id, "DUPTEST", "OUTPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    EXPECT_EQ(duplicate.index, UINT32_MAX);

    const LeTerminalId other_abstract = le_create_terminal(handle, othercell_id, "DUPTEST", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    EXPECT_NE(other_abstract.index, UINT32_MAX);
}

TEST_F(ApiFixture, UpdateTerminalNameToADuplicateOnTheSameAbstractFailsButRenamingToItsOwnCurrentNameSucceeds)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    const LeTerminalId first = le_create_terminal(handle, abstract_id, "FIRST", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalId second = le_create_terminal(handle, abstract_id, "SECOND", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(first.index, UINT32_MAX);
    ASSERT_NE(second.index, UINT32_MAX);

    EXPECT_NE(le_update_terminal(handle, second, 0, LeAbstractId{}, "FIRST", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);
    EXPECT_EQ(le_update_terminal(handle, second, 0, LeAbstractId{}, "SECOND", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);
}

TEST_F(ApiFixture, TerminalPropertyCountAndAtForUnknownIdDegradeGracefully)
{
    const LeTerminalId bogus{.index = UINT32_MAX, .generation = 0};
    EXPECT_EQ(le_terminal_property_count(handle, bogus), 0);

    const LeProperty property = le_terminal_property_at(handle, bogus, 0);
    EXPECT_EQ(property.name, nullptr);

    EXPECT_EQ(le_terminal_property_count(nullptr, bogus), 0);
}

TEST_F(ApiFixture, UpdateTerminalNameAndDirectionUpdateTheirProperties)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(id.index, UINT32_MAX);

    EXPECT_EQ(le_update_terminal(handle, id, 0, LeAbstractId{}, "IN0_RENAMED", "OUTPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);

    const int32_t count = le_terminal_property_count(handle, id);
    bool checked_name = false, checked_direction = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const LeProperty property = le_terminal_property_at(handle, id, i);
        if (std::string(property.name) == "name")
        {
            checked_name = true;
            EXPECT_STREQ(property.string_value, "IN0_RENAMED");
        }
        else if (std::string(property.name) == "direction")
        {
            checked_direction = true;
            EXPECT_STREQ(property.string_value, "OUTPUT");
        }
    }
    EXPECT_TRUE(checked_name);
    EXPECT_TRUE(checked_direction);
}

TEST_F(ApiFixture, UpdateTerminalWithNullHandleOrUnknownIdReturnsNonzero)
{
    const LeTerminalId bogus{.index = UINT32_MAX, .generation = 0};
    // le_update_terminal(handle, id, has_abstract, abstract_id, name,
    // direction, shape, use, must_join, net_expr, leq, taper_rule,
    // supply_sensitivity, ground_sensitivity, has_rise_slew_limit,
    // rise_slew_limit, has_fall_slew_limit, fall_slew_limit, has_max_load,
    // max_load, has_max_delay, max_delay) - see declarations.inc.
    EXPECT_NE(le_update_terminal(nullptr, bogus, 0, LeAbstractId{}, "X", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);
    EXPECT_NE(le_update_terminal(handle, bogus, 0, LeAbstractId{}, "X", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);
    EXPECT_NE(le_update_terminal(handle, bogus, 0, LeAbstractId{}, nullptr, "OUTPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0), 0);
}

TEST_F(ApiFixture, DeleteTerminalRemovesItAndIsIdempotentlySafeAfterwards)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(id.index, UINT32_MAX);

    EXPECT_EQ(le_delete_terminal(handle, id), 0);
    EXPECT_EQ(le_terminal_property_count(handle, id), 0);
    // Already deleted, and a never-created id - neither crashes.
    EXPECT_NE(le_delete_terminal(handle, id), 0);
    EXPECT_NE(le_delete_terminal(handle, LeTerminalId{.index = UINT32_MAX, .generation = 0}), 0);
    EXPECT_NE(le_delete_terminal(nullptr, id), 0);
}

TEST_F(ApiFixture, BuildingALibraryDesignAbstractFromScratchAndSelectingItWithLeSetCurrentAbstractLetsCreateTerminalPortResolveTheTerminal)
{
    // Regression: le_terminal_by_name (Terminal's own hand-written
    // friendly-id resolver - unique_per_parent means it can't use the
    // generated by-name lookup pair, see Field.unique_per_parent's own
    // docstring) used to read handle->scene.current_abstract() - a
    // separate GUI-rendering "current view" only ever moved as a side
    // effect of selecting a Design (le_set_current_design_abstract/
    // le_set_current_design_abstract_by_id) - instead of
    // handle->current_abstract_id, the same field le_set_current_abstract
    // itself sets and le_get_terminals' own default scope already
    // derives from. A script with no LEF-loaded Design to select at all
    // - building a brand new Library/Design/Abstract purely through
    // create_<type> calls and selecting the Abstract directly via
    // le_set_current_abstract, exactly this test's own flow - hit this:
    // create_terminal_port -terminal $in failed with "unknown terminal"
    // even though the Terminal it just created was right there,
    // findable via get_terminals. No LEF file involved at all, unlike
    // every other ApiFixture test above, which is the point.
    const LeLibraryId library_id = le_create_library(handle, "LIB");
    ASSERT_NE(library_id.index, UINT32_MAX);
    const LeDesignId design_id = le_create_design(handle, library_id, "CELL");
    ASSERT_NE(design_id.index, UINT32_MAX);
    // create_abstract's own -size/-origin/-bbox (le_create_abstract's
    // has_size/has_origin/has_bbox) are all dbu-using compound fields -
    // Klass.create_api_body() fetches dbu_per_um unconditionally
    // whenever a class has any such field, regardless of whether it was
    // actually provided (see Field.cmd_uses_dbu()'s own docstring), so a
    // Technology has to exist even though every one of them is omitted
    // (has_*=0) below - matches the original bug report's own script,
    // which also read_lef's a tech file before building anything.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = le_create_abstract(
        handle, design_id, nullptr, 0, 0.0, 0.0, 0, 0.0, 0.0, 0, 0.0, 0.0, 0.0, 0.0, 0, 0, 0, 0, nullptr, nullptr, nullptr, 0, 0.0, nullptr, 0);
    ASSERT_NE(abstract_id.index, UINT32_MAX);

    ASSERT_EQ(le_set_current_abstract(handle, abstract_id), 0);

    const LeTerminalId in_id = le_create_terminal(handle, abstract_id, "IN", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_NE(in_id.index, UINT32_MAX);

    // The bug: this returned an invalid id (scene.current_abstract() was
    // never set, only current_abstract_id was) even though the Terminal
    // genuinely exists under the currently-selected Abstract.
    EXPECT_EQ(le_terminal_by_name(handle, "IN").index, in_id.index);

    // create_terminal_port_cmd (le_tcl_shim.cpp) resolves its own
    // -terminal token through exactly this same le_terminal_by_name path
    // (resolve_terminal_id) - the actual command the user-reported script
    // called.
    const LeTerminalPortId port_id = le_create_terminal_port(handle, in_id, nullptr);
    EXPECT_NE(port_id.index, UINT32_MAX);
}

TEST_F(ApiFixture, SearchTerminalFindsMatchesByFilterExpression)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId in0 = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalId in1 = le_create_terminal(handle, abstract_id, "IN1", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    le_create_terminal(handle, abstract_id, "OUT0", "OUTPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);

    const int32_t count = le_search_terminal(handle, ".name =~ IN*");
    ASSERT_EQ(count, 2);

    std::vector<uint32_t> found;
    for (int32_t i = 0; i < count; ++i)
        found.push_back(le_search_result_terminal_at(handle, i).index);
    EXPECT_NE(std::find(found.begin(), found.end(), in0.index), found.end());
    EXPECT_NE(std::find(found.begin(), found.end(), in1.index), found.end());

    EXPECT_EQ(le_search_terminal(handle, ".direction == OUTPUT"), 1);
    EXPECT_EQ(le_search_terminal(handle, ".name == DOES_NOT_EXIST"), 0);
}

TEST_F(ApiFixture, SearchTerminalWithBadFilterExpressionReturnsNegativeOneAndPushesAMessage)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const int32_t before = le_message_count(handle);

    EXPECT_EQ(le_search_terminal(handle, "not a filter expression"), -1);
    EXPECT_GT(le_message_count(handle), before);

    EXPECT_EQ(le_search_terminal(nullptr, ".name == X"), 0);
    EXPECT_EQ(le_search_terminal(handle, nullptr), 0);
}

TEST_F(ApiFixture, SearchResultTerminalAtOutOfRangeReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_search_terminal(handle, ".name == DOES_NOT_EXIST"), 0);

    EXPECT_EQ(le_search_result_terminal_at(handle, 0).index, UINT32_MAX);
    EXPECT_EQ(le_search_result_terminal_at(handle, -1).index, UINT32_MAX);
    EXPECT_EQ(le_search_result_terminal_at(nullptr, 0).index, UINT32_MAX);
}

// --- TerminalPort/Obstruction CRUD + filter-search, and Abstract boundary
// update (Phase 4, continued) ---

namespace
{
    constexpr double kRect0[] = {0.1, 0.1, 0.3, 0.4}; // matches UPDATES.md item 15's own example verbatim

    // Shape.layer is now a required, resolved LeLayerId (readers error
    // rather than create a Shape with an unresolved layer - see its own
    // schema.py comment) - this test file used to pass arbitrary "M4"/
    // "M5"/"M6" strings straight through when Shape.layer_name was a free
    // string; those names aren't declared by testcell.lef (only "M1" is),
    // so this resolves-or-creates a real Layer under the fixture's own
    // Technology to stand in for them. Idempotent (le_layer_by_name first)
    // since several tests request the same name more than once.
    LeLayerId named_layer(LeHandle *handle, const char *name)
    {
        const LeLayerId existing = le_layer_by_name(handle, name);
        if (existing.index != UINT32_MAX)
            return existing;
        return le_create_layer(handle, le_technology_id(handle), name, "ROUTING", "NONE",
                                0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0,
                                0, 0, 0, 0.0, 0.0, 0, 0.0, 0.0, 0, 0.0, 0, 0.0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0,
                                0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0.0, 0, 0.0);
    }

    // Composes le_create_terminal_port + le_create_shape (with its own
    // -rects-equivalent has_rects/rects_flat_um/rects_flat_count slots,
    // rather than a separate le_add_shape_rect call - that command no
    // longer exists, geometry is provided at create_shape/update_shape
    // time now) - the common case most tests below want (a port with
    // exactly one rect shape), without re-typing the sequence in every
    // test. Tests that specifically exercise one of those calls' own
    // validation call them directly instead.
    LeTerminalPortId create_terminal_port_with_rect(LeHandle *handle, LeTerminalId terminal_id, const char *layer_name, const double rect_um[4])
    {
        const LeTerminalPortId port_id = le_create_terminal_port(handle, terminal_id, nullptr);
        le_create_shape(handle, port_id, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, layer_name), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect_um, 4, 0, 0.0, 0, 0.0, 0);
        return port_id;
    }

    LeObstructionId create_obstruction_with_rect(LeHandle *handle, LeAbstractId abstract_id, const char *layer_name, const double rect_um[4])
    {
        const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);
        le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, layer_name), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect_um, 4, 0, 0.0, 0, 0.0, 0);
        return obstruction_id;
    }
}

TEST_F(ApiFixture, CreateTerminalPortWithNullHandleOrUnknownTerminalReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_EQ(le_create_terminal_port(nullptr, LeTerminalId{.index = UINT32_MAX, .generation = 0}, nullptr).index, UINT32_MAX);
    EXPECT_EQ(le_create_terminal_port(handle, LeTerminalId{.index = UINT32_MAX, .generation = 0}, nullptr).index, UINT32_MAX);
}

TEST_F(ApiFixture, CreateTerminalPortSucceedsAndIsReadableViaProperties)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId terminal_id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);

    const LeTerminalPortId port_id = create_terminal_port_with_rect(handle, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);

    const int32_t count = le_terminal_port_property_count(handle, port_id);
    bool found_shapes_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const LeProperty property = le_terminal_port_property_at(handle, port_id, i);
        if (std::string(property.name) == "shapes_count")
        {
            found_shapes_count = true;
            EXPECT_EQ(property.int_value, 1);
        }
    }
    EXPECT_TRUE(found_shapes_count);

    // The Terminal's own ports_count now reflects the new port.
    const int32_t terminal_property_count = le_terminal_property_count(handle, terminal_id);
    for (int32_t i = 0; i < terminal_property_count; ++i)
    {
        const LeProperty property = le_terminal_property_at(handle, terminal_id, i);
        if (std::string(property.name) == "ports_count")
            EXPECT_EQ(property.int_value, 1);
    }
}

TEST_F(ApiFixture, DeleteTerminalPortCascadesToItsShapesAndIsIdempotentlySafeAfterwards)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId terminal_id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalPortId port_id = create_terminal_port_with_rect(handle, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);
    const LeShapeId shape_id = le_terminal_port_shape_at(handle, port_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(le_delete_terminal_port(handle, port_id), 0);
    EXPECT_EQ(le_terminal_port_property_count(handle, port_id), 0);
    // Cascade: the shape it owned is gone too, not left as unreachable garbage.
    EXPECT_EQ(le_shape_layer_name(handle, shape_id), nullptr);
    EXPECT_NE(le_delete_terminal_port(handle, port_id), 0);
    EXPECT_NE(le_delete_terminal_port(nullptr, port_id), 0);
}

TEST_F(ApiFixture, SearchTerminalPortFindsMatchesUsingUpdatesMdItem15SExampleExpression)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId in0 = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalId out0 = le_create_terminal(handle, abstract_id, "OUT0", "OUTPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalPortId matching_port = create_terminal_port_with_rect(handle, in0, "M4", kRect0);
    create_terminal_port_with_rect(handle, in0, "M5", kRect0);  // wrong layer
    create_terminal_port_with_rect(handle, out0, "M4", kRect0); // wrong terminal name
    ASSERT_NE(matching_port.index, UINT32_MAX);

    const int32_t count = le_search_terminal_port(handle, ".terminal.name =~ IN* && .shapes.layer.name == M4");
    ASSERT_EQ(count, 1);
    EXPECT_EQ(le_search_result_terminal_port_at(handle, 0).index, matching_port.index);
}

TEST_F(ApiFixture, SearchTerminalPortWithBadFilterExpressionReturnsNegativeOne)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_search_terminal_port(handle, "not a filter expression"), -1);
}

TEST_F(ApiFixture, CreateObstructionWithNullHandleOrUnknownAbstractReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_EQ(le_create_obstruction(nullptr, LeAbstractId{.index = UINT32_MAX, .generation = 0}).index, UINT32_MAX);
    EXPECT_EQ(le_create_obstruction(handle, LeAbstractId{.index = UINT32_MAX, .generation = 0}).index, UINT32_MAX);
}

TEST_F(ApiFixture, CreateObstructionSucceedsAndIsReadableViaProperties)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    const LeObstructionId id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    ASSERT_NE(id.index, UINT32_MAX);

    const int32_t count = le_obstruction_property_count(handle, id);
    bool found_shapes_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const LeProperty property = le_obstruction_property_at(handle, id, i);
        if (std::string(property.name) == "shapes_count")
        {
            found_shapes_count = true;
            EXPECT_EQ(property.int_value, 1);
        }
    }
    EXPECT_TRUE(found_shapes_count);
}

TEST_F(ApiFixture, DeleteObstructionCascadesToItsShapesAndIsIdempotentlySafeAfterwards)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    ASSERT_NE(id.index, UINT32_MAX);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(le_delete_obstruction(handle, id), 0);
    EXPECT_EQ(le_obstruction_property_count(handle, id), 0);
    EXPECT_EQ(le_shape_layer_name(handle, shape_id), nullptr);
    EXPECT_NE(le_delete_obstruction(handle, id), 0);
    EXPECT_NE(le_delete_obstruction(nullptr, id), 0);
}

TEST_F(ApiFixture, SearchObstructionFindsMatchesByLayerName)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId matching = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    create_obstruction_with_rect(handle, abstract_id, "M5", kRect0);
    ASSERT_NE(matching.index, UINT32_MAX);

    const int32_t count = le_search_obstruction(handle, ".shapes.layer.name == M4");
    ASSERT_EQ(count, 1);
    EXPECT_EQ(le_search_result_obstruction_at(handle, 0).index, matching.index);
}

// --- Shape CRUD, addressed by a stable id (Phase 4, continued). Rects,
// polygons, and paths are all created/read/removed via their own
// symmetric set of calls - none baked into creation, matching the design
// decision recorded in TCL_EXPLORATION.md. ---

TEST_F(ApiFixture, CreateShapeWithNullHandleOrUnknownLayerOrUnknownParentReturnsInvalidId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId terminal_id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalPortId port_id = le_create_terminal_port(handle, terminal_id, nullptr);
    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);
    const LeLayerId m4 = named_layer(handle, "M4");
    // layer is optional now (Shape.purpose's own schema.py comment) - the
    // default-invalid sentinel means "omitted", not "unknown", so this
    // needs a syntactically-plausible id that just doesn't exist, to
    // actually exercise the "unknown layer" rejection path.
    const LeLayerId no_such_layer{.index = 999, .generation = 0};

    EXPECT_EQ(le_create_shape(nullptr, port_id, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, m4, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);
    EXPECT_EQ(le_create_shape(handle, port_id, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, no_such_layer, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, m4, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);

    EXPECT_EQ(le_create_shape(nullptr, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, m4, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, no_such_layer, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, LeObstructionId{.index = UINT32_MAX, .generation = 0}, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, m4, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);
}

TEST_F(ApiFixture, TerminalPortShapeCountAndAtEnumerateAndReadBackWhatWasCreated)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeTerminalId terminal_id = le_create_terminal(handle, abstract_id, "IN0", "INPUT", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0);
    const LeTerminalPortId port_id = create_terminal_port_with_rect(handle, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);

    ASSERT_EQ(le_terminal_port_shape_count(handle, port_id), 1);
    const LeShapeId shape_id = le_terminal_port_shape_at(handle, port_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M4");
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 1);
    const LeRectUm rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.1);
    EXPECT_DOUBLE_EQ(rect.ll_y_um, 0.1);
    EXPECT_DOUBLE_EQ(rect.ur_x_um, 0.3);
    EXPECT_DOUBLE_EQ(rect.ur_y_um, 0.4);

    EXPECT_EQ(le_terminal_port_shape_at(handle, port_id, 1).index, UINT32_MAX);
}

TEST_F(ApiFixture, ObstructionShapeCountAndAtEnumerateAndReadBackWhatWasCreated)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M5", kRect0);
    ASSERT_NE(obstruction_id.index, UINT32_MAX);

    ASSERT_EQ(le_obstruction_shape_count(handle, obstruction_id), 1);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M5");
}

TEST_F(ApiFixture, UpdateShapeLayerNameRenamesWithoutTouchingGeometryOrParent)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    const LeLayerId m6 = named_layer(handle, "M6");
    EXPECT_EQ(le_update_shape(handle, shape_id, 1, m6, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0, 0), 0);

    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M6");
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 1); // untouched
    const LeRectUm rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.1); // untouched

    // Same shape id, same parent.
    EXPECT_EQ(le_obstruction_shape_count(handle, obstruction_id), 1);
    EXPECT_EQ(le_obstruction_shape_at(handle, obstruction_id, 0).index, shape_id.index);
}

TEST_F(ApiFixture, UpdateShapeLayerNameWithNullHandleOrUnknownIdReturnsNonzero)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    const LeLayerId m6 = named_layer(handle, "M6");

    EXPECT_NE(le_update_shape(nullptr, shape_id, 1, m6, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0, 0), 0);
    EXPECT_NE(le_update_shape(handle, LeShapeId{.index = UINT32_MAX, .generation = 0}, 1, m6, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0, 0), 0);

    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M4"); // untouched
}

TEST_F(ApiFixture, CreateShapeWithRectsThenUpdateReplacesThemAndRemoveShapeRectWorks)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);

    // -rects' own wire encoding needs no per-record length prefix (every
    // Rect is always exactly 4 numbers - see Field.list_compound_kind()'s
    // own "flat" docstring), just the coordinates back to back.
    constexpr double rects[] = {0.1, 0.1, 0.3, 0.4, 1.0, 1.0, 2.0, 2.0};
    const LeShapeId shape_id = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M4"), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rects, 8, 0, 0.0, 0, 0.0, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 2);
    EXPECT_DOUBLE_EQ(le_shape_rect_at(handle, shape_id, 1).ur_x_um, 2.0);

    // Malformed -rects encoding (not a multiple of 4) is rejected.
    constexpr double malformed[] = {0.1, 0.1, 0.3};
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M5"), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, malformed, 3, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);

    // update_shape's own -rects flag replaces the whole list, not appends.
    constexpr double replacement[] = {5.0, 5.0, 6.0, 6.0};
    EXPECT_EQ(le_update_shape(handle, shape_id, 0, LeLayerId{.index = UINT32_MAX, .generation = 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, replacement, 4, 0, 0.0, 0, 0.0, 0, 0), 0);
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 1);
    EXPECT_DOUBLE_EQ(le_shape_rect_at(handle, shape_id, 0).ur_x_um, 6.0);

    EXPECT_NE(le_remove_shape_rect(nullptr, shape_id, 0), 0);
    EXPECT_NE(le_remove_shape_rect(handle, shape_id, 5), 0);
    EXPECT_EQ(le_remove_shape_rect(handle, shape_id, 0), 0);
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 0);
}

TEST_F(ApiFixture, CreateShapeWithPolygonThenRemoveShapePolygonWorks)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);

    // -polygons' own wire encoding (Field.list_compound_kind()'s "points"
    // shape): [record_count, (per record: point_count, x,y,x,y,...)] -
    // each polygon's own point count varies, so it's prefixed per record,
    // unlike -rects' fixed 4-per-record shape above.
    constexpr double triangle[] = {1, 3, 0.0, 0.0, 1.0, 0.0, 0.5, 1.0};
    const LeShapeId shape_id = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M4"), nullptr, 0, nullptr, 0, 1, triangle, 8, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(le_shape_polygon_count(handle, shape_id), 1);
    ASSERT_EQ(le_shape_polygon_point_count(handle, shape_id, 0), 3);
    const LePointUm p1 = le_shape_polygon_point_at(handle, shape_id, 0, 1);
    EXPECT_DOUBLE_EQ(p1.x_um, 1.0);
    EXPECT_DOUBLE_EQ(p1.y_um, 0.0);
    EXPECT_EQ(le_shape_polygon_point_at(handle, shape_id, 0, 5).x_um, 0.0); // out of range -> zeroed

    // Malformed: point_count claims 3 but only 2 points of data follow.
    constexpr double truncated[] = {1, 3, 0.0, 0.0, 1.0, 0.0};
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M5"), nullptr, 0, nullptr, 0, 1, truncated, 6, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);

    // Malformed: point_count below the minimum of 2.
    constexpr double too_few[] = {1, 1, 0.0, 0.0};
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M6"), nullptr, 0, nullptr, 0, 1, too_few, 4, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);

    EXPECT_NE(le_remove_shape_polygon(handle, shape_id, 5), 0);
    EXPECT_EQ(le_remove_shape_polygon(handle, shape_id, 0), 0);
    EXPECT_EQ(le_shape_polygon_count(handle, shape_id), 0);
}

TEST_F(ApiFixture, CreateShapeWithPathThenRemoveShapePathWorks)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);

    // -paths' own wire encoding (Field.list_compound_kind()'s
    // "points_plus_scalars" shape): [record_count, (per record: width_um,
    // point_count, x,y,x,y,...)] - width prefixes the point list, since
    // Path = {polygon: Polygon, width: dbu} has one scalar sibling
    // alongside its own point-list field.
    constexpr double centerline[] = {1, 0.1, 2, 0.0, 0.0, 1.0, 0.0};
    const LeShapeId shape_id = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M4"), nullptr, 1, centerline, 7, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(le_shape_path_count(handle, shape_id), 1);
    EXPECT_DOUBLE_EQ(le_shape_path_width_um(handle, shape_id, 0), 0.1);
    ASSERT_EQ(le_shape_path_point_count(handle, shape_id, 0), 2);
    const LePointUm p0 = le_shape_path_point_at(handle, shape_id, 0, 0);
    const LePointUm p1 = le_shape_path_point_at(handle, shape_id, 0, 1);
    EXPECT_DOUBLE_EQ(p0.x_um, 0.0);
    EXPECT_DOUBLE_EQ(p1.x_um, 1.0);

    // Malformed: point_count claims 2 but only 1 point of data follows.
    constexpr double truncated[] = {1, 0.1, 2, 0.0, 0.0};
    EXPECT_EQ(le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M5"), nullptr, 1, truncated, 5, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index, UINT32_MAX);

    EXPECT_NE(le_remove_shape_path(handle, shape_id, 5), 0);
    EXPECT_EQ(le_remove_shape_path(handle, shape_id, 0), 0);
    EXPECT_EQ(le_shape_path_count(handle, shape_id), 0);
}

TEST_F(ApiFixture, DeleteShapeRemovesItAndParentCountDropsButParentSurvives)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(le_delete_shape(handle, shape_id), 0);

    EXPECT_EQ(le_obstruction_shape_count(handle, obstruction_id), 0);
    EXPECT_EQ(le_shape_layer_name(handle, shape_id), nullptr);
    EXPECT_EQ(le_obstruction_property_count(handle, obstruction_id), 1); // Obstruction itself still exists (shapes_count now 0)
    EXPECT_NE(le_delete_shape(handle, shape_id), 0);
    EXPECT_NE(le_delete_shape(nullptr, shape_id), 0);
}

TEST_F(ApiFixture, ShapeAccessorsWithNullHandleOrUnknownIdDegradeGracefully)
{
    const LeShapeId bogus{.index = UINT32_MAX, .generation = 0};
    const LeTerminalPortId bogus_port{.index = UINT32_MAX, .generation = 0};
    const LeObstructionId bogus_obstruction{.index = UINT32_MAX, .generation = 0};

    EXPECT_EQ(le_terminal_port_shape_count(handle, bogus_port), 0);
    EXPECT_EQ(le_terminal_port_shape_at(handle, bogus_port, 0).index, UINT32_MAX);
    EXPECT_EQ(le_obstruction_shape_count(handle, bogus_obstruction), 0);
    EXPECT_EQ(le_obstruction_shape_at(handle, bogus_obstruction, 0).index, UINT32_MAX);
    EXPECT_EQ(le_shape_layer_name(handle, bogus), nullptr);
    EXPECT_EQ(le_shape_layer_name(nullptr, bogus), nullptr);

    EXPECT_EQ(le_shape_rect_count(handle, bogus), 0);
    const LeRectUm rect = le_shape_rect_at(handle, bogus, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.0);
    EXPECT_DOUBLE_EQ(rect.ur_y_um, 0.0);

    EXPECT_EQ(le_shape_polygon_count(handle, bogus), 0);
    EXPECT_EQ(le_shape_polygon_point_count(handle, bogus, 0), 0);
    const LePointUm polygon_point = le_shape_polygon_point_at(handle, bogus, 0, 0);
    EXPECT_DOUBLE_EQ(polygon_point.x_um, 0.0);

    EXPECT_EQ(le_shape_path_count(handle, bogus), 0);
    EXPECT_DOUBLE_EQ(le_shape_path_width_um(handle, bogus, 0), 0.0);
    EXPECT_EQ(le_shape_path_point_count(handle, bogus, 0), 0);
    const LePointUm path_point = le_shape_path_point_at(handle, bogus, 0, 0);
    EXPECT_DOUBLE_EQ(path_point.x_um, 0.0);
}

// --- Editing / undo-redo (UPDATES.md item 21) ---

TEST_F(ApiFixture, SelectAllInEditModeIsANoOpEvenWithCtrlHeld)
{
    load_two_shapes_at_known_scale(handle);
    le_set_mode(handle, LE_MODE_EDIT);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, DeselectAllInEditModeIsANoOpEvenWithCtrlHeld)
{
    load_two_shapes_at_known_scale(handle);
    le_mouse_down(handle, 25, 175); // PIN A, in the default Select mode
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_set_mode(handle, LE_MODE_EDIT);
    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_DESELECT_ALL);

    EXPECT_EQ(le_selection_count(handle), 1); // untouched
}

TEST_F(ApiFixture, UndoRedoRoundTripsAGeneratedUpdateCallThroughBeginEndCommand)
{
    // Confirms the generated recording hook actually fires end-to-end
    // through the real le_update_shape entry point, not just at the
    // Root layer directly (see editing_test.cpp for that).
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(le_can_undo(handle), 0);

    // named_layer's own le_create_layer must run *before* le_begin_command
    // - a Layer created while a command is recording would itself become
    // an undo/redo step in the same transaction, and Pool::create() always
    // allocates a fresh generation on redo (never reproduces the original
    // id - see IdCellPtr's own comment), so the update_shape step's own
    // "after" snapshot (a plain copy of the LayerId, not a repointing
    // IdCellPtr reference - that indirection is only built for a create/
    // delete step's own id) would go stale the moment the transaction's
    // own Layer-create step is undone and redone alongside it.
    const LeLayerId m6 = named_layer(handle, "M6");

    le_begin_command(handle, "update_shape test");
    EXPECT_EQ(le_update_shape(handle, shape_id, 1, m6, nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0, 0), 0);
    le_end_command(handle, 1);

    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M6");
    EXPECT_NE(le_can_undo(handle), 0);
    EXPECT_EQ(le_can_redo(handle), 0);

    EXPECT_NE(le_undo(handle), 0);
    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M4"); // restored
    EXPECT_EQ(le_can_undo(handle), 0);
    EXPECT_NE(le_can_redo(handle), 0);

    EXPECT_NE(le_redo(handle), 0);
    EXPECT_STREQ(le_shape_layer_name(handle, shape_id), "M6");
    EXPECT_EQ(le_undo(nullptr), 0);
    EXPECT_EQ(le_redo(nullptr), 0);
}

TEST_F(ApiFixture, CommandHistoryOnlyRecordsSuccessfulCommands)
{
    EXPECT_EQ(le_command_history_count(handle), 0);

    le_begin_command(handle, "ok_command");
    le_end_command(handle, 1);
    ASSERT_EQ(le_command_history_count(handle), 1);
    EXPECT_STREQ(le_command_history_at(handle, 0), "ok_command");

    le_begin_command(handle, "bad_command");
    le_end_command(handle, 0);
    EXPECT_EQ(le_command_history_count(handle), 1); // unchanged

    EXPECT_EQ(le_command_history_at(handle, 1), nullptr); // out of range
    EXPECT_EQ(le_command_history_at(handle, -1), nullptr);
}

TEST_F(ApiFixture, EndCommandWithNoBeginCommandIsANoOp)
{
    le_end_command(handle, 1);
    EXPECT_EQ(le_command_history_count(handle), 0);
    EXPECT_EQ(le_can_undo(handle), 0);
}

TEST_F(ApiFixture, EditingFunctionsWithNullHandleDoNotCrash)
{
    le_begin_command(nullptr, "x");
    le_end_command(nullptr, 1);
    EXPECT_EQ(le_undo(nullptr), 0);
    EXPECT_EQ(le_redo(nullptr), 0);
    EXPECT_EQ(le_can_undo(nullptr), 0);
    EXPECT_EQ(le_can_redo(nullptr), 0);
    EXPECT_EQ(le_command_history_count(nullptr), 0);
    EXPECT_EQ(le_command_history_at(nullptr, 0), nullptr);
    le_select_all(nullptr);
    le_deselect_all(nullptr);
    le_arm_move(nullptr);
    le_cancel_move(nullptr);
    EXPECT_EQ(le_is_move_armed(nullptr), 0);
}

TEST_F(ApiFixture, MoveTranslatesSelectedShapeGeometryAndIsUndoable)
{
    // Own obstruction+rect at a known dbu location (testcell.lef is
    // DATABASE MICRONS 1000, so kRect0 (0.1,0.1)-(0.3,0.4) um is
    // (100,100)-(300,400) dbu), rather than the LEF-fixture pins
    // load_two_shapes_at_known_scale sets up, so the exact rect can be
    // read back and compared by value below.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);
    const LeObstructionId obstruction_id = create_obstruction_with_rect(handle, abstract_id, "M4", kRect0);
    const LeShapeId shape_id = le_obstruction_shape_at(handle, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    // scale 0.1 (10 dbu/px), pan (0,0) - "anchor at the bottom-left image
    // corner" trick (see load_two_shapes_at_known_scale's own comment):
    // le_zoom's factor is multiplicative (new_scale = old_scale*(1+factor)),
    // so -0.9 takes the default scale 1.0 to 0.1; anchoring at (0,
    // viewport_height) - dbu (0,0) at the already-(0,0) pan - keeps pan
    // pinned at (0,0) across the zoom.
    le_set_viewport_size(handle, 200, 200);
    le_zoom(handle, -0.9, 0, 200);

    // dbu (200,250) - inside the (100,100)-(300,400) rect - is device
    // pixel (20,175) at this scale/pan (pre-flip: x_px=dbu_x*scale,
    // y_px=viewport_height-dbu_y*scale). Select it (default Select mode).
    le_mouse_down(handle, 20, 175);
    le_mouse_up(handle, 20, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_set_mode(handle, LE_MODE_EDIT);
    le_arm_move(handle);
    EXPECT_NE(le_is_move_armed(handle), 0);

    // First click sets the anchor at dbu (200,250) - same point as the
    // selecting click above (Move reads the *stored* mouse position, not
    // this call's own x/y - le_set_mouse_position must precede it, unlike
    // a plain Select-mode click - see le_mouse_up's own doc comment).
    le_set_mouse_position(handle, 20, 175);
    le_mouse_down(handle, 20, 175);
    le_mouse_up(handle, 20, 175);

    // Second click at dbu (250,260) - dx=50, dy=10, orthogonal delta
    // (50,0) dbu = (0.05,0) um - commits the move.
    le_set_mouse_position(handle, 25, 174);
    le_mouse_down(handle, 25, 174);
    le_mouse_up(handle, 25, 174);

    // Move stays armed after a successful commit (UPDATES.md item 21) -
    // ready for an immediate follow-up move on the same selection,
    // without re-arming, until Escape (see below).
    EXPECT_NE(le_is_move_armed(handle), 0);
    ASSERT_EQ(le_selection_count(handle), 1); // selection untouched by Move

    LeRectUm rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.15);
    EXPECT_DOUBLE_EQ(rect.ll_y_um, 0.1);
    EXPECT_DOUBLE_EQ(rect.ur_x_um, 0.35);
    EXPECT_DOUBLE_EQ(rect.ur_y_um, 0.4);

    // A follow-up move, with no re-arming call in between - proves Move
    // really does stay armed for another gesture. dbu (300,150) (device
    // (30,185)) is inside the moved rect; dbu (300,200) (device (30,180))
    // is a pure +50 dbu y-delta (dx=0, dy=50).
    le_set_mouse_position(handle, 30, 185);
    le_mouse_down(handle, 30, 185);
    le_mouse_up(handle, 30, 185);
    le_set_mouse_position(handle, 30, 180);
    le_mouse_down(handle, 30, 180);
    le_mouse_up(handle, 30, 180);

    EXPECT_NE(le_is_move_armed(handle), 0); // still armed
    rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.15);
    EXPECT_DOUBLE_EQ(rect.ll_y_um, 0.15);
    EXPECT_DOUBLE_EQ(rect.ur_x_um, 0.35);
    EXPECT_DOUBLE_EQ(rect.ur_y_um, 0.45);

    // Escape (LE_KEY_FINISH_RULER's handler) disarms it.
    le_key_down(handle, LE_KEY_FINISH_RULER);
    EXPECT_EQ(le_is_move_armed(handle), 0);

    // Undo unwinds the two moves LIFO - the second move first.
    ASSERT_NE(le_can_undo(handle), 0);
    ASSERT_NE(le_undo(handle), 0);
    rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.15); // back to after the first move only
    EXPECT_DOUBLE_EQ(rect.ll_y_um, 0.1);

    ASSERT_NE(le_undo(handle), 0);
    rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.1); // back to the original position
    EXPECT_DOUBLE_EQ(rect.ur_x_um, 0.3);

    ASSERT_NE(le_redo(handle), 0);
    ASSERT_NE(le_redo(handle), 0);
    rect = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll_x_um, 0.15); // both moves re-applied
    EXPECT_DOUBLE_EQ(rect.ll_y_um, 0.15);
}

TEST_F(ApiFixture, ArmMoveWithEmptySelectionOrOutsideEditModeIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    // Select mode, nothing selected.
    le_arm_move(handle);
    EXPECT_EQ(le_is_move_armed(handle), 0);

    // Selected, but still Select mode (not Edit).
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);
    le_arm_move(handle);
    EXPECT_EQ(le_is_move_armed(handle), 0);

    // Edit mode, but nothing selected.
    le_key_down(handle, LE_KEY_DESELECT_ALL); // not Ctrl-gated via direct call below
    le_deselect_all(handle);
    le_set_mode(handle, LE_MODE_EDIT);
    le_arm_move(handle);
    EXPECT_EQ(le_is_move_armed(handle), 0);
}

TEST_F(ApiFixture, CancelMoveViaEscapeClearsArmedState)
{
    load_two_shapes_at_known_scale(handle);
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    le_set_mode(handle, LE_MODE_EDIT);
    le_arm_move(handle);
    ASSERT_NE(le_is_move_armed(handle), 0);

    le_key_down(handle, LE_KEY_FINISH_RULER); // Escape - also cancels an in-progress move
    EXPECT_EQ(le_is_move_armed(handle), 0);
}

TEST_F(ApiFixture, KeyDownMoveWithoutCtrlOrWithShiftAlsoHeldIsANoOp)
{
    load_two_shapes_at_known_scale(handle);
    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    le_set_mode(handle, LE_MODE_EDIT);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_MOVE); // Ctrl never held
    EXPECT_EQ(le_is_move_armed(handle), 0);

    le_key_down(handle, LE_KEY_CTRL);
    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_MOVE); // Ctrl+Shift both held
    EXPECT_EQ(le_is_move_armed(handle), 0);

    le_key_up(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_MOVE); // Ctrl alone - actually arms
    EXPECT_NE(le_is_move_armed(handle), 0);
}

TEST_F(ApiFixture, ClickSelectsAndMovesOnlyOneRectOfATwoRectShapeNotBothOrTheWrongOne)
{
    // Regression: selection/Move are piece-granular (UPDATES.md item
    // 21) - clicking one rect of a Shape that bundles 2+ rects together
    // (e.g. several RECT statements under one LEF OBS LAYER line) must
    // select and move only that raw piece, at its real stored index, not
    // the whole Shape or the wrong sibling.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);
    const double rects_um[8] = {0.1, 0.1, 0.3, 0.4, 0.5, 0.1, 0.7, 0.4};
    const LeShapeId shape_id = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M4"), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rects_um, 8, 0, 0.0, 0, 0.0, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 2);

    // scale 0.1 (10 dbu/px), pan (0,0) - same recipe as
    // MoveTranslatesSelectedShapeGeometryAndIsUndoable above.
    le_set_viewport_size(handle, 200, 200);
    le_zoom(handle, -0.9, 0, 200);

    // Click rect index 1's center - dbu (600, 250), device (60, 175)
    // (pre-flip: x_px=dbu_x*scale, y_px=viewport_height-dbu_y*scale).
    // Rect index 0 occupies device x 10-30; rect index 1 occupies device
    // x 50-70 - unambiguous which one this hits.
    le_mouse_down(handle, 60, 175);
    le_mouse_up(handle, 60, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_set_mode(handle, LE_MODE_EDIT);
    le_arm_move(handle);

    // Anchor at dbu (600,250), commit at dbu (650,250) - dx=50, dy=0.
    le_set_mouse_position(handle, 60, 175);
    le_mouse_down(handle, 60, 175);
    le_mouse_up(handle, 60, 175);
    le_set_mouse_position(handle, 65, 175);
    le_mouse_down(handle, 65, 175);
    le_mouse_up(handle, 65, 175);

    // Only rect index 1 (the clicked piece) moved - rect index 0 is
    // untouched, and both remain real rects at their original indices.
    ASSERT_EQ(le_shape_rect_count(handle, shape_id), 2);
    const LeRectUm rect0 = le_shape_rect_at(handle, shape_id, 0);
    EXPECT_DOUBLE_EQ(rect0.ll_x_um, 0.1); // untouched sibling rect
    EXPECT_DOUBLE_EQ(rect0.ur_x_um, 0.3);
    const LeRectUm rect1 = le_shape_rect_at(handle, shape_id, 1);
    EXPECT_DOUBLE_EQ(rect1.ll_x_um, 0.55); // moved by +0.05 um (50 dbu)
    EXPECT_DOUBLE_EQ(rect1.ur_x_um, 0.75);
    EXPECT_EQ(le_shape_polygon_count(handle, shape_id), 0);
}

TEST_F(ApiFixture, DragSelectEnclosingTwoPiecesOfTheSameShapeSelectsBothAsSeparateEntries)
{
    // Selection is piece-granular (UPDATES.md item 21) - a drag enclosing
    // two rects that happen to belong to the same Shape must select both
    // as independent entries, not dedup down to one whole-shape entry.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design_abstract(handle, 0), 0);
    const LeAbstractId abstract_id = testcell_abstract_id(handle);

    const LeObstructionId obstruction_id = le_create_obstruction(handle, abstract_id);
    const double rects_um[8] = {0.1, 0.1, 0.3, 0.4, 0.5, 0.1, 0.7, 0.4};
    const LeShapeId shape_id = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, obstruction_id, LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, LeRouteId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0}, named_layer(handle, "M4"), nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rects_um, 8, 0, 0.0, 0, 0.0, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    le_set_viewport_size(handle, 200, 200);
    le_zoom(handle, -0.9, 0, 200); // scale 0.1, pan (0,0)

    // Drag a rectangle enclosing both rects: device (0,200)-(80,0) - dbu
    // (0,0)-(800,2000), comfortably containing both (100,100)-(300,400)
    // and (500,100)-(700,400).
    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 80, 0);

    EXPECT_EQ(le_selection_count(handle), 2);
}
