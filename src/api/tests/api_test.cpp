#include "session_test.hpp"
#include "../../database/library_helpers.hpp"
#include "../le_handle.hpp"
#include "../../test_support/scratch_dir.hpp"
#include "generated/pipelines/renderable_classes.hpp"
#include <algorithm>
#include <set>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    // A moderately-sized (not stress_data.hpp's full 1M-shape) generated
    // LEF - one PIN per shape, matching stress_data.hpp's "fresh LAYER
    // before every geometry item" trick so shapes_from_parser finalizes
    // many separate Shape entries - giving each render enough work per
    // call to take measurable time, without making this test itself
    // slow. Written to a scratch temp file rather than a checked-in
    // fixture, since it's generated, not hand-authored. The name is unique
    // per process and size: ctest runs each test as its own process, so a
    // shared path lets a parallel test overwrite the file before it's read.
    std::string generate_concurrency_stress_lef(int shape_count)
    {
        const std::filesystem::path path = le::test::scratch_path("le_concurrency_stress_" + std::to_string(shape_count) + ".lef");

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

    /// @brief Reads a generated stress LEF into `handle`, then deletes the temp file.
    bool read_concurrency_stress_lef(le::Session &session, int shape_count)
    {
        const std::string path = generate_concurrency_stress_lef(shape_count);
        const bool read = session.write().read_lef(path, "test_lib").has_value();
        std::filesystem::remove(path);
        return read;
    }

    // ROUTING layers (e.g. the pin's M1) render with a tiled FillPattern
    // (diagonal stripes) rather than a flat fill - unlike a flat fill, a
    // single fixed pixel can land in a transparent gap by coincidence of
    // the pattern's phase. Scans a region instead of trusting one point.
    bool region_has_opaque_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
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
    // kCursorBoxColor, draw_helpers.hpp) - distinct from the grid's gray/white
    // dots and any layer fill color, so "clearly red-dominant and opaque"
    // is a reliable way to detect it without depending on exact stroke
    // antialiasing.
    bool region_has_red_cursor_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
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
    // kHoverOutlineColor, draw_helpers.hpp) - distinct from the cursor box (red),
    // origin marker (amber), and grid (gray/white), so "clearly
    // yellow-dominant and opaque" reliably detects it without depending
    // on exact stroke antialiasing.
    bool region_has_yellow_hover_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
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
    // kSelectionOutlineColor, draw_helpers.hpp) - distinct from every other
    // overlay/grid color and from every default layer palette color (none
    // of which are pure white), so "R/G/B all near max and opaque"
    // reliably detects it without depending on exact stroke antialiasing.
    bool region_has_white_selection_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
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

    // The Move ghost preview is drawn dashed, translucent white (see
    // draw_helpers.hpp's own kMoveGhostColor, alpha 160/255 - unlike the
    // fully-opaque selection outline above) onto whatever's already
    // there via ordinary SRC_OVER compositing - premultiplied over a
    // transparent background, its own ink lands around (160,160,160,160)
    // at full stroke coverage. "Grayish/white-ish but clearly not fully
    // opaque" distinguishes it from both a fully-transparent background
    // and the fully-opaque selection outline/design fill colors, without
    // depending on exact dash-phase/stroke-antialiasing coverage.
    bool region_has_move_ghost_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 100 && p[1] > 100 && p[2] > 100 && p[3] > 80 && p[3] < 220)
                    return true;
            }
        return false;
    }

    // A committed ruler segment is drawn pure opaque orange (see
    // draw_helpers.hpp's own kRulerColor) - distinct from every other
    // overlay color (red cursor, yellow hover, white selection,
    // translucent-white Move ghost) by hue alone, so "orange and opaque"
    // reliably detects it without depending on exact stroke
    // antialiasing.
    bool region_has_ruler_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] > 90 && p[1] < 190 && p[2] < 60 && p[3] > 200)
                    return true;
            }
        return false;
    }

    // The live, not-yet-committed ruler ghost segment is drawn translucent
    // orange (see draw_helpers.hpp's own kRulerGhostColor, alpha 140/255) -
    // premultiplied over a transparent background its own ink lands
    // around (140,77,0,140), distinguishable from the fully-opaque
    // committed-segment color above by alpha alone.
    bool region_has_ruler_ghost_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 90 && p[1] > 30 && p[1] < 150 && p[2] < 60 && p[3] > 60 && p[3] < 200)
                    return true;
            }
        return false;
    }

    // A major grid dot is drawn opaque near-white (see draw_helpers.hpp's
    // own kMajorGridColor, {255,255,255,230}) - premultiplied, that's
    // R==G==B==alpha (white's own RGB channels equal alpha exactly once
    // multiplied through), a ratio of 1.0 regardless of how much of a
    // pixel the dot's own small on-screen radius (kGridDotRadius, only
    // ~1-2px across) actually covers - a dbu-integer lattice point
    // always falls exactly on a pixel *corner* shared by four pixels,
    // so no single pixel ever reaches the dot's own full nominal alpha,
    // only a fraction of it. The >140 floor is comfortably above the
    // minor grid dot's own analogous ratio (~0.5 - kMinorGridColor's
    // RGB is roughly half its own alpha) and above background/edge
    // antialiasing noise, while still well below the dot's typical
    // partially-covered value (~160 at 2x zoom).
    bool region_has_major_grid_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 140 && p[1] > 140 && p[2] > 140 && p[3] > 140)
                    return true;
            }
        return false;
    }

    // A minor grid dot is drawn translucent gray (see draw_helpers.hpp's
    // own kMinorGridColor, {128,128,128,120}) - premultiplied over a
    // transparent background its own ink lands around (60,60,60,120),
    // distinguishable from the major tier's own much brighter/more-opaque
    // ink by both R and alpha.
    bool region_has_minor_grid_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 30 && p[0] < 110 && p[1] > 30 && p[1] < 110 && p[2] > 30 && p[2] < 110 && p[3] > 70 && p[3] < 180)
                    return true;
            }
        return false;
    }

    // The Abstract origin marker is drawn opaque amber (see
    // draw_helpers.hpp's own kOriginMarkerColor, {255,200,0,255}) - a
    // low blue channel like the major grid dot, but distinguished from
    // it by G (~200, not ~255). The marker sits exactly on dbu (0,0),
    // which is also where the axis lines are drawn (kAxisLineColor,
    // translucent white) - the marker's own antialiased stroke edges
    // partially blend with that underlying axis ink, so the blue
    // channel here is loosened well past the marker's own "pure" value
    // to tolerate that real, expected blending rather than requiring an
    // exact color match.
    bool region_has_origin_marker_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] > 150 && p[1] < 230 && p[2] < 120 && p[3] > 200)
                    return true;
            }
        return false;
    }

    using le::test::fixture_path;

    struct ApiFixture : le::test::SessionTest
    {
    };
}

TEST_F(ApiFixture, ReadLefWithMissingFileReturnsNonzeroAndLoadsNoDesigns)
{
    EXPECT_FALSE(read_lef("/does/not/exist.lef", "test_lib"));
    EXPECT_EQ(static_cast<int>(design_count()), 0);
}

TEST_F(ApiFixture, ReadLefWithValidFileSucceedsAndPopulatesOneDesign)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_EQ(static_cast<int>(design_count()), 1);

    EXPECT_EQ(design_name(0), "TESTCELL");
}

// Backend messages go straight to spdlog (le_shell's own console), not
// a queue the API exposes.
// Message *content* (ERROR/WARNING text, malformed vs. warning-only
// fixtures) is still covered directly at the reader level - see
// LEFReaderErrors/LEFReaderMessages in lef_reader_test.cpp - so only
// read_lef's own return-code contract for these fixtures is worth
// re-asserting here, at the API layer.
TEST_F(ApiFixture, ReadLefWithMalformedContentReturnsNonzero)
{
    EXPECT_FALSE(read_lef(fixture_path("malformed.lef"), "malformed"));
}

TEST_F(ApiFixture, ReadLefWithAWarningProducingFileStillSucceeds)
{
    EXPECT_TRUE(read_lef(fixture_path("warning_currentden.lef"), "warning_currentden"));
}

// tooltip.
TEST_F(ApiFixture, TooltipMessageReturnsTheSelectModeInstructions)
{
    const std::string tooltip(read().tooltip());
    EXPECT_NE(std::string(tooltip).find("Left click to select"), std::string::npos);
}

TEST_F(ApiFixture, TooltipMessageReflectsEditMode)
{
    write().set_mode(le::InteractionMode::EDIT);
    const std::string tooltip(read().tooltip());
    EXPECT_EQ(std::string(tooltip).find("Left click to select"), std::string::npos);
}

TEST_F(ApiFixture, EditModeTooltipListsEveryEditModeShortcutSeparatedByBlankLines)
{
    write().set_mode(le::InteractionMode::EDIT);
    const std::string tooltip = std::string(read().tooltip());
    for (const char *shortcut : {"Ctrl-M", "Ctrl-R", "Shift", "Delete", "Esc", "Ctrl-Z", "Ctrl-Shift-Z", "Z / Shift-Z", "\n\nF: fit the design.\n\n", "\n\nCtrl-F: fit the selection.\n\n", "Arrows",
                                 "1-9", "S / R"})
        EXPECT_NE(tooltip.find(shortcut), std::string::npos) << shortcut;
    std::size_t separators = 0;
    for (std::size_t at = tooltip.find("\n\n"); at != std::string::npos; at = tooltip.find("\n\n", at + 2))
        ++separators;
    EXPECT_EQ(separators, 12u);
    EXPECT_EQ(tooltip.find("\n\n\n"), std::string::npos);
    EXPECT_EQ(static_cast<std::size_t>(std::ranges::count(tooltip, '\n')), 2 * separators); // no single newlines
}

TEST_F(ApiFixture, TooltipMessageReflectsRulerMode)
{
    write().set_mode(le::InteractionMode::RULER);
    const std::string tooltip(read().tooltip());
    EXPECT_NE(std::string(tooltip).find("Esc"), std::string::npos);
}

// mode()/set_mode.
TEST_F(ApiFixture, GetModeDefaultsToSelectMode)
{
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);
}

TEST_F(ApiFixture, SetModeThenGetModeRoundTrips)
{
    write().set_mode(le::InteractionMode::EDIT);
    EXPECT_EQ(read().mode(), le::InteractionMode::EDIT);

    write().set_mode(le::InteractionMode::SELECT);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);
}

TEST_F(ApiFixture, SetModeToRulerDoesNotEagerlyCreateARuler)
{
    write().set_mode(le::InteractionMode::RULER);
    EXPECT_EQ(read().mode(), le::InteractionMode::RULER);
    // Rulers start lazily on the first click (LeHandle::add_ruler_point) -
    // entering Ruler mode alone doesn't create an empty one.
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 0);
}

TEST_F(ApiFixture, SetCurrentDesignValidIndexSucceeds)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_TRUE(open_abstract(size_t(0)));
}

TEST_F(ApiFixture, RenderPixelBufferBeforeAnySetupDoesNotCrashAndStaysWithinRequestedViewport)
{
    // No LEF loaded, no Design selected, but a real viewport size - the
    // degenerate "nothing to draw yet" case should still produce a valid,
    // correctly-sized buffer rather than crash.
    write().set_viewport_size(50, 50);
    le::RenderedFrame buffer = render();
    EXPECT_NE(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 50);
    EXPECT_EQ(buffer.height, 50);
    EXPECT_GE(buffer.row_bytes, 50 * 4);
}

TEST_F(ApiFixture, AnOversizedViewportIsClampedToTheImageLimit)
{
    write().set_viewport_size(100000, 300);
    EXPECT_EQ(read().view().width_px, 8192);
    EXPECT_EQ(read().view().height_px, 300);
    write().set_viewport_size(-5, 300);
    EXPECT_EQ(read().view().width_px, 0);
}

TEST_F(ApiFixture, RenderPixelBufferWithZeroSizedViewportDoesNotCrash)
{
    le::RenderedFrame buffer = render();
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
}

TEST_F(ApiFixture, RenderPixelBufferProducesTheRequestedDimensions)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);

    // The view starts at scale 1.0 / pan (0, 0) - zoom to scale 10.0 (10
    // px/dbu-micron-ish, matches DATABASE MICRONS 1000 -> 1000 dbu/micron)
    // anchored at image pixel (0, 200) (bottom-left corner, i.e. dbu (0,
    // 0) at the starting pan/scale) keeps pan pinned at (0, 0) exactly.
    write().zoom(9.0, 0, 200);

    le::RenderedFrame buffer = render();
    EXPECT_EQ(buffer.width, 200);
    EXPECT_EQ(buffer.height, 200);
}

TEST_F(ApiFixture, SubPixelShapeIsNotRenderedAndIsNotSelectable)
{
    // bbox_is_sub_pixel (draw_helpers.hpp) - a shape under 1 on-screen
    // pixel in both dimensions is skipped before any fill/outline work is
    // done for it at all, and not replaced by a dot: at full-design zoom
    // most of a real design's shapes are sub-pixel, so drawing them in
    // any form would dominate the Rasterize cost.
    ASSERT_TRUE(read_lef(fixture_path("tiny_shape.lef"), "tiny_shape"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);

    // The view starts at scale 1.0 / pan (0, 0). PIN A's RECT is exactly 1x1
    // dbu - at scale 1.0 that's exactly at (not below) the sub-pixel
    // threshold, so it renders normally there. Zooming out to scale 0.5,
    // anchored at pixel (0, 100) (dbu (0, 0) at the starting pan/scale -
    // see RenderPixelBufferProducesTheRequestedDimensions's own comment
    // for why this anchor keeps pan pinned exactly at (0, 0)), doubles the
    // threshold to 2 dbu, putting the shape below it.
    write().zoom(-0.5, 0, 100);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // No dot, no outline, nothing - the shape's own bbox center stays
    // exactly as blank as the rest of the empty canvas.
    EXPECT_FALSE(region_has_opaque_pixel(buffer, 3, 93, 7, 97));

    write().mouse_down(5, 95);
    write().mouse_up(5, 95);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, FitSceneWithNoDesignSelectedFallsBackToDefaultScaleAndPan)
{
    write().set_viewport_size(100, 100);
    write().fit_view(10);

    le::RenderedFrame buffer = render();
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
}

TEST_F(ApiFixture, FitSceneFillsTheViewportWithThePinVisible)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);
    write().fit_view(10);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // The macro's content should now fill most of the 200x200 viewport -
    // scan a block around the center, which sits inside both the 10x10
    // micron macro boundary and PIN A's (2,2)-(8,8) micron RECT. PIN A is
    // on M1 (a ROUTING layer), so its fill is a tiled diagonal-stripe
    // pattern rather than a flat fill - a single fixed pixel could land in
    // a transparent gap by coincidence of the pattern's phase.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, FitRectWithADegenerateRectFallsBackToDefaultScaleAndPan)
{
    write().set_viewport_size(100, 100);
    fit_rect(0.5, 0.5, 0.5, 0.5, 10); // zero-area (ll == ur)

    le::RenderedFrame buffer = render();
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
}

TEST_F(ApiFixture, FitRectUsesTheGivenRectNotTheDesignsOwnBbox)
{
    // Same fixture as FitSceneFillsTheViewportWithThePinVisible (TESTCELL,
    // PIN A's own (2,2)-(8,8)um local rect) - fit_rect takes microns
    // directly (converted to dbu internally via this fixture's own
    // 1000 dbu/um), not raw dbu - the point here is that it frames
    // whatever rect the caller passes, not the whole macro's own
    // declared bbox the way fit_view does.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);

    // region_has_opaque_pixel alone can't tell PIN A's own M1 fill apart
    // from the background dot/axis grid (both opaque) - at the tight
    // zoom a 1000x1000 dbu rect into a 200x200px viewport produces, grid
    // dots are dense enough to hit almost any region by chance, so this
    // checks specifically for a *colored* (non-grayscale) pixel, the
    // same r != g trick hierarchy_resolver_test.cpp's own
    // pixel_buffer_region_has_opaque_pixel uses - M1 (a ROUTING layer)
    // never gets a grayscale default color, so this reliably
    // distinguishes real content from the grid.
    auto region_has_colored_pixel = [](const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[3] > 0 && p[0] != p[1])
                    return true;
            }
        return false;
    };

    // A patch of the macro's own empty interior that PIN A's own rect
    // never reaches - deliberately not touching the macro's own
    // declared (0,0)-(10,10)um boundary edges either (a real,
    // separately-drawn outline, not phantom grid, that a rect anchored
    // right at the origin would pick up as a false positive here). If
    // fit_rect silently fell back to the design's own full bbox (like
    // fit_view), the pin would still show up somewhere in the frame;
    // it must not, since this rect doesn't include it at all.
    fit_rect(0.1, 0.1, 1.1, 1.1, 0);
    le::RenderedFrame empty_corner = render();
    ASSERT_NE(empty_corner.data, nullptr);
    EXPECT_FALSE(region_has_colored_pixel(empty_corner, 0, 0, 199, 199));

    // Fitting exactly to PIN A's own rect (2,2)-(8,8)um should fill the
    // viewport with it - a much tighter frame than fit_view's own
    // whole-macro view.
    fit_rect(2, 2, 8, 8, 0);
    le::RenderedFrame pin_area = render();
    ASSERT_NE(pin_area.data, nullptr);
    EXPECT_TRUE(region_has_colored_pixel(pin_area, 80, 80, 120, 120));
}

TEST_F(ApiFixture, FitSceneInLayoutViewFramesTheDiereaNotTheOrigin)
{
    // In a Layout view there is no current_abstract() (the two "current
    // view" trackers are mutually exclusive), so fit must frame the
    // Layout's own DIEAREA rather than fall back to scale=1.0/pan={0,0}.
    // Places the diearea (and the placement whose pin should end up
    // visible) far from the origin - a scale=1/pan={0,0} fallback
    // against a 100x100px viewport can't reach dbu coordinates in the
    // millions no matter what, so this is a direct, robust proof the fix
    // actually frames the real content rather than replicating fit's own
    // pan/scale math by hand.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const double diearea_um[4] = {1000.0, 1000.0, 1100.0, 1100.0};
    create_rects(le::ShapeOwner::layout(top_layout), le::LayerId{}, std::span<const double>(diearea_um, 4), le::ShapePurpose::BOUNDARY);

    // TESTCELL placed at (1010,1010)um - well inside the diearea above -
    // so its own PIN A rect ((2,2)-(8,8)um local) lands at world
    // (1012,1012)-(1018,1018)um, still far from the origin.
    ASSERT_NE(create_placement(top_layout, testcell_design.id, "U1", le::PointUm{1010.0, 1010.0}, le::Orientation::N, le::PlacementStatus::PLACED).index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1); // remaining_depth 0 - the placement falls back straight to TESTCELL's own Abstract

    write().set_viewport_size(100, 100);
    write().fit_view(10); // no manual pan/zoom - fit_scene itself must compute a real view

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 0, 0, 100, 100));
}

TEST_F(ApiFixture, EachLefReadCreatesItsOwnLibrary)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_lef(fixture_path("othercell.lef"), "othercell"));

    ASSERT_EQ(library_count(), 2);

    const LibraryInfo lib0 = library_at(0);
    EXPECT_EQ(lib0.name, "testcell");
    EXPECT_TRUE(lib0.id.index != UINT32_MAX);

    const LibraryInfo lib1 = library_at(1);
    EXPECT_EQ(lib1.name, "othercell");
    EXPECT_NE(lib1.id.index, lib0.id.index);
}

TEST_F(ApiFixture, LibraryDesignAtReturnsTheDesignAndItsAbstractId)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    ASSERT_EQ(library_design_count(0), 1);

    const LibraryInfo library = library_at(0);
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);

    EXPECT_EQ(design.name, "TESTCELL");
    EXPECT_EQ(design.library_id.index, library.id.index);
    EXPECT_EQ(design.library_id.generation, library.id.generation);
    EXPECT_NE(design.id.index, UINT32_MAX);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX); // every read Design gets an Abstract view
    EXPECT_EQ(design.layout_id.index, UINT32_MAX);   // a plain LEF macro has no Layout view
}

TEST_F(ApiFixture, LibraryDesignAtReturnsAValidLayoutIdOnceADefIsReadIntoTheSameDesign)
{
    // testcell.def's own DESIGN name (TESTCELL) matches testcell.lef's own
    // macro name exactly, so DEFReader::get_design_by_name reuses the same
    // Design a LEF-only read already created (a Design can carry both an
    // Abstract and a Layout view at once, not just one or the other).
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));

    ASSERT_EQ(library_design_count(0), 1);
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);

    EXPECT_NE(design.abstract_id.index, UINT32_MAX);
    EXPECT_NE(design.layout_id.index, UINT32_MAX);
}

TEST_F(ApiFixture, OpeningAnUnknownDesignFails)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_FALSE(open_abstract(le::DesignId{UINT32_MAX, 0}));
    EXPECT_FALSE(open_abstract(le::DesignId{99, 0}));
}

TEST_F(ApiFixture, SetCurrentDesignByIdAlsoSetsTheGeneratedCurrentAbstract)
{
    // open_abstract is the shared entry point both
    // the GUI and a TCL script
    // (open_design) select a Design through - both should mean the same
    // thing: get_terminals/get_shapes/etc.'s own default -of-omitted
    // scope (current_abstract - the generated has_current_access
    // state, not LeHandle::current_abstract()) has to move too, not just
    // whatever the render actually shows.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    EXPECT_EQ(read().current_abstract().index, UINT32_MAX); // nothing selected yet

    ASSERT_TRUE(open_abstract(design.id));

    const le::AbstractId current = read().current_abstract();
    EXPECT_EQ(current.index, design.abstract_id.index);
    EXPECT_EQ(current.generation, design.abstract_id.generation);
}

TEST_F(ApiFixture, SetCurrentDesignAlsoSetsTheGeneratedCurrentAbstract)
{
    // Same as SetCurrentDesignByIdAlsoSetsTheGeneratedCurrentAbstract
    // above, for the index-addressed sibling entry point.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_abstract(size_t(0)));

    const le::AbstractId current = read().current_abstract();
    EXPECT_EQ(current.index, design.abstract_id.index);
    EXPECT_EQ(current.generation, design.abstract_id.generation);
}

TEST_F(ApiFixture, SetCurrentDesignByIdSelectsTheSameDesignAsSetCurrentDesign)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_abstract(design.id));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    // PIN A is on M1 (ROUTING), so its fill is a tiled diagonal-stripe
    // pattern - scan the pin's own region rather than trusting one pixel.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect visible, same as __OK__(open_abstract(size_t(0))) would give
}

// --- Layout view / hierarchy depth ---

TEST_F(ApiFixture, HierarchyDepthDefaultsToZeroAndRoundTrips)
{
    EXPECT_EQ(read().hierarchy_depth(), 0);
    write().set_hierarchy_depth(3);
    EXPECT_EQ(read().hierarchy_depth(), 3);
}

TEST_F(ApiFixture, SetHierarchyDepthRejectsNegativeValues)
{
    write().set_hierarchy_depth(2);
    ASSERT_EQ(read().hierarchy_depth(), 2);
    write().set_hierarchy_depth(-1);
    EXPECT_EQ(read().hierarchy_depth(), 2);
}

// --- Max concurrency ---

TEST_F(ApiFixture, MaxConcurrencyDefaultsToEightAndRoundTrips)
{
    EXPECT_EQ(read().max_concurrency(), 8);
    write().set_max_concurrency(4);
    EXPECT_EQ(read().max_concurrency(), 4);
}

TEST_F(ApiFixture, MaxConcurrencySetsTheRenderThreadCount)
{
    const auto render_threads = [&]
    { return handle->view_render_pipeline.rasterize_stage().thread_count(); };
    EXPECT_EQ(render_threads(), static_cast<uint32_t>(read().max_concurrency()));
    write().set_max_concurrency(3);
    EXPECT_EQ(render_threads(), 3u);
    write().set_max_concurrency(0);
    EXPECT_EQ(render_threads(), 2u);
}

TEST_F(ApiFixture, SetMaxConcurrencyClampsBelowTwoUpToTwoRatherThanRejecting)
{
    write().set_max_concurrency(1);
    EXPECT_EQ(read().max_concurrency(), 2);
    write().set_max_concurrency(0);
    EXPECT_EQ(read().max_concurrency(), 2);
    write().set_max_concurrency(-5);
    EXPECT_EQ(read().max_concurrency(), 2);
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
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    ASSERT_NE(create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED).index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1); // remaining_depth 0 - the placement falls back straight to TESTCELL's own Abstract

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // same scale=0.01 setup as the Abstract-view test above

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79));
}

// The PLACEMENT purpose draws each placement's outline and name label,
// and hiding it hides both.
TEST_F(ApiFixture, PlacementPurposeDrawsTheOutlineAndLabelAndHidingItHidesBoth)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    ASSERT_NE(create_placement(top_layout, testcell_design.id, "U1", le::PointUm{2.0, 2.0}, le::Orientation::N, le::PlacementStatus::PLACED).index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1);
    write().set_viewport_size(200, 200);
    fit_rect(-5.0, -5.0, 15.0, 15.0, 0); // U1 at (2um,2um) spans pixels x ~70-170, y ~30-130

    // Alpha > 64, not merely nonzero, so antialiased fringes don't count.
    auto region_has_visible_pixel = [](const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (buffer.data[static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4 + 3] > 64)
                    return true;
        return false;
    };

    auto show_only = [&](int32_t purpose)
    {
        const std::vector<le::ViewLayerPurpose> purposes = read().purposes();
        le::WriteView view = write();
        for (const le::ViewLayerPurpose p : purposes)
            view.set_purpose_visible(p, static_cast<int32_t>(p) == purpose);
    };

    // The placement's left edge (x ~70), well above the bottom-left-
    // anchored label and clear of the axis lines (dbu 0, pixel 50) and the
    // grid dots (every 50px); the label itself sits just inside the
    // bottom-left corner.
    show_only(11 /* PLACEMENT */);
    le::RenderedFrame shown = render();
    ASSERT_NE(shown.data, nullptr);
    EXPECT_TRUE(region_has_visible_pixel(shown, 65, 40, 75, 75));    // outline
    EXPECT_TRUE(region_has_visible_pixel(shown, 75, 103, 140, 128)); // label

    show_only(-1);
    le::RenderedFrame hidden = render();
    ASSERT_NE(hidden.data, nullptr);
    EXPECT_FALSE(region_has_visible_pixel(hidden, 65, 40, 75, 75));
    EXPECT_FALSE(region_has_visible_pixel(hidden, 75, 103, 140, 128));
}

TEST_F(ApiFixture, SetCurrentDesignLayoutClearsTheAbstractViewAndViceVersa)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100);

    // Abstract view first - the pin is visible directly.
    ASSERT_TRUE(open_abstract(testcell_design.id));
    le::RenderedFrame abstract_buffer = render();
    ASSERT_NE(abstract_buffer.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(abstract_buffer, 21, 21, 79, 79));

    // Switching to the Layout view must stop rendering the old Abstract
    // (handle->current_abstract() clears) - render_pixel_buffer renders
    // the Layout instead.
    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1);
    le::RenderedFrame layout_buffer = render();
    ASSERT_NE(layout_buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(layout_buffer, 21, 21, 79, 79)); // same pin, now reached via the placement

    // Switching back to the Abstract view must stop rendering the Layout.
    ASSERT_TRUE(open_abstract(testcell_design.id));
    le::RenderedFrame back_to_abstract = render();
    ASSERT_NE(back_to_abstract.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(back_to_abstract, 21, 21, 79, 79));
}

// --- Selectable objects in Layout view ---
// A Layout view must hit-test the Layout's own content, never whatever
// Abstract content happens to exist. These exercise the real, full click -> LeHandle::
// selection() -> selection()() path end-to-end, the same way
// ClickSelectingAShapeReportsExactlyTheSamePropertiesAsGetPropertiesOnItsShapeId
// already does for the Abstract path.

TEST_F(ApiFixture, UnsetOptionalReferenceFieldDisplaysAsEmptyStringNotADanglingToken)
{
    // Shape.layer is_optional=True (unset for a BOUNDARY-purpose Shape,
    // which uses Shape.purpose instead - see Shape.layer/.purpose's own
    // schema.py comments) - the friendly-token override in
    // build_shape_properties (api_property_accessors_internal_inc_j2.py)
    // must degrade to an empty string when root.get_layer() finds nothing
    // for the default-invalid le::LayerId, not dereference a null pointer
    // or otherwise fall back to the raw "Id{index=.., generation=..}"
    // debug string.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const double diearea_um[4] = {0.0, 0.0, 100.0, 100.0};
    const le::ShapeId diearea_shape_id = create_rects(le::ShapeOwner::layout(top_layout), le::LayerId{}, std::span<const double>(diearea_um, 4), le::ShapePurpose::BOUNDARY);
    ASSERT_NE(diearea_shape_id.index, UINT32_MAX);

    const le::ObjectId shape_ref = le::ObjectId{diearea_shape_id};
    const int32_t count = static_cast<int32_t>(properties(shape_ref).size());
    bool found_layer = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(shape_ref).at(i);
        if (std::string(property.name) != "layer")
            continue;
        found_layer = true;
        EXPECT_EQ(property.string_value, "");
    }
    EXPECT_TRUE(found_layer);
}

TEST_F(ApiFixture, UnsetOptionalEnumFieldDisplaysAsEmptyStringNotItsZeroValuedMember)
{
    // Reported bug: a Route's own Shape (real routing geometry, scoped
    // via Shape.route with a real Shape.layer - see Shape.layer/.purpose's
    // own schema.py comments) displayed its unset Shape.purpose as
    // "BOUNDARY" in the Property Viewer - ShapePurpose::BOUNDARY just
    // happens to be the enum's zero-valued member (schema.py's
    // ShapePurpose declaration order), and value.purpose.value_or(
    // ShapePurpose{}) value-initialized to that zero value whenever
    // purpose was genuinely unset, indistinguishable from a real,
    // deliberate BOUNDARY classification. Fixed generically for every
    // is_optional enum field (Field._optional_enum_needs_unset_guard(),
    // codegen/codegen/schema.py) - a real has_value() check now degrades
    // to an empty string instead.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    ASSERT_NE(route_id.index, UINT32_MAX);
    const le::LayerId m1_layer = layer_named("M1");
    ASSERT_NE(m1_layer.index, UINT32_MAX);
    const double route_rect_um[4] = {0.0, 0.0, 1.0, 5.0};
    const le::ShapeId route_shape_id = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape_id.index, UINT32_MAX);

    const le::ObjectId shape_ref = le::ObjectId{route_shape_id};
    const int32_t count = static_cast<int32_t>(properties(shape_ref).size());
    bool found_purpose = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(shape_ref).at(i);
        if (std::string(property.name) != "purpose")
            continue;
        found_purpose = true;
        EXPECT_EQ(property.string_value, ""); // not "BOUNDARY"
    }
    EXPECT_TRUE(found_purpose);
}

// set_view_flip mirrors the picture about the view's centre, and the
// mouse, zoom anchor and arrow keys follow what's on screen.
TEST_F(ApiFixture, AFlippedViewMirrorsThePictureAndTheMouseFollowsIt)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_design_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    const double route_rect_um[4] = {0.0, 0.0, 6.0, 6.0};
    const le::ShapeId route_shape = create_rects(le::ShapeOwner::route(route_id), layer_named("M1"), std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape.index, UINT32_MAX);
    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100); // 200 dbu/px: the shape covers pixels x 0..30, y 70..100
    const auto lit = [&](int x, int y) { return region_has_opaque_pixel(render(), x - 1, y - 1, x + 1, y + 1); };
    const auto click_selects_the_shape = [&](int x, int y) {
        write().mouse_down(x, y);
        write().mouse_up(x, y);
        return selection_count() == 1 && selected(0) == le::ObjectId{route_shape};
    };

    EXPECT_EQ(read().view().flip, le::ViewFlip::NONE);
    EXPECT_TRUE(lit(15, 85));
    EXPECT_FALSE(lit(85, 85));

    write().set_view_flip(le::ViewFlip::HORIZONTAL);
    EXPECT_EQ(read().view().flip, le::ViewFlip::HORIZONTAL);
    EXPECT_FALSE(lit(15, 85));
    EXPECT_TRUE(lit(85, 85)) << "left and right swap";
    EXPECT_TRUE(click_selects_the_shape(85, 85));
    EXPECT_FALSE(click_selects_the_shape(15, 85));

    // Zooming in at the shape keeps it under the cursor.
    write().zoom(1.0, 85, 85);
    EXPECT_TRUE(click_selects_the_shape(85, 85));
    write().zoom(-0.5, 85, 85);

    // The left arrow still moves the picture right on screen (a quarter
    // of the view): the shape's left edge goes from x 70 to 95.
    EXPECT_TRUE(lit(72, 85));
    write().key_down(le::Key::PAN_LEFT);
    EXPECT_FALSE(lit(72, 85));
    EXPECT_TRUE(lit(97, 85));
    write().key_down(le::Key::PAN_RIGHT);

    write().set_view_flip(le::ViewFlip::VERTICAL);
    EXPECT_FALSE(lit(15, 85));
    EXPECT_TRUE(lit(15, 15)) << "top and bottom swap";
    EXPECT_TRUE(click_selects_the_shape(15, 15));

}

// In a mirrored view the status bar's coordinates (snapped_mouse_position)
// are mirrored too, with the centre of the boundary (here a 100 x 50 um
// diearea) at the origin: its on-screen bottom-left is (-50, -25).
TEST_F(ApiFixture, AFlippedViewsCursorCoordinatesAreCentredOnTheBoundary)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_design_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    {
        le::WriteView view = write();
        ASSERT_TRUE(view.build_shape(le::ShapeOwner::layout(top_layout)).purpose(le::ShapePurpose::BOUNDARY).polygon({{0, 0}, {100, 0}, {100, 50}, {0, 50}}).create());
    }
    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(200, 100);
    write().fit_view(0); // 2 px per um: unflipped, pixel (x, y) is (x / 2, 50 - y / 2) um
    const auto cursor_at = [&](int x, int y) {
        write().set_mouse_position(x, y);
        const std::optional<le::PointUm> pos = snapped_mouse_position();
        EXPECT_TRUE(pos.has_value());
        return pos ? std::pair{pos->x, pos->y} : std::pair{0.0, 0.0};
    };

    EXPECT_EQ(cursor_at(40, 20), std::pair(20.0, 40.0)) << "unflipped: database coordinates";

    write().set_view_flip(le::ViewFlip::HORIZONTAL);
    EXPECT_EQ(cursor_at(40, 20), std::pair(-30.0, 15.0)); // shows (80, 40) um
    EXPECT_EQ(cursor_at(0, 100), std::pair(-50.0, -25.0)) << "the boundary's on-screen bottom-left";
    EXPECT_EQ(cursor_at(200, 0), std::pair(50.0, 25.0)) << "and top-right";

    write().set_view_flip(le::ViewFlip::VERTICAL);
    EXPECT_EQ(cursor_at(40, 20), std::pair(-30.0, 15.0)); // shows (20, 10) um
    EXPECT_EQ(cursor_at(0, 100), std::pair(-50.0, -25.0));
}

// The axis lines cross at the user's (0, 0): the database origin, or in a
// mirrored view the centre of the boundary.
TEST_F(ApiFixture, AFlippedViewsAxesCrossAtTheBoundaryCentre)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_design_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    // A 100 x 50 um diearea from (-20, -10): its centre is (30, 15) um.
    {
        le::WriteView view = write();
        ASSERT_TRUE(view.build_shape(le::ShapeOwner::layout(top_layout)).purpose(le::ShapePurpose::BOUNDARY).polygon({{-20, -10}, {80, -10}, {80, 40}, {-20, 40}}).create());
    }
    ASSERT_TRUE(open_layout(top_design));
    write().set_layer_visible("BOUNDARY", false); // only the axes and grid dots draw
    write().set_viewport_size(200, 100);
    write().fit_view(0); // 2 px per um: unflipped, um (x, y) is pixel (2 (x + 20), 2 (40 - y))
    // Whether a column / row holds an axis line: lit along most of its length.
    const auto column_is_axis = [&](int x) {
        const le::RenderedFrame buffer = render();
        int lit = 0;
        for (int y = 0; y < buffer.height; ++y)
            lit += buffer.data[y * buffer.row_bytes + x * 4 + 3] > 0;
        return lit > buffer.height * 3 / 4;
    };
    const auto row_is_axis = [&](int y) {
        const le::RenderedFrame buffer = render();
        int lit = 0;
        for (int x = 0; x < buffer.width; ++x)
            lit += buffer.data[y * buffer.row_bytes + x * 4 + 3] > 0;
        return lit > buffer.width * 3 / 4;
    };

    EXPECT_TRUE(column_is_axis(40)) << "x = 0 um";
    EXPECT_TRUE(row_is_axis(80)) << "y = 0 um";

    // Mirrored, the axes run through the boundary centre, (30, 15) um:
    // column 100 and row 50 whichever way it's flipped.
    for (const le::ViewFlip flip : {le::ViewFlip::HORIZONTAL, le::ViewFlip::VERTICAL})
    {
        write().set_view_flip(flip);
        EXPECT_TRUE(column_is_axis(100)) << static_cast<int>(flip);
        EXPECT_TRUE(row_is_axis(50)) << static_cast<int>(flip);
        EXPECT_FALSE(column_is_axis(flip == le::ViewFlip::HORIZONTAL ? 160 : 40)) << "not through the database origin";
    }
}

// A click over both a Route's own shape and a placement's bbox selects
// the shape first (shape pieces come before placements in click order);
// a click over the placement alone selects the placement.
TEST_F(ApiFixture, MouseClickInLayoutViewPrefersARouteOwnShapeOverAPlacementsBoundingBoxAtTheSamePoint)
{
    // TESTCELL (10x10 um) placed at (0,0), and a 6x6 um route shape at
    // the same origin: (3,3) um is inside both, (8,8) um only inside the
    // placement's bbox.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::PlacementId placement_id = create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
    ASSERT_NE(placement_id.index, UINT32_MAX);

    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    ASSERT_NE(route_id.index, UINT32_MAX);
    const double route_rect_um[4] = {0.0, 0.0, 6.0, 6.0};
    const le::LayerId m1_layer = layer_named("M1");
    ASSERT_NE(m1_layer.index, UINT32_MAX);
    const le::ShapeId route_shape_id = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape_id.index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1);

    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100); // 200 dbu/px: device (x, y) is dbu (200 x, 200 (100 - y))

    write().mouse_down(15, 85); // dbu (3000,3000) = (3,3) um - inside both the route shape and the placement's own bbox
    write().mouse_up(15, 85);
    ASSERT_EQ(selection_count(), 1);

    const std::optional<le::ObjectId> shape_ref = selected(0);
    EXPECT_TRUE(is<le::ShapeId>(shape_ref));
    EXPECT_EQ(shape_ref, le::ObjectId{route_shape_id});

    const std::optional<le::ObjectId> parent_ref = parent(shape_ref);
    EXPECT_TRUE(is<le::RouteId>(parent_ref));
    EXPECT_EQ(parent_ref, le::ObjectId{route_id});

    write().mouse_down(40, 60); // dbu (8000,8000) = (8,8) um - inside the placement's own bbox only
    write().mouse_up(40, 60);
    ASSERT_EQ(selection_count(), 1); // no shift held - replaces the previous selection
    EXPECT_TRUE(is<le::PlacementId>(selected(0)));
}

// A route shape moved after a click is found at its new place, not its old
// one (no render here, so this takes the whole-Layout scan - see
// selection_test.cpp for the render-tree path).
TEST_F(ApiFixture, AClickFindsARouteShapeAtItsNewPlaceAfterAnEdit)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    const double route_rect_um[4] = {0.0, 0.0, 2.0, 2.0};
    const le::LayerId m1_layer = layer_named("M1");
    const le::ShapeId shape_id = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(route_rect_um, 4));
    ASSERT_NE(shape_id.index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100); // 0.2 um per pixel from (0,0) at the bottom-left, as the tests above

    auto click = [&](int x, int y)
    {
        write().clear_selection();
        write().mouse_down(x, y);
        write().mouse_up(x, y);
        return selection_count();
    };
    ASSERT_EQ(click(5, 95), 1); // (1,1) um

    const le::Rect moved = units().to_dbu(le::RectUm{{12.0, 12.0}, {14.0, 14.0}});
    ASSERT_TRUE(write().update_shape(shape_id, {.rects = le::CompactVector<le::Rect>{moved}}));
    EXPECT_EQ(click(5, 95), 0);  // nothing at the old place
    ASSERT_EQ(click(65, 35), 1); // (13,13) um
    EXPECT_EQ(selected(0), le::ObjectId{shape_id});
}

// Hiding the PLACEMENT purpose, or making it unselectable, stops both
// click and drag selection from picking a placement up.
TEST_F(ApiFixture, PlacementsAreSelectableOnlyWhilePlacementPurposeIsVisibleAndSelectable)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    ASSERT_NE(create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED).index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_hierarchy_depth(1);
    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100); // same setup as the Route version above: (8,8) um is pixel (40,60)

    auto click = [&]
    {
        write().clear_selection();
        write().mouse_down(40, 60);
        write().mouse_up(40, 60);
        return selection_count();
    };
    auto drag = [&]
    {
        write().clear_selection();
        write().mouse_down(-5, 105); // encloses the whole 0-10um placement
        write().mouse_up(60, 40);
        return selection_count();
    };

    ASSERT_EQ(click(), 1);
    EXPECT_TRUE(is<le::PlacementId>(selected(0)));
    ASSERT_EQ(drag(), 1);
    EXPECT_TRUE(is<le::PlacementId>(selected(0)));

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(11 /* PLACEMENT */), 0);
    EXPECT_EQ(click(), 0);
    EXPECT_EQ(drag(), 0);

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(11), 1);
    write().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(11), 0);
    EXPECT_EQ(click(), 0);
    EXPECT_EQ(drag(), 0);

    write().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(11), 1);
    EXPECT_EQ(click(), 1);
}

TEST_F(ApiFixture, MouseClickInLayoutViewSelectsAPhysicalPortOwnShape)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell")); // establishes the Technology

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::PhysicalPortId port_id = write().create_physical_port({.layout = top_layout, .name = "PIN1"}).value_or(le::PhysicalPortId{});
    ASSERT_NE(port_id.index, UINT32_MAX);
    const le::PhysicalPortSegmentId segment_id = write().create_physical_port_segment({.physical_port = port_id}).value_or(le::PhysicalPortSegmentId{});
    ASSERT_NE(segment_id.index, UINT32_MAX);

    const double port_rect_um[4] = {2.0, 2.0, 4.0, 4.0};
    const le::LayerId m1_layer = layer_named("M1");
    ASSERT_NE(m1_layer.index, UINT32_MAX);
    const le::ShapeId port_shape_id = create_rects(le::ShapeOwner::physical_port_segment(segment_id), m1_layer, std::span<const double>(port_rect_um, 4));
    ASSERT_NE(port_shape_id.index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));

    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100);

    write().mouse_down(15, 85); // dbu (3000,3000) = (3,3) um - inside the port's own 2,2-4,4 shape
    write().mouse_up(15, 85);
    ASSERT_EQ(selection_count(), 1);

    const std::optional<le::ObjectId> shape_ref = selected(0);
    EXPECT_TRUE(is<le::ShapeId>(shape_ref));
    EXPECT_EQ(shape_ref, le::ObjectId{port_shape_id});

    // Shape.physical_port_segment -> PhysicalPortSegment.physical_port -
    // two hops, same as Terminal-port's own Shape->TerminalPort->Terminal
    // chain (object_ref_parent, api.cpp).
    const std::optional<le::ObjectId> segment_ref = parent(shape_ref);
    EXPECT_TRUE(is<le::PhysicalPortSegmentId>(segment_ref));
    EXPECT_EQ(segment_ref, le::ObjectId{segment_id});

    const std::optional<le::ObjectId> port_ref = parent(segment_ref);
    EXPECT_TRUE(is<le::PhysicalPortId>(port_ref));
    EXPECT_EQ(port_ref, le::ObjectId{port_id});
}

TEST_F(ApiFixture, SelectObjectRefWithRouteKindSelectsEveryPieceOfEveryChildShape)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    ASSERT_NE(route_id.index, UINT32_MAX);
    const le::LayerId m1_layer = layer_named("M1");
    const double rect_a_um[4] = {0.0, 0.0, 1.0, 1.0};
    const le::ShapeId shape_a = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(rect_a_um, 4));
    ASSERT_NE(shape_a.index, UINT32_MAX);
    const double rect_b_um[4] = {2.0, 2.0, 3.0, 3.0};
    const le::ShapeId shape_b = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(rect_b_um, 4));
    ASSERT_NE(shape_b.index, UINT32_MAX);

    const le::ObjectId route_ref = le::ObjectId{route_id};
    EXPECT_TRUE(select(route_id));
    ASSERT_EQ(selection_count(), 2); // one rect piece from each of shape_a/shape_b

    bool found_a = false, found_b = false;
    for (int32_t i = 0; i < selection_count(); ++i)
    {
        const std::optional<le::ObjectId> ref = selected(i);
        EXPECT_TRUE(is<le::ShapeId>(ref));
        found_a |= ref == le::ObjectId{shape_a};
        found_b |= ref == le::ObjectId{shape_b};
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

TEST_F(ApiFixture, SelectObjectRefWithRouteKindFailsForAnUnknownId)
{
    EXPECT_FALSE(select(le::RouteId{}));
    EXPECT_EQ(selection_count(), 0);
}

// A Route on a hidden ViewLayer (visibility off) must not be
// click-selectable - hit_test_layout_point's is_selectable predicate
// checks visibility as well as selectability, as in the Abstract view
// (MouseClickDoesNotSelectATerminalOnAHiddenLayer).
TEST_F(ApiFixture, MouseClickInLayoutViewDoesNotSelectARouteOnAHiddenLayer)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    ASSERT_NE(route_id.index, UINT32_MAX);
    const le::LayerId m1_layer = layer_named("M1");
    ASSERT_NE(m1_layer.index, UINT32_MAX);
    const double route_rect_um[4] = {0.0, 0.0, 6.0, 6.0};
    const le::ShapeId route_shape_id = create_rects(le::ShapeOwner::route(route_id), m1_layer, std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape_id.index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    write().zoom(0.005 - 1.0, 0, 100); // see MouseClickInLayoutViewPrefersARouteOwnShape...'s own scale/pan derivation
    write().set_layer_visible("M1", false);

    write().mouse_down(15, 85); // dbu (3000,3000) = (3,3) um - inside the route shape, but M1 is hidden
    write().mouse_up(15, 85);

    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, SelectObjectRefWithPhysicalPortKindSelectsEveryPieceOfEverySegmentShape)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    const le::PhysicalPortId port_id = write().create_physical_port({.layout = top_layout, .name = "PIN1"}).value_or(le::PhysicalPortId{});
    ASSERT_NE(port_id.index, UINT32_MAX);
    const le::PhysicalPortSegmentId segment_id = write().create_physical_port_segment({.physical_port = port_id}).value_or(le::PhysicalPortSegmentId{});
    ASSERT_NE(segment_id.index, UINT32_MAX);
    const le::LayerId m1_layer = layer_named("M1");
    const double rect_um[4] = {2.0, 2.0, 4.0, 4.0};
    const le::ShapeId shape_id = create_rects(le::ShapeOwner::physical_port_segment(segment_id), m1_layer, std::span<const double>(rect_um, 4));
    ASSERT_NE(shape_id.index, UINT32_MAX);

    const le::ObjectId port_ref = le::ObjectId{port_id};
    EXPECT_TRUE(select(port_id));
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(is<le::ShapeId>(selected(0)));
    EXPECT_EQ(selected(0), le::ObjectId{shape_id});
}

TEST_F(ApiFixture, MouseClickInLayoutViewSelectsARowWithNoBackingShape)
{
    // A Row has no stored Shape; its footprint is synthesized from its
    // Site (row_footprint_bbox).
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell")); // establishes the Technology

    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

    ASSERT_NE(create_site("SITE1", le::PointUm{10.0, 20.0}).index, UINT32_MAX);
    const le::RowId row_id = create_row(top_layout, "ROW1", "SITE1", le::PointUm{0.0, 0.0}, le::Orientation::N);
    ASSERT_NE(row_id.index, UINT32_MAX);

    // ROW defaults to invisible - a
    // hidden ViewLayer is filtered out before hit-testing ever sees it,
    // so this click-to-select test needs it made visible first; this
    // test is about the "origin set but no ShapeId" selection fork, not
    // about ROW's own default visibility.
    ASSERT_EQ(purpose_at(0), 6); // ROW - see ViewLayerPurpose's own declaration order
    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(6), 1);

    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    // Row footprint is (0,0)-(10,20) um (site size, single tile) ==
    // (0,0)-(10000,20000) dbu - scale 0.005 as above, same anchor.
    write().zoom(0.005 - 1.0, 0, 100);

    write().mouse_down(25, 75); // dbu (5000,5000) = (5,5) um - inside the row
    write().mouse_up(25, 75);

    ASSERT_EQ(selection_count(), 1);
    const std::optional<le::ObjectId> ref = selected(0);
    EXPECT_TRUE(is<le::RowId>(ref));
    EXPECT_EQ(ref, le::ObjectId{row_id});
}

// Layout view at scale 0.005 px/dbu over a 100x100 viewport: device
// (x, y) is dbu (200 x, 200 (100 - y)), so the view spans (0,0)-(20,20) um.
namespace
{
    constexpr int32_t kRowPurpose = 6; // ViewLayerPurpose::ROW

    le::DesignId row_test_layout(le::test::SessionHelpers &s, le::LayoutId &layout_out)
    {
        EXPECT_TRUE(s.read_lef(fixture_path("testcell.lef"), "testcell"));
        const le::LibraryId top_library = s.write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
        const le::DesignId top_design = s.write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
        layout_out = s.write().create_layout({.design = top_design}).value_or(le::LayoutId{});
        EXPECT_NE(s.create_site("SITE4", le::PointUm{4.0, 4.0}).index, UINT32_MAX);
        return top_design;
    }

    void show_layout_at_row_test_scale(le::test::SessionHelpers &s, le::DesignId design)
    {
        ASSERT_TRUE(s.open_layout(design));
        s.write().set_viewport_size(100, 100);
        s.write().zoom(0.005 - 1.0, 0, 100);
    }
}

TEST_F(ApiFixture, MouseClickOnARowUnderAPlacementSelectsThePlacementFirstThenTheRow)
{
    le::LayoutId layout{};
    const le::DesignId design = row_test_layout(*this, layout);
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);
    // TESTCELL is 10x10 um; the Row (0,0)-(4,4) um lies under it.
    const le::PlacementId placement_id = create_placement(layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
    const le::RowId row_id = create_row(layout, "ROW1", "SITE4", le::PointUm{0.0, 0.0}, le::Orientation::N);
    ASSERT_NE(row_id.index, UINT32_MAX);
    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(kRowPurpose), 1);
    show_layout_at_row_test_scale(*this, design);

    write().mouse_down(10, 90); // (2,2) um
    write().mouse_up(10, 90);
    ASSERT_EQ(selection_count(), 1);
    std::optional<le::ObjectId> ref = selected(0);
    EXPECT_TRUE(is<le::PlacementId>(ref));
    EXPECT_EQ(ref, le::ObjectId{placement_id});

    write().mouse_down(10, 90);
    write().mouse_up(10, 90);
    ASSERT_EQ(selection_count(), 1);
    ref = selected(0);
    EXPECT_TRUE(is<le::RowId>(ref));
    EXPECT_EQ(ref, le::ObjectId{row_id});
}

TEST_F(ApiFixture, DragSelectInLayoutViewSelectsAFullyEnclosedRowAndSkipsAPartlyEnclosedOne)
{
    le::LayoutId layout{};
    const le::DesignId design = row_test_layout(*this, layout);
    const le::RowId inside = create_row(layout, "ROW_IN", "SITE4", le::PointUm{0.0, 0.0}, le::Orientation::N);
    const le::RowId partly = create_row(layout, "ROW_PART", "SITE4", le::PointUm{8.0, 0.0}, le::Orientation::N);
    ASSERT_NE(inside.index, UINT32_MAX);
    ASSERT_NE(partly.index, UINT32_MAX);
    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(kRowPurpose), 1);
    show_layout_at_row_test_scale(*this, design);

    // (0,0)-(10,6) um: all of ROW_IN (0,0)-(4,4), half of ROW_PART (8,0)-(12,4).
    write().mouse_down(0, 100);
    write().mouse_up(50, 70);

    ASSERT_EQ(selection_count(), 1);
    const std::optional<le::ObjectId> ref = selected(0);
    EXPECT_TRUE(is<le::RowId>(ref));
    EXPECT_EQ(ref, le::ObjectId{inside});
}

TEST_F(ApiFixture, RowsAreNotSelectableWhileTheRowPurposeIsHiddenOrUnselectable)
{
    le::LayoutId layout{};
    const le::DesignId design = row_test_layout(*this, layout);
    ASSERT_NE(create_row(layout, "ROW1", "SITE4", le::PointUm{0.0, 0.0}, le::Orientation::N).index, UINT32_MAX);
    show_layout_at_row_test_scale(*this, design);

    // ROW is hidden by default.
    write().mouse_down(10, 90);
    write().mouse_up(10, 90);
    EXPECT_EQ(selection_count(), 0);
    write().mouse_down(0, 100);
    write().mouse_up(50, 70);
    EXPECT_EQ(selection_count(), 0);

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(kRowPurpose), 1);
    write().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(kRowPurpose), 0);
    write().mouse_down(10, 90);
    write().mouse_up(10, 90);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, NoLayerRowsBeforeATechnologyIsRead)
{
    EXPECT_TRUE(read().layer_rows().empty());
}

TEST_F(ApiFixture, LayerAtListsRowThenBoundaryThenEveryPhysicalLayer)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    // testcell.lef declares one physical Layer (M1) - the API doesn't
    // special-case BOUNDARY, it's just another row, so the count is
    // M1 + ROW + GCELLGRID + PLACEMENT_BLOCKAGE + REGION + BOUNDARY +
    // PLACEMENT + DEBUG + FLIGHTLINE + PORT_MARKER = 10, plus a row per
    // renderable class an extension adds (see
    // ViewLayerSet::build_for_technology). ROW then BOUNDARY then
    // PLACEMENT come first (this declaration order is also the real draw
    // z-order, see ViewLayerSet::rows()'s own doc comment).
    ASSERT_EQ(static_cast<int32_t>(read().layer_rows().size()), 10 + static_cast<int32_t>(le::renderable::kCount));

    const le::LayerRow boundary_row = read().layer_rows().at(1);
    EXPECT_EQ(boundary_row.name, "BOUNDARY");

    const le::LayerRow placement_row = read().layer_rows().at(2);
    EXPECT_EQ(placement_row.name, "PLACEMENT");

    const le::LayerRow m1_row = read().layer_rows().at(3);
    EXPECT_EQ(m1_row.name, "M1");

    // M1 is the first ROUTING layer declared - first slot of the bright
    // ROUTING/CUT palette, red (255, 0, 0) - see view_style.hpp.
    EXPECT_EQ(m1_row.color.r, 255);
    EXPECT_EQ(m1_row.color.g, 0);
    EXPECT_EQ(m1_row.color.b, 0);
}

TEST_F(ApiFixture, PurposeCountAndAtAreZeroOrInvalidForNullHandleOrNoViewLayerSetYet)
{
    EXPECT_EQ(purpose_count(), 0); // no LEF read yet - no ViewLayerSet built

    EXPECT_EQ(purpose_at(0), -1);
}

TEST_F(ApiFixture, PurposeAtListsRowThenBoundaryThenTerminalObstruction)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    // The "columns" axis - row-independent, not scoped to M1 or any other
    // specific layer (see ViewLayerSet::purposes()'s own comment). This
    // walks ViewLayerSet::rows() in its own declaration order (also the
    // real draw z-order): ROW, BOUNDARY and PLACEMENT pseudo-rows first;
    // then the per-Layer TERMINAL/OBSTRUCTION/TRACK_PREFERRED/
    // TRACK_NON_PREFERRED/ROUTING_BLOCKAGE/ROUTE/CUSTOM_SHAPE columns; then
    // any per_layer renderable classes' columns; then
    // the GCELLGRID/PLACEMENT_BLOCKAGE/REGION/... pseudo-rows. The raw ordinal values
    // below are le::ViewLayerPurpose's own declaration order, unrelated to
    // this traversal order.
    ASSERT_EQ(purpose_count(), 16 + static_cast<int32_t>(le::renderable::kCount)); // + extensions' purposes, last
    EXPECT_EQ(purpose_at(0), 6);   // ROW
    EXPECT_EQ(purpose_at(1), 2);   // BOUNDARY
    EXPECT_EQ(purpose_at(2), 11);  // PLACEMENT
    EXPECT_EQ(purpose_at(3), 0);   // TERMINAL
    EXPECT_EQ(purpose_at(4), 1);   // OBSTRUCTION
    EXPECT_EQ(purpose_at(5), 3);   // TRACK_PREFERRED
    EXPECT_EQ(purpose_at(6), 4);   // TRACK_NON_PREFERRED
    EXPECT_EQ(purpose_at(7), 5);   // ROUTING_BLOCKAGE
    EXPECT_EQ(purpose_at(8), 9);   // ROUTE
    EXPECT_EQ(purpose_at(9), 12);  // CUSTOM_SHAPE
    const int32_t p = static_cast<int32_t>(le::renderable::kPerLayerCount);
    EXPECT_EQ(purpose_at(p + 10), 7); // GCELLGRID
    EXPECT_EQ(purpose_at(p + 11), 8); // PLACEMENT_BLOCKAGE
    EXPECT_EQ(purpose_at(p + 12), 10); // REGION
    EXPECT_EQ(purpose_at(p + 13), 13); // DEBUG
    EXPECT_EQ(purpose_at(p + 14), 14); // FLIGHTLINE
    EXPECT_EQ(purpose_at(p + 15), 15); // PORT_MARKER
}

TEST_F(ApiFixture, LayerNameVisibilityDefaultsTrueAndRoundTrips)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    EXPECT_NE(read().layer_visible("M1"), 0);
    // Unknown-to-null-handle/name default matches LeHandle's own default.

    write().set_layer_visible("M1", 0);
    EXPECT_EQ(read().layer_visible("M1"), 0);
    // A different, never-toggled layer name is unaffected.
    EXPECT_NE(read().layer_visible("BOUNDARY"), 0);

    write().set_layer_visible("M1", 1);
    EXPECT_NE(read().layer_visible("M1"), 0);
}

TEST_F(ApiFixture, ReadLefDefaultsNonRoutingCutLayersToHidden)
{
    // mixed_layer_types.lef: M1 (ROUTING), V1 (CUT), OVERLAP (OVERLAP),
    // SLICE (MASTERSLICE) - only ROUTING/CUT/BOUNDARY should default
    // visible.
    ASSERT_TRUE(read_lef(fixture_path("mixed_layer_types.lef"), "mixed_layer_types"));

    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_NE(read().layer_visible("V1"), 0);
    EXPECT_EQ(read().layer_visible("OVERLAP"), 0);
    EXPECT_EQ(read().layer_visible("SLICE"), 0);
    // BOUNDARY isn't a physical layer (no LayerId of its own), so it's
    // untouched by the new default-hiding pass and stays visible via
    // LeHandle's own default-true-until-toggled behavior.
    EXPECT_NE(read().layer_visible("BOUNDARY"), 0);
}

TEST_F(ApiFixture, ReadLefDefaultHidingOnlyAppliesToNewlyIntroducedLayers)
{
    ASSERT_TRUE(read_lef(fixture_path("mixed_layer_types.lef"), "mixed_layer_types"));
    ASSERT_EQ(read().layer_visible("OVERLAP"), 0);

    write().set_layer_visible("OVERLAP", 1); // user explicitly reveals it via the layer manager
    ASSERT_NE(read().layer_visible("OVERLAP"), 0);

    // A second read_lef call (e.g. a macro file with its own inline
    // LAYER declarations) must not re-default OVERLAP back to hidden -
    // only layers newly introduced by *this* read get defaulted (see
    // read_lef's own comment). via_pairing.lef doesn't declare an
    // OVERLAP layer at all.
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing"));
    EXPECT_NE(read().layer_visible("OVERLAP"), 0);
}

TEST_F(ApiFixture, DigitKeysToggleTheNthRoutingLayerAndPairTheCutLayerBetweenThem)
{
    // via_pairing.lef: M1 (ROUTING, index 0 -> le::Key::DIGIT_1), V1 (CUT, between
    // M1/M2), M2 (ROUTING, index 1 -> le::Key::DIGIT_2). No MACRO needed - the
    // Technology's own layer list is populated straight from LAYER
    // statements (see LEFReader::lefrLayerCbkFn), independent of any
    // Library/Design.
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing"));

    ASSERT_NE(read().layer_visible("M1"), 0); // default-visible
    ASSERT_NE(read().layer_visible("M2"), 0);
    ASSERT_NE(read().layer_visible("V1"), 0);

    write().key_down(le::Key::DIGIT_1); // M1 -> hidden
    EXPECT_EQ(read().layer_visible("M1"), 0);
    EXPECT_EQ(read().layer_visible("V1"), 0); // M1/M2 no longer both visible

    write().key_down(le::Key::DIGIT_2); // M2 -> hidden too (M1 already hidden)
    EXPECT_EQ(read().layer_visible("M2"), 0);
    EXPECT_EQ(read().layer_visible("V1"), 0); // still not both visible

    write().key_down(le::Key::DIGIT_1); // M1 -> visible again; M2 still hidden
    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_EQ(read().layer_visible("V1"), 0);

    write().key_down(le::Key::DIGIT_2); // M2 -> visible again; now both M1/M2 visible
    EXPECT_NE(read().layer_visible("M2"), 0);
    EXPECT_NE(read().layer_visible("V1"), 0); // V1 paired on

    write().key_down(le::Key::DIGIT_1); // M1 -> hidden again
    EXPECT_EQ(read().layer_visible("M1"), 0);
    EXPECT_EQ(read().layer_visible("V1"), 0); // V1 paired back off
}

TEST_F(ApiFixture, ManualLayerVisibilityDoesNotTriggerViaPairing)
{
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing"));

    write().set_layer_visible("M1", 0);
    write().set_layer_visible("M2", 0);
    write().set_layer_visible("V1", 0);
    ASSERT_EQ(read().layer_visible("V1"), 0);

    // Manually re-visible-ing both routing layers through the direct API
    // (as a layer-manager click would) must not re-trigger the pairing
    // logic - that's only wired into the le::Key::DIGIT_1..le::Key::DIGIT_9 path.
    write().set_layer_visible("M1", 1);
    write().set_layer_visible("M2", 1);
    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_NE(read().layer_visible("M2"), 0);
    EXPECT_EQ(read().layer_visible("V1"), 0); // still hidden - no pairing happened
}

TEST_F(ApiFixture, DigitKeyWithNoNthRoutingLayerIsANoOp)
{
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing"));

    // Only two ROUTING layers (M1, M2) exist - le::Key::DIGIT_3..le::Key::DIGIT_9 have no
    // 3rd+ routing layer to toggle.
    write().key_down(le::Key::DIGIT_3);
    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_NE(read().layer_visible("M2"), 0);
    EXPECT_NE(read().layer_visible("V1"), 0);
}

TEST_F(ApiFixture, DigitKey0TogglesTheTenthRoutingLayer)
{
    // many_routing_layers.lef: M1..M12 ROUTING, with V10 between M10/M11
    // and V11 between M11/M12 - le::Key::DIGIT_0 addresses M10 (the 10th ROUTING
    // layer, routing_index 9), unconditional on le::Key::CTRL.
    ASSERT_TRUE(read_lef(fixture_path("many_routing_layers.lef"), "many_routing_layers"));
    ASSERT_NE(read().layer_visible("M10"), 0); // default-visible

    write().key_down(le::Key::DIGIT_0);
    EXPECT_EQ(read().layer_visible("M10"), 0);

    write().key_down(le::Key::DIGIT_0);
    EXPECT_NE(read().layer_visible("M10"), 0);
}

TEST_F(ApiFixture, CtrlPlusDigitKeysToggleTheEleventhAndTwelfthRoutingLayersAndPairTheirCutLayers)
{
    ASSERT_TRUE(read_lef(fixture_path("many_routing_layers.lef"), "many_routing_layers"));

    ASSERT_NE(read().layer_visible("M10"), 0);
    ASSERT_NE(read().layer_visible("M11"), 0);
    ASSERT_NE(read().layer_visible("M12"), 0);
    ASSERT_NE(read().layer_visible("V10"), 0);
    ASSERT_NE(read().layer_visible("V11"), 0);

    // Without Ctrl held, le::Key::DIGIT_1/le::Key::DIGIT_2 address M1/M2, not M11/M12 -
    // M10/M11/M12 stay untouched.
    write().key_down(le::Key::DIGIT_1);
    write().key_down(le::Key::DIGIT_2);
    EXPECT_NE(read().layer_visible("M11"), 0);
    EXPECT_NE(read().layer_visible("M12"), 0);
    write().key_down(le::Key::DIGIT_1); // toggle M1/M2 back off, restoring the fixture to its default state
    write().key_down(le::Key::DIGIT_2);

    // Ctrl held: le::Key::DIGIT_1 -> M11 (hides it), pairing re-check drops both
    // V10 (M10/M11 no longer both visible) and V11 (M11/M12 no longer
    // both visible).
    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DIGIT_1);
    EXPECT_EQ(read().layer_visible("M11"), 0);
    EXPECT_EQ(read().layer_visible("V10"), 0);
    EXPECT_EQ(read().layer_visible("V11"), 0);

    // le::Key::DIGIT_1 again (Ctrl still held) -> M11 visible again; both pairs
    // are back to both-visible, so both cut layers reappear.
    write().key_down(le::Key::DIGIT_1);
    EXPECT_NE(read().layer_visible("M11"), 0);
    EXPECT_NE(read().layer_visible("V10"), 0);
    EXPECT_NE(read().layer_visible("V11"), 0);

    // le::Key::DIGIT_2 (Ctrl held) -> M12 (hides it); only the M11/M12 pair (V11)
    // is affected - M10/M11 (V10) is untouched.
    write().key_down(le::Key::DIGIT_2);
    EXPECT_EQ(read().layer_visible("M12"), 0);
    EXPECT_NE(read().layer_visible("V10"), 0);
    EXPECT_EQ(read().layer_visible("V11"), 0);
}

TEST_F(ApiFixture, DigitKey0WithNoTenthRoutingLayerIsANoOp)
{
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing")); // only M1, M2 ROUTING

    write().key_down(le::Key::DIGIT_0);
    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_NE(read().layer_visible("M2"), 0);
    EXPECT_NE(read().layer_visible("V1"), 0);
}

TEST_F(ApiFixture, CtrlPlusDigitKeyWithNoLayerAtThatPositionIsANoOp)
{
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing")); // only M1, M2 ROUTING - no 11th

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DIGIT_1);
    EXPECT_NE(read().layer_visible("M1"), 0);
    EXPECT_NE(read().layer_visible("M2"), 0);
    EXPECT_NE(read().layer_visible("V1"), 0);
}

TEST_F(ApiFixture, DigitKeysWithShiftHeldAreANoOp)
{
    ASSERT_TRUE(read_lef(fixture_path("via_pairing.lef"), "via_pairing"));

    write().key_down(le::Key::SHIFT);
    write().key_down(le::Key::DIGIT_1);
    EXPECT_NE(read().layer_visible("M1"), 0); // unchanged

    write().key_down(le::Key::DIGIT_0);
    EXPECT_NE(read().layer_visible("M2"), 0); // unchanged

    write().key_down(le::Key::CTRL); // Ctrl+Shift both held
    write().key_down(le::Key::DIGIT_1);
    EXPECT_NE(read().layer_visible("M1"), 0); // still unchanged
}

TEST_F(ApiFixture, DigitKeyWithNoTechnologyReadYetDoesNotCrash)
{
    write().key_down(le::Key::DIGIT_1); // no read_lef call at all
}

TEST_F(ApiFixture, PurposeVisibilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(read().purpose_visible(static_cast<le::ViewLayerPurpose>(1)), 0); // OBSTRUCTION

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(1), 0);
    EXPECT_EQ(read().purpose_visible(static_cast<le::ViewLayerPurpose>(1)), 0);
    // A different, never-toggled purpose is unaffected.
    EXPECT_NE(read().purpose_visible(static_cast<le::ViewLayerPurpose>(0)), 0); // TERMINAL

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(1), 1);
    EXPECT_NE(read().purpose_visible(static_cast<le::ViewLayerPurpose>(1)), 0);
}

TEST(PurposeRegistry, EveryOrdinalHasAUniqueLabel)
{
    std::set<std::string_view> labels;
    for (size_t ordinal = 0; ordinal < le::kViewLayerPurposeCount; ++ordinal)
    {
        const le::ViewLayerPurposeInfo &info = le::kViewLayerPurposes[ordinal];
        EXPECT_EQ(static_cast<size_t>(info.purpose), ordinal);
        EXPECT_FALSE(info.label.empty()) << ordinal;
        EXPECT_TRUE(labels.insert(info.label).second) << "duplicate label " << info.label;
    }
}

TEST(ObjectKinds, EveryKindHasAUniqueName)
{
    std::set<std::string_view> names;
    const auto add = [&](const le::ObjectId &id) { EXPECT_TRUE(names.insert(le::object_kind_name(id)).second) << le::object_kind_name(id); };
    [&]<size_t... I>(std::index_sequence<I...>) { (add(le::ObjectId{std::in_place_index<I>}), ...); }(std::make_index_sequence<std::variant_size_v<le::ObjectId>>{});
    EXPECT_EQ(le::object_kind_name(le::TerminalPortId{}), "terminal_port");
    EXPECT_TRUE(le::object_kind_is_named(le::LibraryId{}));
    EXPECT_TRUE(le::object_kind_is_named(le::PlacementId{}));
    EXPECT_FALSE(le::object_kind_is_named(le::ShapeId{}));
    EXPECT_FALSE(le::object_kind_is_named(le::AbstractId{}));
}

TEST_F(ApiFixture, ObjectRefsReachEveryReadableClass)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "test_lib"));
    const le::LayerId m1 = layer_named("M1");
    ASSERT_NE(m1.index, UINT32_MAX);
    const le::ObjectId layer_ref{m1};
    EXPECT_GT(static_cast<int32_t>(properties(layer_ref).size()), 0);

    const std::optional<le::ObjectId> technology_ref = parent(layer_ref);
    EXPECT_TRUE(is<le::TechnologyId>(technology_ref));
    EXPECT_GT(static_cast<int32_t>(properties(technology_ref).size()), 0);
    EXPECT_FALSE(parent(technology_ref)); // Technology has no parent
}

TEST_F(ApiFixture, FreshHandleHidesExactlyThePurposesHiddenByDefault)
{
    for (const le::ViewLayerPurposeInfo &info : le::kViewLayerPurposes)
        EXPECT_EQ(read().purpose_visible(info.purpose), info.visible_by_default) << info.label;
}

TEST_F(ApiFixture, LayerNameSelectabilityDefaultsTrueAndRoundTrips)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    EXPECT_NE(read().layer_selectable("M1"), 0);

    write().set_layer_selectable("M1", 0);
    EXPECT_EQ(read().layer_selectable("M1"), 0);

    write().set_layer_selectable("M1", 1);
    EXPECT_NE(read().layer_selectable("M1"), 0);
}

TEST_F(ApiFixture, PurposeSelectabilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(read().purpose_selectable(static_cast<le::ViewLayerPurpose>(1)), 0); // OBSTRUCTION

    write().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(1), 0);
    EXPECT_EQ(read().purpose_selectable(static_cast<le::ViewLayerPurpose>(1)), 0);

    write().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(1), 1);
    EXPECT_NE(read().purpose_selectable(static_cast<le::ViewLayerPurpose>(1)), 0);
}

TEST_F(ApiFixture, HidingALayerByNameRemovesItFromTheRenderedBuffer)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A visible

    write().set_layer_visible("M1", 0); // hides both M1/TERMINAL and M1/OBSTRUCTION

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // M1 is hidden now
}

TEST_F(ApiFixture, HidingATerminalPurposeRemovesItFromTheRenderedBuffer)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A (TERMINAL) visible

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(0), 0); // TERMINAL off, across every layer

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // PIN A's TERMINAL column is hidden now
}

TEST_F(ApiFixture, GridSpacingDefaultsMatchScene)
{
    EXPECT_EQ(read().minor_grid_spacing(), 5);
    EXPECT_EQ(read().major_grid_spacing(), 50);
}

TEST_F(ApiFixture, GridSpacingRoundTrips)
{
    write().set_minor_grid_spacing(10);
    write().set_major_grid_spacing(100);

    EXPECT_EQ(read().minor_grid_spacing(), 10);
    EXPECT_EQ(read().major_grid_spacing(), 100);
}

TEST_F(ApiFixture, GridSpacingIgnoresNonPositiveValues)
{
    write().set_minor_grid_spacing(10);
    write().set_minor_grid_spacing(0);
    write().set_minor_grid_spacing(-5);
    EXPECT_EQ(read().minor_grid_spacing(), 10); // unchanged

    write().set_major_grid_spacing(100);
    write().set_major_grid_spacing(0);
    write().set_major_grid_spacing(-5);
    EXPECT_EQ(read().major_grid_spacing(), 100); // unchanged
}

TEST_F(ApiFixture, RulerLabelSizeDefaultsMatchesScene)
{
    EXPECT_DOUBLE_EQ(read().ruler_label_size(), 11.0);
}

TEST_F(ApiFixture, RulerLabelSizeRoundTrips)
{
    write().set_ruler_label_size(20.0);
    EXPECT_DOUBLE_EQ(read().ruler_label_size(), 20.0);
}

TEST_F(ApiFixture, RulerLabelSizeIgnoresNonPositiveValues)
{
    write().set_ruler_label_size(20.0);
    write().set_ruler_label_size(0.0);
    write().set_ruler_label_size(-5.0);
    EXPECT_DOUBLE_EQ(read().ruler_label_size(), 20.0); // unchanged
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionUntilMouseIsSet)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    write().set_viewport_size(100, 100);
    const std::optional<le::PointUm> result = snapped_mouse_position();
    EXPECT_FALSE(result);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionWithoutATechnologyLoaded)
{
    // No read_lef call at all - degrades gracefully instead of dividing
    // by a Technology::database_units_microns that doesn't exist yet.
    write().set_viewport_size(100, 100);
    write().set_mouse_position(50, 50);

    const std::optional<le::PointUm> result = snapped_mouse_position();
    EXPECT_FALSE(result);
}

TEST_F(ApiFixture, SnappedMousePositionReturnsGridSnappedMicronCoordinates)
{
    // testcell.lef declares DATABASE MICRONS 1000 - 1000 dbu per micron.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    write().set_viewport_size(100, 100);
    write().set_minor_grid_spacing(10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70
    // -> 0.02um and 0.07um at 1000 dbu/um.
    write().set_mouse_position(23, 27);

    const std::optional<le::PointUm> result = snapped_mouse_position();
    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result->x, 0.02);
    EXPECT_DOUBLE_EQ(result->y, 0.07);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionAfterClear)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    write().set_viewport_size(100, 100);
    write().set_mouse_position(50, 50);
    ASSERT_TRUE(snapped_mouse_position());

    write().clear_mouse_position();
    EXPECT_FALSE(snapped_mouse_position());
}

TEST_F(ApiFixture, RenderPixelBufferShowsRedCursorBoxAtSnappedMousePosition)
{
    write().set_viewport_size(100, 100);
    write().zoom(1.0, 0, 100); // scale 1.0 -> 2.0, anchored at dbu (0,0) so pan stays (0,0) - default minor spacing (5dbu*2=10px) clears the density floor

    // Screen pixel (50,50), top-left origin/y-down (same convention as
    // zoom's x/y) -> dbu (25,25), already a multiple of the default
    // 5dbu minor grid, so it snaps to itself and the box lands centered
    // on screen pixel (50,50) after compose_with_overlays's Y-flip.
    write().set_mouse_position(50, 50);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, RenderPixelBufferHasNoCursorBoxWhenNoMousePositionSet)
{
    write().set_viewport_size(100, 100);
    write().zoom(1.0, 0, 100);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ClearMousePositionRemovesTheCursorBoxFromSubsequentRenders)
{
    write().set_viewport_size(100, 100);
    write().zoom(1.0, 0, 100);
    write().set_mouse_position(50, 50);

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_red_cursor_pixel(before, 40, 40, 60, 60));

    write().clear_mouse_position();
    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(after, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ZoomWithDegenerateFactorLeavesScaleAndPanUnchanged)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    // A factor <= -1.0 would make new_scale non-positive - must be
    // ignored entirely (same guard as LeHandle::set_scale), not clamp to
    // some fallback value.
    write().zoom(-1.0, 50, 50);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect still visible exactly where it was
}

TEST_F(ApiFixture, ZoomKeepsTheAnchorPixelFixedOnScreen)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    // A small window around (50, 50) rather than that one exact pixel -
    // PIN A's ROUTING-layer fill is a tiled stripe pattern, so a single
    // point could land in a transparent gap by coincidence of phase.
    ASSERT_TRUE(region_has_opaque_pixel(before, 43, 43, 57, 57)); // baseline: pin rect visible around (50, 50)

    // Zoom in 2x anchored at the same pixel being sampled - the dbu point
    // under (50, 50) must still be under (50, 50) afterward, so it should
    // still show the same pin rect content there.
    write().zoom(1.0, 50, 50);

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 43, 43, 57, 57));
}

TEST_F(ApiFixture, PanShiftsContentOutOfView)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: pin rect visible

    // Pan by 2 full viewport widths in dbu x - the whole 10000-dbu-wide
    // macro (well under one viewport width at this scale) ends up entirely
    // out of view.
    write().pan(2.0, 0.0);

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownZoomZoomsInAnchoredAtTheCurrentMousePosition)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_opaque_pixel(before, 13, 48, 17, 52)); // just outside the pin's left edge

    write().set_mouse_position(50, 50); // center of the macro/pin
    write().key_down(le::Key::ZOOM);

    // Zooming in (factor +0.3, scale 0.01 -> 0.013) grows the pin's
    // half-width from 30px to 39px around the same (50,50) anchor - the
    // sample window that was just outside it before is now inside.
    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 13, 48, 17, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithShiftHeldZoomsOutInstead)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 23, 48, 27, 52)); // inside the pin, near its left edge

    write().set_mouse_position(50, 50);
    write().key_down(le::Key::SHIFT);
    write().key_down(le::Key::ZOOM);

    // Zooming out (factor -0.3, scale 0.01 -> 0.007) shrinks the pin's
    // half-width from 30px to 21px around the same anchor - the sample
    // window that was inside it before is now outside. Sampled at x:18-22
    // rather than reusing the "before" window (23-27): the mouse position
    // set above also puts the pin under hover, and its outline's stroke
    // antialiasing extends a pixel or two past the pin's own new edge
    // (~x=29) toward the old window, so 23-27 isn't a clean miss anymore.
    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 18, 48, 22, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithoutAMousePositionSetDoesNotCrash)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);

    write().key_down(le::Key::ZOOM); // no set_mouse_position call first - anchors at the default (0,0)

    le::RenderedFrame buffer = render();
    EXPECT_NE(buffer.data, nullptr);
}

TEST_F(ApiFixture, ZoomDragFitsTheDraggedRectToTheViewport)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);

    // The view starts at pan (0,0)/scale 1.0. Drag from pixel (0,100) [dbu
    // (0,0), via pixel_to_dbu] to pixel (10000,-9900) [dbu (10000,10000)] -
    // chosen so the resulting drag rect is exactly the macro's own
    // (0,0)-(10000,10000) bbox. Feeding that into fit_to_content (padding
    // 0, 100x100 viewport) lands on scale 0.01 / pan (0,0) - the same
    // state the existing write().zoom(100.0/10000.0-1.0, 0, 100)-based
    // zoom tests use - so this reuses their already-verified pixel
    // assertions (PIN A at device (20,20)-(80,80)) as proof the rect-zoom
    // math matches LeHandle::fit_to_content exactly.
    write().zoom_drag_down(0, 100);
    write().mouse_up(10000, -9900);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 23, 48, 27, 52)); // inside the pin, near its left edge
    EXPECT_FALSE(region_has_opaque_pixel(buffer, 13, 48, 17, 52)); // just outside the pin's left edge
}

TEST_F(ApiFixture, ClickSizedZoomDragDoesNotChangeTheView)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    write().zoom_drag_down(50, 50);
    write().mouse_up(51, 51); // dx=1,dy=1 - well under kClickDragThresholdPx, so end_drag() with no fit_to_content call

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 23, 48, 27, 52));
    EXPECT_FALSE(region_has_opaque_pixel(buffer, 13, 48, 17, 52));
}

TEST_F(ApiFixture, ZoomDragWithoutAMouseUpLeavesTheSceneStillDragging)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);

    write().zoom_drag_down(10, 10);

    le::RenderedFrame buffer = render();
    EXPECT_NE(buffer.data, nullptr); // no crash while a zoom drag is in progress but never released
}

TEST_F(ApiFixture, SelectDragRectangleIsBlueZoomDragRectangleIsGreen)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);

    // Default pan (0,0)/scale 1.0 puts the visible dbu range at
    // (0,0)-(100,100) - far smaller than PIN A's (2000,2000)-(8000,8000)
    // rect, so nothing from the design itself renders here; the grid dots
    // (see draw_grid) are grayscale (R==G==B) and contribute equally to
    // every channel, so comparing the drag rect's own green vs. blue
    // channel is robust regardless of whether a grid dot lands on the
    // sampled pixel.
    write().mouse_down(10, 10);
    write().set_mouse_position(90, 90); // LeHandle::drag_rect_dbu() needs a stored mouse position, not just the down-event x/y

    le::RenderedFrame select_buffer = render();
    ASSERT_NE(select_buffer.data, nullptr);
    const uint8_t *select_pixel = select_buffer.data + static_cast<size_t>(50) * static_cast<size_t>(select_buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(select_pixel[3], 0);
    EXPECT_GT(select_pixel[2], select_pixel[1]); // kDragRectFillColor = {80,160,255,60} - blue > green

    write().mouse_up(90, 90);

    write().zoom_drag_down(10, 10);
    write().set_mouse_position(90, 90);

    le::RenderedFrame zoom_buffer = render();
    ASSERT_NE(zoom_buffer.data, nullptr);
    const uint8_t *zoom_pixel = zoom_buffer.data + static_cast<size_t>(50) * static_cast<size_t>(zoom_buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(zoom_pixel[3], 0);
    EXPECT_GT(zoom_pixel[1], zoom_pixel[2]); // kZoomDragRectFillColor = {80,255,160,60} - green > blue
}

TEST_F(ApiFixture, KeyDownFitFitsTheViewportToContent)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);
    write().key_down(le::Key::FIT);

    // Mirrors FitSceneFillsTheViewportWithThePinVisible, triggered via
    // key_down instead of fit_view directly.
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, KeyDownPanLeftShiftsContentOutOfView)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    // kKeyPanFactor (0.25) per press - 8 presses covers 2 full viewport
    // widths, the same margin PanShiftsContentOutOfView uses directly.
    for (int i = 0; i < 8; ++i)
        write().key_down(le::Key::PAN_LEFT);

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownPanDirectionsAreEachOthersOpposite)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100);

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    write().key_down(le::Key::PAN_LEFT);
    write().key_down(le::Key::PAN_RIGHT); // undoes the left pan
    write().key_down(le::Key::PAN_UP);
    write().key_down(le::Key::PAN_DOWN); // undoes the up pan

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownPanWithCtrlOrShiftHeldIsANoOp)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100);

    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    write().key_down(le::Key::CTRL);
    for (int i = 0; i < 8; ++i)
        write().key_down(le::Key::PAN_LEFT);
    write().key_down(le::Key::SHIFT); // Ctrl+Shift both held
    for (int i = 0; i < 8; ++i)
        write().key_down(le::Key::PAN_LEFT);
    write().key_up(le::Key::CTRL);
    for (int i = 0; i < 8; ++i)
        write().key_down(le::Key::PAN_LEFT); // Shift alone

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // unchanged
}

// le::Key::SELECT_MODE/le::Key::EDIT_MODE.
TEST_F(ApiFixture, KeyDownEditModeSwitchesToEditMode)
{
    ASSERT_EQ(read().mode(), le::InteractionMode::SELECT);
    write().key_down(le::Key::EDIT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::EDIT);
}

TEST_F(ApiFixture, KeyDownSelectModeSwitchesBackToSelectMode)
{
    write().key_down(le::Key::EDIT_MODE);
    ASSERT_EQ(read().mode(), le::InteractionMode::EDIT);

    write().key_down(le::Key::SELECT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);
}

TEST_F(ApiFixture, KeyDownEditModeWithCtrlOrShiftHeldIsANoOp)
{
    // Mode-switch keys ('e'/'s'/'r') are bare-only: with a modifier held
    // (e.g. Ctrl-S) they must not steal the keystroke from whatever the
    // modifier combo actually means.
    ASSERT_EQ(read().mode(), le::InteractionMode::SELECT);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::EDIT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);

    write().key_down(le::Key::SHIFT); // Ctrl+Shift both held
    write().key_down(le::Key::EDIT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);

    write().key_up(le::Key::CTRL);
    write().key_down(le::Key::EDIT_MODE); // Shift alone
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);
}

TEST_F(ApiFixture, KeyDownSelectModeWithCtrlOrShiftHeldIsANoOp)
{
    write().key_down(le::Key::EDIT_MODE);
    ASSERT_EQ(read().mode(), le::InteractionMode::EDIT);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::SELECT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::EDIT);

    write().key_down(le::Key::SHIFT);
    write().key_down(le::Key::SELECT_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::EDIT);
}

TEST_F(ApiFixture, KeyDownRulerModeWithCtrlOrShiftHeldIsANoOp)
{
    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::RULER_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);

    write().key_down(le::Key::SHIFT);
    write().key_down(le::Key::RULER_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::SELECT);

    write().key_up(le::Key::CTRL);
    write().key_up(le::Key::SHIFT);
    write().key_down(le::Key::RULER_MODE); // bare - actually switches
    EXPECT_EQ(read().mode(), le::InteractionMode::RULER);
}

TEST_F(ApiFixture, RenderPixelBufferDrawsThePinRectAtItsExpectedLocation)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    // MACRO SIZE is 10x10 microns, PIN A's RECT is (2,2)-(8,8) microns.
    // DATABASE MICRONS 1000 -> 1 micron = 1000 dbu. Scale chosen so the
    // whole 10x10 micron (10000x10000 dbu) macro fills a 100x100px buffer.
    // The view starts at scale 1.0 / pan (0, 0) - zoom to that scale
    // anchored at image pixel (0, 100) (dbu (0, 0) at the starting
    // pan/scale) keeps pan pinned at (0, 0) exactly.
    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100);

    le::RenderedFrame buffer = render();
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

// The Abstract view's hover outline was removed on request - moving the
// mouse over a selectable shape draws nothing but the cursor box.
TEST_F(ApiFixture, MouseMoveOverASelectableShapeDrawsNoHoverOutline)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    // Same pan/scale as RenderPixelBufferDrawsThePinRectAtItsExpectedLocation -
    // PIN A's rect ends up at device pixel (20,20)-(80,80).
    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100);

    write().set_mouse_position(50, 50); // well inside PIN A's rect

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52)); // no outline along the pin's left edge
}

TEST_F(ApiFixture, KeyDownThenIsKeyHeldReturnsTrue)
{
    EXPECT_EQ(read().key_held(le::Key::SHIFT), 0);

    write().key_down(le::Key::SHIFT);
    EXPECT_NE(read().key_held(le::Key::SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpClearsAHeldKey)
{
    write().key_down(le::Key::SHIFT);
    ASSERT_NE(read().key_held(le::Key::SHIFT), 0);

    write().key_up(le::Key::SHIFT);
    EXPECT_EQ(read().key_held(le::Key::SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpWithoutAPrecedingKeyDownIsANoOp)
{
    write().key_up(le::Key::SHIFT);
    EXPECT_EQ(read().key_held(le::Key::SHIFT), 0);
}

TEST_F(ApiFixture, ClearAllKeysReleasesAHeldShift)
{
    write().key_down(le::Key::SHIFT);
    ASSERT_NE(read().key_held(le::Key::SHIFT), 0);

    write().clear_keys();
    EXPECT_EQ(read().key_held(le::Key::SHIFT), 0);
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
    void load_two_shapes_at_known_scale(le::test::SessionHelpers &s)
    {
        ASSERT_TRUE(s.read_lef(fixture_path("two_shapes.lef"), "two_shapes"));
        ASSERT_TRUE(s.open_abstract(size_t(0)));

        s.write().set_viewport_size(200, 200);
        s.write().zoom(0.01 - 1.0, 0, 200);
    }
}

TEST_F(ApiFixture, CtrlDDeselectAllClearsTheSelection)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(0, 200); // drag over both pins
    write().mouse_up(200, 0);
    ASSERT_EQ(selection_count(), 2);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DESELECT_ALL);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, DeselectAllWithoutCtrlHeldIsANoOp)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().key_down(le::Key::DESELECT_ALL); // Ctrl never pressed
    EXPECT_EQ(selection_count(), 1);
}

TEST_F(ApiFixture, DeselectAllWhenNothingIsSelectedIsANoOp)
{
    load_two_shapes_at_known_scale(*this);
    ASSERT_EQ(selection_count(), 0);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DESELECT_ALL);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, DeselectAllWithCtrlAndShiftBothHeldIsANoOp)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(0, 200); // drag over both pins
    write().mouse_up(200, 0);
    ASSERT_EQ(selection_count(), 2);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::SHIFT); // Ctrl+Shift both held
    write().key_down(le::Key::DESELECT_ALL);
    EXPECT_EQ(selection_count(), 2); // unchanged
}

TEST_F(ApiFixture, CtrlFFitsTheViewportToOnlyTheSelectedShape)
{
    load_two_shapes_at_known_scale(*this);

    // (100,100) device is empty space at the baseline scale (see
    // load_two_shapes_at_known_scale's own comment) - neither pin reaches
    // the viewport center.
    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_opaque_pixel(before, 80, 80, 120, 120));

    write().mouse_down(25, 175); // PIN A only
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::FIT);

    // PIN A's own (1,1)-(4,4) micron bbox now fills the viewport (with
    // kKeyFitPaddingPx of margin) - the same center window that was empty
    // at the whole-design baseline scale is now well inside it.
    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 80, 80, 120, 120));
}

TEST_F(ApiFixture, FitSelectedWithNoSelectionLeavesTheViewUnchanged)
{
    load_two_shapes_at_known_scale(*this);
    ASSERT_EQ(selection_count(), 0);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::FIT);

    // Nothing to fit to - view stays at load_two_shapes_at_known_scale's
    // own baseline (see its comment for these exact device positions).
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 20, 170, 30, 180)); // PIN A
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 170, 20, 180, 30)); // PIN B
}

TEST_F(ApiFixture, PlainFKeyStillFitsTheWholeSceneEvenWithASelectionActive)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A only
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().key_down(le::Key::FIT); // Ctrl not held - whole-design fit, not Ctrl-F's fit-selected

    // Both pins should still be visible (generous quadrant windows -
    // fit_view's own computed scale/pan differs slightly from
    // load_two_shapes_at_known_scale's synthetic baseline, since it fits
    // with kKeyFitPaddingPx rather than that helper's own zoom trick, but
    // PIN A stays in the bottom-left quadrant and PIN B in the top-right
    // either way).
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 0, 140, 60, 200)); // PIN A's quadrant
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 140, 0, 200, 60)); // PIN B's quadrant
}

TEST_F(ApiFixture, MouseDownThenUpAsAClickSelectsTheHitShape)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175);
    write().mouse_up(25, 175);

    EXPECT_EQ(selection_count(), 1);
}

// A hidden ViewLayer (visibility off) must not be click-selectable even
// though it's still marked selectable=true (the default) - "selectable"
// means "eligible to be selected when visible", not "selectable
// regardless of visibility". The is_selectable predicate passed to
// hit_test_abstract_point checks both, as hover (set_mouse_position)
// does (see that test below).
TEST_F(ApiFixture, MouseClickDoesNotSelectATerminalOnAHiddenLayer)
{
    load_two_shapes_at_known_scale(*this);
    write().set_layer_visible("M1", false);

    write().mouse_down(25, 175); // PIN A's own device-space center
    write().mouse_up(25, 175);

    EXPECT_EQ(selection_count(), 0);
}


TEST_F(ApiFixture, SelectionVersionBumpsOnlyOnAnActualSelectionChange)
{
    load_two_shapes_at_known_scale(*this);

    const int64_t baseline = read().selection_version();

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);
    const int64_t after_select = read().selection_version();
    EXPECT_NE(after_select, baseline);

    // Reselecting the exact same shape (no shift, so it clears first then
    // reselects the same one) still changes it, since clear+reselect is
    // two real selection_ mutations even though the end state looks the
    // same - selection_version reflects LeHandle::selection_version()
    // directly, not a "did the final state differ" comparison.
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    EXPECT_NE(read().selection_version(), after_select);
    const int64_t after_reselect = read().selection_version();

    // A pure mouse-move (no selection change) must not bump it - this is
    // the whole point of exposing this counter.
    write().set_mouse_position(30, 170);
    EXPECT_EQ(read().selection_version(), after_reselect);
}

TEST_F(ApiFixture, SmallMovementBetweenDownAndUpIsStillTreatedAsAClick)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175);
    write().mouse_up(26, 176); // well under the click/drag threshold

    EXPECT_EQ(selection_count(), 1);
}

TEST_F(ApiFixture, ClickOnEmptySpaceClearsTheSelection)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().mouse_down(100, 100); // empty space
    write().mouse_up(100, 100);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, PlainClickReplacesThePreviousSelectionRatherThanAddingToIt)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().mouse_down(175, 25); // PIN B, no shift
    write().mouse_up(175, 25);
    EXPECT_EQ(selection_count(), 1); // still just one - B replaced A, not added
}

TEST_F(ApiFixture, ShiftClickAddsToTheSelection)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().mouse_down(175, 25); // PIN B, with shift
    write().key_down(le::Key::SHIFT);
    write().mouse_up(175, 25);
    write().key_up(le::Key::SHIFT);
    EXPECT_EQ(selection_count(), 2);
}

TEST_F(ApiFixture, ShiftClickOnEmptySpaceIsANoOp)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().mouse_down(100, 100); // empty space, with shift
    write().key_down(le::Key::SHIFT);
    write().mouse_up(100, 100);
    write().key_up(le::Key::SHIFT);
    EXPECT_EQ(selection_count(), 1); // unchanged
}

TEST_F(ApiFixture, ClickOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(*this);
    write().set_layer_selectable("M1", 0);

    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, DragSelectEnclosesEverySelectableShapeInTheRectangle)
{
    load_two_shapes_at_known_scale(*this);

    // Full-viewport drag - dbu (0,0) to (20,20) micron, the whole macro,
    // enclosing both pins.
    write().mouse_down(0, 200);
    write().mouse_up(200, 0);

    EXPECT_EQ(selection_count(), 2);
}

// A drag that's cancelled - cancel_drag
// (the GUI, on Escape or when the release went to another window) or the
// Escape key itself - selects nothing, and a stray release afterwards is a
// no-op rather than committing the stale rectangle.
TEST_F(ApiFixture, CancellingADragSelectsNothing)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(0, 200);
    write().set_mouse_position(200, 0);
    write().cancel_drag();
    write().mouse_up(200, 0);
    EXPECT_EQ(selection_count(), 0);

    write().mouse_down(0, 200);
    write().set_mouse_position(200, 0);
    write().key_down(le::Key::FINISH_RULER); // Escape
    write().key_up(le::Key::FINISH_RULER);
    write().mouse_up(200, 0);
    EXPECT_EQ(selection_count(), 0);

    // The same drag, not cancelled, selects both pins.
    write().mouse_down(0, 200);
    write().mouse_up(200, 0);
    EXPECT_EQ(selection_count(), 2);

    write().cancel_drag(); // no drag in progress - a no-op
    EXPECT_EQ(selection_count(), 2);
}

TEST_F(ApiFixture, DragSelectExcludesAShapeThatIsOnlyPartiallyEnclosed)
{
    load_two_shapes_at_known_scale(*this);

    // dbu (0,0)-(2.5,5) micron - clips through the left portion of PIN A
    // (1,1)-(4,4) micron without fully enclosing it, and nowhere near
    // PIN B.
    write().mouse_down(0, 150);
    write().mouse_up(25, 200);

    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, ShiftDragAddsToTheExistingSelectionRatherThanReplacingIt)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A, plain click
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    // Small rect tightly around just PIN B, with shift.
    write().mouse_down(155, 5);
    write().key_down(le::Key::SHIFT);
    write().mouse_up(195, 45);
    write().key_up(le::Key::SHIFT);
    EXPECT_EQ(selection_count(), 2);
}

TEST_F(ApiFixture, DragSelectOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(*this);
    write().set_layer_selectable("M1", 0);

    write().mouse_down(0, 200);
    write().mouse_up(200, 0);
    EXPECT_EQ(selection_count(), 0);
}

// Edit mode gates mouse_up's selection changes.
TEST_F(ApiFixture, ClickInEditModeDoesNotChangeSelection)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A, in Select mode
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().mouse_down(175, 25); // PIN B, in Edit mode
    write().mouse_up(175, 25);
    EXPECT_EQ(selection_count(), 1); // still just PIN A - unchanged
}

TEST_F(ApiFixture, DragSelectInEditModeDoesNotChangeSelection)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::EDIT);

    // Same full-viewport drag rectangle that enclosed both pins in Select
    // mode (see DragSelectEnclosesEverySelectableShapeInTheRectangle).
    write().mouse_down(0, 200);
    write().mouse_up(200, 0);

    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, MouseUpInEditModeStillEndsDragging)
{
    // No public is_dragging accessor exists, so this is verified
    // indirectly: mouse_up's own guard (`if (!is_dragging()) return`,
    // input.cpp) makes a *second* mouse-up with no preceding mouse-down a
    // no-op only if the first mouse-up's end_drag() actually ran despite
    // the Edit-mode gate skipping the selection block around it.
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::EDIT);

    write().mouse_down(25, 175);
    write().mouse_up(25, 175); // end_drag() must run even though selection is skipped

    write().set_mode(le::InteractionMode::SELECT);
    write().mouse_up(175, 25); // no preceding mouse-down - a no-op if dragging really ended
    EXPECT_EQ(selection_count(), 0);
}

// Ruler mode.
TEST_F(ApiFixture, ClickInRulerModeCommitsAPointAndLeavesSelectionUntouched)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A, in Select mode
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::RULER);
    write().set_mouse_position(100, 100); // real UI drives this via hover/move events before a click
    write().mouse_down(100, 100);
    write().mouse_up(100, 100);

    EXPECT_EQ(ruler_point_count(0), 1);
    EXPECT_EQ(selection_count(), 1); // unchanged
}

TEST_F(ApiFixture, DragInRulerModeDoesNotCommitAPoint)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(0, 200);
    write().mouse_down(0, 200);
    write().mouse_up(200, 0); // well beyond the click/drag threshold

    EXPECT_EQ(ruler_point_count(0), 0);
}

TEST_F(ApiFixture, ShiftHeldClickInRulerModeAllowsANonOrthogonalPoint)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(20, 180);
    write().mouse_down(20, 180);
    write().mouse_up(20, 180);
    ASSERT_EQ(ruler_point_count(0), 1);

    // Without shift, this would snap orthogonal (whichever axis moved
    // more wins, per LeHandle::ruler_next_point).
    write().key_down(le::Key::SHIFT);
    write().set_mouse_position(60, 140);
    write().mouse_down(60, 140);
    write().mouse_up(60, 140);
    write().key_up(le::Key::SHIFT);

    ASSERT_EQ(ruler_point_count(0), 2);
    const le::PointUm p0 = ruler_point_at(0, 0);
    const le::PointUm p1 = ruler_point_at(0, 1);
    EXPECT_NE(p0.x, p1.x);
    EXPECT_NE(p0.y, p1.y);
}

TEST_F(ApiFixture, RulerPointsComeBackMicronConverted)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(25, 175); // same device position as PIN A's center
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);

    ASSERT_EQ(ruler_point_count(0), 1);
    const le::PointUm p = ruler_point_at(0, 0);
    // load_two_shapes_at_known_scale's own comment: device (25,175) is
    // ~2.5 micron in from the macro's origin - just check it landed in
    // a sane, non-zero range rather than pin an exact grid-snapped value.
    EXPECT_GT(p.x, 0.0);
    EXPECT_GT(p.y, 0.0);
    EXPECT_LT(p.x, 10.0);
    EXPECT_LT(p.y, 10.0);
}

TEST_F(ApiFixture, EscKeyFinishesTheRulerKeepingEveryCommittedPoint)
{
    // Esc (le::Key::FINISH_RULER) finishes the active ruler without
    // touching any of its already-committed points.
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(20, 180);
    write().mouse_down(20, 180);
    write().mouse_up(20, 180);
    write().set_mouse_position(150, 30);
    write().mouse_down(150, 30);
    write().mouse_up(150, 30);
    ASSERT_EQ(ruler_point_count(0), 2);

    write().key_down(le::Key::FINISH_RULER);

    EXPECT_EQ(ruler_point_count(0), 2); // both points survive
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 1); // still just the one ruler
}

TEST_F(ApiFixture, EscKeyWithNoActiveRulerIsANoOp)
{
    write().key_down(le::Key::FINISH_RULER); // no crash, nothing to finish
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 0);
}

TEST_F(ApiFixture, FinishRulerThenNewRulerRequiresTheMinimumDistanceGuard)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(20, 180);
    write().mouse_down(20, 180);
    write().mouse_up(20, 180);
    ASSERT_EQ(ruler_point_count(0), 1);

    write().finish_ruler();
    ASSERT_EQ(static_cast<int>(read().rulers().size()), 1);

    // Only 1 device px away - well under the distance guard.
    write().set_mouse_position(21, 180);
    write().mouse_down(21, 180);
    write().mouse_up(21, 180);
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 1);
    EXPECT_EQ(ruler_point_count(0), 1);

    // Far enough away - starts a new ruler, leaving the first untouched.
    write().set_mouse_position(60, 180);
    write().mouse_down(60, 180);
    write().mouse_up(60, 180);
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 2);
    EXPECT_EQ(ruler_point_count(0), 1);
    EXPECT_EQ(ruler_point_count(1), 1);
}

TEST_F(ApiFixture, ZoomStillWorksInRulerModeMidRulerAndDoesNotMoveCommittedPoints)
{
    load_two_shapes_at_known_scale(*this);
    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(20, 180);
    write().mouse_down(20, 180);
    write().mouse_up(20, 180);
    ASSERT_EQ(ruler_point_count(0), 1);
    const le::PointUm before = ruler_point_at(0, 0);

    write().set_mouse_position(20, 180);
    write().key_down(le::Key::ZOOM); // zooms in, anchored at the current mouse position

    const le::PointUm after = ruler_point_at(0, 0);
    EXPECT_DOUBLE_EQ(before.x, after.x);
    EXPECT_DOUBLE_EQ(before.y, after.y);
}

TEST_F(ApiFixture, MouseDownAloneDoesNotChangeTheSelection)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175);
    EXPECT_EQ(selection_count(), 0); // nothing committed until mouse-up
}

TEST_F(ApiFixture, MouseUpWithoutAPrecedingMouseDownIsANoOp)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_up(25, 175);
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, ClickSelectingAShapeShowsAWhiteOutlineInTheRenderedBuffer)
{
    load_two_shapes_at_known_scale(*this);

    // Render once *before* selecting anything, so the render caches are
    // already warm at {AbstractId, viewport_version, visibility_version}
    // - none of which change when a selection is made. The next render
    // after selecting must still show the outline: selection_version is
    // part of what invalidates the composed frame.
    le::RenderedFrame before = render();
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_white_selection_pixel(before, 8, 170, 12, 180));

    write().mouse_down(25, 175); // PIN A, device (10,160)-(40,190)
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    le::RenderedFrame after = render();
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(after, 8, 170, 12, 180)); // PIN A's left edge
}

TEST_F(ApiFixture, ClearingTheSelectionRemovesTheWhiteOutline)
{
    load_two_shapes_at_known_scale(*this);

    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    // Render with the selection active first (and warm every downstream
    // cache at this selection_version), matching a real frontend that
    // redraws after every gesture - not just once at the very end.
    le::RenderedFrame selected = render();
    ASSERT_NE(selected.data, nullptr);
    ASSERT_TRUE(region_has_white_selection_pixel(selected, 8, 170, 12, 180));

    write().mouse_down(100, 100); // empty space - clears the selection
    write().mouse_up(100, 100);
    ASSERT_EQ(selection_count(), 0);

    le::RenderedFrame cleared = render();
    ASSERT_NE(cleared.data, nullptr);
    EXPECT_FALSE(region_has_white_selection_pixel(cleared, 8, 170, 12, 180));
}

TEST_F(ApiFixture, SwitchingToADifferentDesignClearsTheSelection)
{
    load_two_shapes_at_known_scale(*this); // design 0 = TWOSHAPES
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell")); // design 1 = TESTCELL

    write().mouse_down(25, 175); // PIN A on TWOSHAPES
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    ASSERT_TRUE(open_abstract(size_t(1))); // switch to TESTCELL
    EXPECT_EQ(selection_count(), 0);
}

TEST_F(ApiFixture, SwitchingToADifferentDesignClearsRulers)
{
    // Rulers must not leak across Abstracts - drawn in one Design's
    // abstract view, they must not keep showing up (at the same raw dbu
    // coordinates) after switching to a different Design.
    load_two_shapes_at_known_scale(*this); // design 0 = TWOSHAPES
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell")); // design 1 = TESTCELL

    write().set_mode(le::InteractionMode::RULER);
    write().set_mouse_position(25, 175);
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    ASSERT_EQ(static_cast<int>(read().rulers().size()), 1);

    ASSERT_TRUE(open_abstract(size_t(1))); // switch to TESTCELL
    EXPECT_EQ(static_cast<int>(read().rulers().size()), 0);
}

TEST_F(ApiFixture, ClearAllKeysMakesSubsequentClicksReplaceRatherThanAddAgain)
{
    // The exact reported bug: a stuck shift (e.g. from a missed key-up on
    // focus loss) made every later plain click behave like a shift-click.
    // clear_keys is the escape hatch - simulates a focus-loss
    // recovery.
    load_two_shapes_at_known_scale(*this);

    write().key_down(le::Key::SHIFT); // shift held, then "stuck" (no key-up)

    write().mouse_down(25, 175); // PIN A, shift-click
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().clear_keys(); // e.g. called from a focus-loss handler

    write().mouse_down(175, 25); // PIN B, plain click - should replace, not add
    write().mouse_up(175, 25);
    EXPECT_EQ(selection_count(), 1);
}

namespace
{
    // Shared by every selection-properties test below:
    // reads pin_and_obstruction.lef (MACRO PINOBS, PIN A at (1,1)-(4,4)
    // micron on M1, one OBS rect at (10,10)-(15,15) micron on M1 - see the
    // fixture file), and zooms a 200x200 viewport to scale 0.01 with pan
    // pinned at (0,0), same trick as load_two_shapes_at_known_scale. At
    // that scale/pan (device/image pixel space, top-left origin, y down):
    //   - PIN A occupies device (10,160)-(40,190) - center ~(25,175).
    //   - The OBS rect occupies device (100,50)-(150,100) - center ~(125,75).
    void load_pin_and_obstruction_at_known_scale(le::test::SessionHelpers &s)
    {
        ASSERT_TRUE(s.read_lef(fixture_path("pin_and_obstruction.lef"), "pin_and_obstruction"));
        ASSERT_TRUE(s.open_abstract(size_t(0)));

        s.write().set_viewport_size(200, 200);
        s.write().zoom(0.01 - 1.0, 0, 200);
    }

    // An object's property table as a name -> row map, so a test can look
    // up "does this property exist and does it have this value" without
    // hard-coding property order.
    std::map<std::string, le::PropertyValue> object_properties(le::test::SessionHelpers &s, const std::optional<le::ObjectId> &id)
    {
        std::map<std::string, le::PropertyValue> properties;
        for (le::PropertyValue &property : s.properties(id))
            properties.emplace(property.name, std::move(property));
        return properties;
    }

    // Same, but starting from a selection index - a piece's Shape or Wire.
    std::map<std::string, le::PropertyValue> selected_object_properties(le::test::SessionHelpers &s, int32_t selection_index)
    {
        return object_properties(s, s.selected(selection_index));
    }
}

TEST_F(ApiFixture, ClickSelectingAShapeReportsExactlyTheSamePropertiesAsGetPropertiesOnItsShapeId)
{
    // Clicking a shape must report the *exact* same rows properties (Tcl's
    // get_properties shape:<id>) shows for that same ShapeId - not a
    // pipeline-merged/derived summary. A selected piece reports its Shape,
    // so this compares the selection's properties with the ShapeId's.
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    const std::optional<le::ObjectId> ref = selected(0);
    EXPECT_TRUE(is<le::ShapeId>(ref));
    ASSERT_TRUE(ref);

    const le::ShapeId shape_id = std::get<le::ShapeId>(*ref);
    const int32_t via_selection_count = static_cast<int32_t>(properties(ref).size());
    const int32_t via_shape_id_count = static_cast<int32_t>(properties(le::ObjectId{shape_id}).size());
    ASSERT_EQ(via_selection_count, via_shape_id_count);

    for (int32_t i = 0; i < via_selection_count; ++i)
    {
        const le::PropertyValue via_selection = properties(ref).at(i);
        const le::PropertyValue via_shape_id = properties(le::ObjectId{shape_id}).at(i);
        EXPECT_EQ(via_selection.name, via_shape_id.name);
        EXPECT_EQ(via_selection.type, via_shape_id.type);
        switch (via_selection.type)
        {
        case le::PropertyValue::Type::STRING:
            EXPECT_EQ(via_selection.string_value, via_shape_id.string_value);
            break;
        case le::PropertyValue::Type::INT:
            EXPECT_EQ(via_selection.int_value, via_shape_id.int_value);
            break;
        case le::PropertyValue::Type::DOUBLE:
            EXPECT_DOUBLE_EQ(via_selection.double_value, via_shape_id.double_value);
            break;
        }
    }
}

TEST_F(ApiFixture, SelectedShapesParentChainReportsTerminalPortThenTerminal)
{
    // Selection reports the exact Shape hit (see the regression test
    // above) - a Property Viewer walks *up* from there via parent
    // to reach the owning TerminalPort/Terminal's own name/direction/
    // port_count, rather than those rows being folded into the Shape's
    // own property list the way the old selection-index path used to.
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    const std::optional<le::ObjectId> shape_ref = selected(0);
    ASSERT_TRUE(is<le::ShapeId>(shape_ref));

    const std::optional<le::ObjectId> port_ref = parent(shape_ref);
    EXPECT_TRUE(is<le::TerminalPortId>(port_ref));
    ASSERT_TRUE(port_ref);

    const std::optional<le::ObjectId> terminal_ref = parent(port_ref);
    EXPECT_TRUE(is<le::TerminalId>(terminal_ref));
    ASSERT_TRUE(terminal_ref);

    const std::map<std::string, le::PropertyValue> terminal_properties = object_properties(*this, terminal_ref);
    ASSERT_TRUE(terminal_properties.contains("name"));
    EXPECT_EQ(terminal_properties.at("name").string_value, "A");
    ASSERT_TRUE(terminal_properties.contains("direction"));
    EXPECT_EQ(terminal_properties.at("direction").string_value, "INPUT");
    ASSERT_TRUE(terminal_properties.contains("ports_count"));
    EXPECT_EQ(terminal_properties.at("ports_count").int_value, 1);

    // Library has no parent - the chain terminates gracefully.
    const std::optional<le::ObjectId> abstract_ref = parent(terminal_ref);
    EXPECT_TRUE(is<le::AbstractId>(abstract_ref));
    const std::optional<le::ObjectId> design_ref = parent(abstract_ref);
    EXPECT_TRUE(is<le::DesignId>(design_ref));
    const std::optional<le::ObjectId> library_ref = parent(design_ref);
    EXPECT_TRUE(is<le::LibraryId>(library_ref));
    const std::optional<le::ObjectId> no_parent = parent(library_ref);
    EXPECT_FALSE(no_parent);
}

TEST_F(ApiFixture, SelectedTerminalReportsItsRectConvertedToMicrons)
{
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A, (1,1)-(4,4) micron
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    const std::map<std::string, le::PropertyValue> properties = selected_object_properties(*this, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_EQ(properties.at("rects").type, le::PropertyValue::Type::STRING);
    EXPECT_EQ(properties.at("rects").string_value, "{{1 1} {4 4}}");
    ASSERT_TRUE(properties.contains("polygons"));
    EXPECT_EQ(properties.at("polygons").string_value, "");
    ASSERT_TRUE(properties.contains("paths"));
    EXPECT_EQ(properties.at("paths").string_value, "");
}

TEST_F(ApiFixture, SelectedTerminalRectTrimsTrailingZerosInWholeGroupsOfThree)
{
    // MACRO FRACPIN's PIN A is a RECT at (0.34,0.34)-(5.34,5.34) micron
    // (see fractional_pin.lef) - std::to_string would format each as 6
    // decimal digits ("0.340000"/"5.340000"); the trailing zeros should
    // trim down to the last *significant* group of three ("0.340"), not
    // strip further into the significant "340" group and not leave a
    // partial group like "0.34".
    ASSERT_TRUE(read_lef(fixture_path("fractional_pin.lef"), "fractional_pin"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);
    write().zoom(0.01 - 1.0, 0, 100);

    write().mouse_down(28, 72); // inside the rect - dbu (2800,2800)
    write().mouse_up(28, 72);
    ASSERT_EQ(selection_count(), 1);

    const std::map<std::string, le::PropertyValue> properties = selected_object_properties(*this, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_EQ(properties.at("rects").string_value, "{{0.340 0.340} {5.340 5.340}}");
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
    ASSERT_TRUE(read_lef(fixture_path("full_precision_pin.lef"), "full_precision_pin"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);
    write().zoom(0.01 - 1.0, 0, 100);

    write().mouse_down(50, 50); // inside the rect - dbu (5000,5000)
    write().mouse_up(50, 50);
    ASSERT_EQ(selection_count(), 1);

    const std::map<std::string, le::PropertyValue> properties = selected_object_properties(*this, 0);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_EQ(properties.at("rects").string_value, "{{0.000500 0.000500} {5.000500 5.000500}}");
}

TEST_F(ApiFixture, SelectedObstructionPieceReportsItsRectThenParentReportsShapesCount)
{
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(125, 75); // the OBS rect, (10,10)-(15,15) micron
    write().mouse_up(125, 75);
    ASSERT_EQ(selection_count(), 1);

    const std::optional<le::ObjectId> shape_ref = selected(0);
    EXPECT_TRUE(is<le::ShapeId>(shape_ref));

    const std::map<std::string, le::PropertyValue> shape_properties = object_properties(*this, shape_ref);
    ASSERT_TRUE(shape_properties.contains("rects"));
    EXPECT_EQ(shape_properties.at("rects").string_value, "{{10 10} {15 15}}");
    // Terminal/Obstruction-level property, should not leak onto a Shape's table.
    EXPECT_FALSE(shape_properties.contains("name"));
    EXPECT_FALSE(shape_properties.contains("shapes_count"));

    const std::optional<le::ObjectId> obstruction_ref = parent(shape_ref);
    EXPECT_TRUE(is<le::ObstructionId>(obstruction_ref));
    const std::map<std::string, le::PropertyValue> obstruction_properties = object_properties(*this, obstruction_ref);
    ASSERT_TRUE(obstruction_properties.contains("shapes_count"));
    EXPECT_EQ(obstruction_properties.at("shapes_count").type, le::PropertyValue::Type::INT);
    EXPECT_EQ(obstruction_properties.at("shapes_count").int_value, 1);
}

TEST_F(ApiFixture, SelectedObjectPropertiesDistinguishTwoSelectedObjectsByIndex)
{
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(25, 175); // PIN A
    write().mouse_up(25, 175);
    write().key_down(le::Key::SHIFT);
    write().mouse_down(125, 75); // OBS, shift-click adds
    write().mouse_up(125, 75);
    ASSERT_EQ(selection_count(), 2);

    // Both selections are shape-level (ShapeIds) - "distinguish
    // by index" now means each index's own Shape properties (own rect),
    // with name/shapes_count reachable one hop up via parent.
    const std::optional<le::ObjectId> first_ref = selected(0);
    const std::optional<le::ObjectId> second_ref = selected(1);
    EXPECT_TRUE(is<le::ShapeId>(first_ref));
    EXPECT_TRUE(is<le::ShapeId>(second_ref));

    const std::optional<le::ObjectId> terminal_ref = parent(parent(first_ref));
    EXPECT_TRUE(is<le::TerminalId>(terminal_ref));
    const std::map<std::string, le::PropertyValue> terminal_properties = object_properties(*this, terminal_ref);
    EXPECT_EQ(terminal_properties.at("name").string_value, "A");

    const std::optional<le::ObjectId> obstruction_ref = parent(second_ref);
    EXPECT_TRUE(is<le::ObstructionId>(obstruction_ref));
    const std::map<std::string, le::PropertyValue> obstruction_properties = object_properties(*this, obstruction_ref);
    EXPECT_EQ(obstruction_properties.at("shapes_count").int_value, 1);
}

TEST_F(ApiFixture, SelectionAndPropertiesAreOutOfRangeSafe)
{
    load_pin_and_obstruction_at_known_scale(*this);

    EXPECT_FALSE(selected(0)); // nothing selected
    EXPECT_TRUE(properties(le::ShapeId{}).empty());

    write().mouse_down(25, 175); // select PIN A
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    EXPECT_FALSE(selected(1)); // out of range
}

TEST_F(ApiFixture, ClickSelectingOnePortOfATwoPortTerminalReportsOnlyThatPortsRectAndLayer)
{
    // PIN B has two ports on M1 - (16,1)-(19,4) and (16,16)-(19,19)
    // micron (see pin_and_obstruction.lef) - device (10px/micron,
    // pan (0,0)): port 1 occupies device (160,160)-(190,190) (center
    // ~(175,175)), port 2 occupies device (160,10)-(190,40) (center
    // ~(175,25)).
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(175, 175); // PIN B's first port only
    write().mouse_up(175, 175);
    ASSERT_EQ(selection_count(), 1);
    const std::optional<le::ObjectId> shape_ref = selected(0);
    ASSERT_TRUE(is<le::ShapeId>(shape_ref));

    const std::map<std::string, le::PropertyValue> properties = object_properties(*this, shape_ref);
    ASSERT_TRUE(properties.contains("rects"));
    EXPECT_EQ(properties.at("rects").string_value, "{{16 1} {19 4}}"); // just the clicked port, not the union of both

    // Shape.layer is a reference field now (le::LayerId, not a plain str) -
    // build_shape_properties overwrites the generic property table's own
    // "layer" row with a friendly "layer:<name>" token (see
    // api_property_accessors_internal_inc_j2.py), so this checks the
    // real generic row directly rather than only its shape's layer.
    ASSERT_TRUE(properties.contains("layer"));
    EXPECT_EQ(properties.at("layer").string_value, "layer:M1");
    EXPECT_EQ(shape_layer_name(std::get<le::ShapeId>(*shape_ref)), "M1");

    // Parent-level context (Terminal's own name/port_count) is one hop up
    // via parent, not folded into the Shape's own row list.
    const std::optional<le::ObjectId> terminal_ref = parent(parent(shape_ref));
    ASSERT_TRUE(is<le::TerminalId>(terminal_ref));
    const std::map<std::string, le::PropertyValue> terminal_properties = object_properties(*this, terminal_ref);
    ASSERT_TRUE(terminal_properties.contains("name"));
    EXPECT_EQ(terminal_properties.at("name").string_value, "B");
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
    load_pin_and_obstruction_at_known_scale(*this);

    // Encloses both of PIN B's ports (device x:150-200 -> dbu 15-20
    // micron) without touching PIN A or the OBS rect.
    write().mouse_down(150, 0);
    write().mouse_up(200, 200);
    ASSERT_EQ(selection_count(), 2);
    EXPECT_TRUE(is<le::ShapeId>(selected(0)));
    EXPECT_TRUE(is<le::ShapeId>(selected(1)));

    const std::optional<le::ObjectId> first_ref = selected(0);
    const std::map<std::string, le::PropertyValue> first_properties = selected_object_properties(*this, 0);
    ASSERT_TRUE(first_properties.contains("rects"));
    const std::string first_rects = first_properties.at("rects").string_value;
    // Shape.layer is a reference field now (le::LayerId, not a plain str) -
    // build_shape_properties overwrites the generic property table's own
    // "layer" row with a friendly "layer:<name>" token (see
    // api_property_accessors_internal_inc_j2.py). Copied out immediately,
    // same as first_rects above - properties's own string_value
    // is only valid until the next call for a *different* ref, and
    // second_properties below is exactly that next call.
    ASSERT_TRUE(first_properties.contains("layer"));
    const std::string first_layer = first_properties.at("layer").string_value;

    const std::map<std::string, le::PropertyValue> second_properties = selected_object_properties(*this, 1);
    ASSERT_TRUE(second_properties.contains("rects"));
    const std::string second_rects = second_properties.at("rects").string_value;

    EXPECT_NE(first_rects, second_rects); // each port's own rect, not the aggregate
    EXPECT_EQ(first_rects, "{{16 1} {19 4}}");
    EXPECT_EQ(second_rects, "{{16 16} {19 19}}");
    EXPECT_EQ(first_layer, "layer:M1"); // a piece-level selection always reports its own layer
    EXPECT_EQ(shape_layer_name(std::get<le::ShapeId>(*first_ref)), "M1");
}

TEST_F(ApiFixture, ShiftClickingTwoPiecesOfTheSameTerminalSelectsBothIndependently)
{
    // The actual reported bug: shift-clicking a second shape within the
    // same Terminal must add a second, independently-selected/reportable
    // entry, not replace or no-op against the first one.
    load_pin_and_obstruction_at_known_scale(*this);

    write().mouse_down(175, 175); // PIN B's first port
    write().mouse_up(175, 175);
    ASSERT_EQ(selection_count(), 1);

    write().key_down(le::Key::SHIFT);
    write().mouse_down(175, 25); // PIN B's second port
    write().mouse_up(175, 25);
    write().key_up(le::Key::SHIFT);
    ASSERT_EQ(selection_count(), 2); // both pieces, not one

    EXPECT_TRUE(is<le::ShapeId>(selected(0)));
    EXPECT_TRUE(is<le::ShapeId>(selected(1)));

    // Copy each rects value out as its own std::string immediately -
    // LeProperty::string_value is only valid until the next
    // properties()/_count() call *for the same ref* (see its
    // doc comment); querying a different index (as `second`'s query
    // below does) invalidates `first`'s pointers.
    const std::map<std::string, le::PropertyValue> first_properties = selected_object_properties(*this, 0);
    ASSERT_TRUE(first_properties.contains("rects"));
    const std::string first_rects = first_properties.at("rects").string_value;

    const std::map<std::string, le::PropertyValue> second_properties = selected_object_properties(*this, 1);
    ASSERT_TRUE(second_properties.contains("rects"));
    const std::string second_rects = second_properties.at("rects").string_value;

    EXPECT_NE(first_rects, second_rects); // genuinely distinct pieces
    EXPECT_EQ(first_rects, "{{16 1} {19 4}}");
    EXPECT_EQ(second_rects, "{{16 16} {19 19}}");
}

TEST_F(ApiFixture, ConcurrentRenderAndMousePositionCallsOnTheSameHandleDoNotCrash)
{
    // Renderer::render (the GUI's render thread) and
    // set_mouse_position/zoom (the GUI thread, on every pointer
    // event) run concurrently on the same handle. Every LeHandle-touching
    // function locks the handle's own mutex; this drives both call paths
    // concurrently, repeatedly, and must complete without crashing,
    // deadlocking, or hanging.
    ASSERT_TRUE(read_concurrency_stress_lef(session, 3000));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);

    constexpr int kIterations = 50;
    std::thread render_thread([&]
                               {
        for (int i = 0; i < kIterations; ++i)
        {
            // Alternates a no-op-ish zoom so viewport_version() keeps
            // changing, forcing the render to actually recompute (not
            // just return an already-cached result) on most calls.
            write().zoom((i % 2 == 0) ? 0.001 : -0.001, 100, 100);
            const le::RenderedFrame buffer = render();
            EXPECT_NE(buffer.data, nullptr);
        } });

    for (int i = 0; i < kIterations; ++i)
        write().set_mouse_position(i % 200, (i * 7) % 200);

    render_thread.join();
}

TEST_F(ApiFixture, ConcurrentRendersFromTwoThreadsDoNotCrash)
{
    // The GUI render thread and the Tcl thread (dump_png) both call
    // Renderer::render; each reads its whole frame after the call
    // returns while the other keeps rendering.
    ASSERT_TRUE(read_concurrency_stress_lef(session, 3000));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);

    constexpr int kIterations = 50;
    auto render_and_copy = [&](int offset)
    {
        std::vector<uint8_t> copy;
        for (int i = 0; i < kIterations; ++i)
        {
            // A mouse move makes Compose recompute, replacing the
            // pipeline's cached frame.
            write().set_mouse_position((i + offset) % 200, (i * 7 + offset) % 200);
            const le::RenderedFrame buffer = render();
            ASSERT_NE(buffer.data, nullptr);
            copy.assign(buffer.data, buffer.data + buffer.row_bytes * buffer.height);
        }
    };
    std::thread other_thread(render_and_copy, 100);
    render_and_copy(0);
    other_thread.join();
}

TEST_F(ApiFixture, RenderedFrameStaysValidWhileAnotherThreadRenders)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(100, 100);
    write().fit_view(0);

    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    const std::vector<uint8_t> before(buffer.data, buffer.data + buffer.row_bytes * buffer.height);

    // Each of these renders replaces the pipeline's cached frame; this
    // thread's frame must survive them.
    std::thread other_thread([&]
                             {
        for (int i = 0; i < 5; ++i)
        {
            write().set_mouse_position(10 + i * 10, 10 + i * 10);
            ASSERT_NE(render().data, nullptr);
        } });
    other_thread.join();

    const std::vector<uint8_t> after(buffer.data, buffer.data + buffer.row_bytes * buffer.height);
    EXPECT_EQ(before, after);
}

TEST_F(ApiFixture, ViewportSizeGettersReturnTheSetSizeWithoutRendering)
{
    EXPECT_EQ(read().view().width_px, 0);
    EXPECT_EQ(read().view().height_px, 0);

    write().set_viewport_size(800, 600);
    EXPECT_EQ(read().view().width_px, 800);
    EXPECT_EQ(read().view().height_px, 600);
    EXPECT_EQ(session.renderer().is_rendering(), 0);
}

TEST_F(ApiFixture, IsRenderingReflectsWhetherARenderIsActuallyInProgress)
{
    // The GUI's "rendering..." indicator needs to observe a render
    // actually in progress from a
    // different thread, without blocking behind it (see Renderer::is_rendering's
    // own doc comment for why it deliberately doesn't take handle's own
    // mutex). Same concurrency-stress fixture as
    // ConcurrentRenderAndMousePositionCallsOnTheSameHandleDoNotCrash
    // above, reused here specifically because its own render takes real,
    // observable time: a render_thread does the actual render while this
    // thread tight-spin-polls Renderer::is_rendering concurrently, expecting to
    // catch it true at least once before the render finishes.
    ASSERT_TRUE(read_concurrency_stress_lef(session, 3000));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);

    EXPECT_EQ(session.renderer().is_rendering(), 0);

    std::atomic<bool> render_done{false};
    std::thread render_thread([&]
                               {
        const le::RenderedFrame buffer = render();
        EXPECT_NE(buffer.data, nullptr);
        render_done.store(true, std::memory_order_relaxed); });

    bool observed_rendering = false;
    while (!render_done.load(std::memory_order_relaxed))
    {
        if (session.renderer().is_rendering())
        {
            observed_rendering = true;
            break;
        }
    }

    render_thread.join();
    EXPECT_TRUE(observed_rendering) << "expected to observe le_is_rendering() true at least once during a real render";
    EXPECT_EQ(session.renderer().is_rendering(), 0) << "should be false again once the render has finished";
}

// A Tcl command (le_repl_eval brackets every one with begin/end) renders
// once when it ends, not after each mutation - a loop of deletes would
// otherwise re-render, and stall on the render's shared lock, per delete.
TEST_F(ApiFixture, RenderWaitIsHeldUntilTheCommandEnds)
{
    write().begin_command("held");
    std::promise<void> woke;
    std::future<void> woke_future = woke.get_future();
    std::thread waiter([&]
                       { session.renderer().wait_until_needed();
                         woke.set_value(); });

    write().set_viewport_size(120, 80);
    write().zoom(1.5, 60, 40);
    EXPECT_EQ(woke_future.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);

    write().end_command(1);
    EXPECT_EQ(woke_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    waiter.join();
}

TEST_F(ApiFixture, IsCommandRunningIsTrueBetweenBeginAndEndCommand)
{
    EXPECT_EQ(session.renderer().renders_held(), 0);
    write().begin_command("running");
    EXPECT_EQ(session.renderer().renders_held(), 1);
    write().end_command(1);
    EXPECT_EQ(session.renderer().renders_held(), 0);
}

// Window teardown cancels the wait to stop its render thread; that must
// work mid-command too, since a typed `exit` never reaches end_command.
TEST_F(ApiFixture, CancelRenderWaitWakesTheWaiterWhileACommandHoldsRenders)
{
    write().begin_command("held");
    write().set_viewport_size(120, 80);
    std::promise<void> woke;
    std::future<void> woke_future = woke.get_future();
    std::thread waiter([&]
                       { session.renderer().wait_until_needed();
                         woke.set_value(); });

    session.renderer().cancel_wait();
    const bool woke_in_time = woke_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!woke_in_time)
        write().end_command(1); // unblock the waiter so the join below returns
    waiter.join();
    EXPECT_TRUE(woke_in_time);
    write().end_command(1);
}

TEST_F(ApiFixture, TakeShowGuiRequestConsumesTheRequestExactlyOnce)
{
    // Dear ImGui prototype's own show_gui signal - a one-shot edge (a Tcl
    // console thread requesting the window open), not a level, so
    // take_show_gui_request() must clear it as part of reading it -
    // a second poll with no intervening request_show_gui() call must
    // not see a stale true.
    EXPECT_EQ(session.requests().take_show_gui_request(), 0) << "no request made yet";

    session.requests().request_show_gui();
    EXPECT_EQ(session.requests().take_show_gui_request(), 1) << "the just-made request should be observed";
    EXPECT_EQ(session.requests().take_show_gui_request(), 0) << "the request was already consumed by the previous call";

}

TEST_F(ApiFixture, PendingTclCommandQueueIsFifoAndDrainsToEmpty)
{
    // GUI components' own queued-Tcl-command mechanism (le_gui.hpp's own
    // doc comment on why some actions go this route instead of a direct
    // le_* call) - a real multi-item FIFO, not a one-shot edge like
    // gui_show_requested_ above, so this covers ordering and full
    // drainage instead of just "consumed exactly once".
    const le::SessionRequests requests = session.requests();
    EXPECT_EQ(requests.take_next_tcl_command(), std::nullopt) << "nothing queued yet";

    requests.enqueue_tcl_command("set_layer_visible {M1} true");
    requests.enqueue_tcl_command("set_hierarchy_depth 2");

    EXPECT_EQ(requests.take_next_tcl_command(), "set_layer_visible {M1} true") << "FIFO order, not LIFO";
    EXPECT_EQ(requests.take_next_tcl_command(), "set_hierarchy_depth 2");
    EXPECT_EQ(requests.take_next_tcl_command(), std::nullopt) << "queue should be empty after draining both";
}

// --- Terminal CRUD + filter-search ---

namespace
{
    le::AbstractId testcell_abstract_id(le::test::SessionHelpers &s)
    {
        return s.library_design_at(0, 0).abstract_id;
    }
}

// --- select (the script-driven counterpart to a real
// mouse click) ---

TEST_F(ApiFixture, SelectObjectRefWithAShapeRefSelectsEveryPieceOfIt)
{
    load_pin_and_obstruction_at_known_scale(*this);
    // A real click first, purely to discover PIN A's own ShapeId (no
    // other way to get one without duplicating this fixture's own LEF
    // content) - then clear the selection select is about
    // to redo, so this test actually exercises that function, not the
    // click.
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);
    const std::optional<le::ObjectId> shape_ref = selected(0);
    ASSERT_TRUE(is<le::ShapeId>(shape_ref));
    write().clear_selection();
    ASSERT_EQ(selection_count(), 0);

    EXPECT_TRUE(select(std::get<le::ShapeId>(*shape_ref)));
    EXPECT_GE(selection_count(), 1); // every rect/polygon/path of the shape, not just one
    for (int32_t i = 0; i < selection_count(); i++)
        EXPECT_EQ(selected(i), shape_ref);
}

TEST_F(ApiFixture, SelectObjectRefWithAnInvalidShapeFailsWithAMessage)
{
    EXPECT_FALSE(select(le::ShapeId{}));
}

TEST_F(ApiFixture, SelectObjectRefIsAdditiveNotReplacing)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_layout(design.id));
    const le::LayoutId layout_id = design.layout_id;
    ASSERT_NE(layout_id.index, UINT32_MAX);

    const le::RowId row_id = create_row(layout_id, "ROW_0", "CORE", std::nullopt, le::Orientation::N);
    ASSERT_NE(row_id.index, UINT32_MAX);
    const le::RegionId region_id = write().create_region({.layout = layout_id, .name = "REGION_0"}).value_or(le::RegionId{});
    ASSERT_NE(region_id.index, UINT32_MAX);

    EXPECT_TRUE(select(row_id));
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(select(region_id));
    EXPECT_EQ(selection_count(), 2); // additive - the Row is still selected too
}

TEST_F(ApiFixture, SelectObjectRefWithAPlacementSelectsIt)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_layout(design.id));
    const le::LayoutId layout_id = design.layout_id;
    ASSERT_NE(layout_id.index, UINT32_MAX);

    const le::AbstractId testcell_abstract = testcell_abstract_id(*this);
    ASSERT_NE(testcell_abstract.index, UINT32_MAX);
    const le::DesignId testcell_design = library_design_at(0, 0).id;
    const le::PlacementId placement_id = create_placement(layout_id, testcell_design, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
    ASSERT_NE(placement_id.index, UINT32_MAX);

    EXPECT_TRUE(select(placement_id));
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(is<le::PlacementId>(selected(0)));
    EXPECT_EQ(selected(0), le::ObjectId{placement_id});
}

TEST_F(ApiFixture, CreateTerminalWithUnknownAbstractIdReturnsInvalidId)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId bogus{.index = UINT32_MAX, .generation = 0};

    EXPECT_EQ(write().create_terminal({.abstract = bogus, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{}).index, UINT32_MAX);
}

TEST_F(ApiFixture, CreateTerminalSucceedsAndIsReadableViaProperties)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);

    const le::TerminalId id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(id.index, UINT32_MAX);

    const int32_t count = static_cast<int32_t>(properties(le::ObjectId{id}).size());
    ASSERT_GT(count, 0);

    bool found_name = false, found_direction = false, found_port_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(le::ObjectId{id}).at(i);
        if (std::string(property.name) == "name")
        {
            found_name = true;
            EXPECT_EQ(property.string_value, "IN0");
        }
        else if (std::string(property.name) == "direction")
        {
            found_direction = true;
            EXPECT_EQ(property.string_value, "INPUT");
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
    // scoped to Abstract) - create_terminal surfaces that as an invalid
    // id, same as any other failure. Two different Abstracts legitimately
    // reuse the same pin name (e.g. VDD/IN0 across a library's cells), so
    // that must keep succeeding.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_lef(fixture_path("othercell.lef"), "othercell"));
    const le::AbstractId testcell_id = testcell_abstract_id(*this);
    const le::AbstractId othercell_id = library_design_at(1, 0).abstract_id;
    ASSERT_NE(testcell_id.index, UINT32_MAX);
    ASSERT_NE(othercell_id.index, UINT32_MAX);

    const le::TerminalId first = write().create_terminal({.abstract = testcell_id, .name = "DUPTEST", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(first.index, UINT32_MAX);

    const le::TerminalId duplicate = write().create_terminal({.abstract = testcell_id, .name = "DUPTEST", .direction = le::SignalDirection::OUTPUT}).value_or(le::TerminalId{});
    EXPECT_EQ(duplicate.index, UINT32_MAX);

    const le::TerminalId other_abstract = write().create_terminal({.abstract = othercell_id, .name = "DUPTEST", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    EXPECT_NE(other_abstract.index, UINT32_MAX);
}

TEST_F(ApiFixture, UpdateTerminalNameToADuplicateOnTheSameAbstractFailsButRenamingToItsOwnCurrentNameSucceeds)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);

    const le::TerminalId first = write().create_terminal({.abstract = abstract_id, .name = "FIRST", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    const le::TerminalId second = write().create_terminal({.abstract = abstract_id, .name = "SECOND", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(first.index, UINT32_MAX);
    ASSERT_NE(second.index, UINT32_MAX);

    EXPECT_FALSE(write().update_terminal(second, {.name = "FIRST"}));
    EXPECT_TRUE(write().update_terminal(second, {.name = "SECOND"}));
}

TEST_F(ApiFixture, UpdateTerminalNameAndDirectionUpdateTheirProperties)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(id.index, UINT32_MAX);

    EXPECT_TRUE(write().update_terminal(id, {.name = "IN0_RENAMED", .direction = le::SignalDirection::OUTPUT}));

    const int32_t count = static_cast<int32_t>(properties(le::ObjectId{id}).size());
    bool checked_name = false, checked_direction = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(le::ObjectId{id}).at(i);
        if (std::string(property.name) == "name")
        {
            checked_name = true;
            EXPECT_EQ(property.string_value, "IN0_RENAMED");
        }
        else if (std::string(property.name) == "direction")
        {
            checked_direction = true;
            EXPECT_EQ(property.string_value, "OUTPUT");
        }
    }
    EXPECT_TRUE(checked_name);
    EXPECT_TRUE(checked_direction);
}

TEST_F(ApiFixture, DeleteTerminalRemovesItAndIsIdempotentlySafeAfterwards)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_terminal(id));
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{id}).size()), 0);
    // Already deleted, and a never-created id - neither crashes.
    EXPECT_FALSE(write().delete_terminal(id));
    EXPECT_FALSE(write().delete_terminal(le::TerminalId{}));
}

TEST_F(ApiFixture, BuildingALibraryDesignAbstractFromScratchAndMakingItCurrentFindsItsTerminalByName)
{
    // Tcl's terminal:NAME lookup (Terminal's own hand-written friendly-id
    // resolver - unique_per_parent means it can't use the generated
    // by-name lookup pair, see Field.unique_per_parent's own docstring)
    // must read handle->current_abstract_id - the field
    // set_current_abstract sets and get_terminals' own default
    // scope derives from - not the GUI-rendering current_abstract(),
    // which only moves when a Design is selected. A script building a
    // brand new Library/Design/Abstract purely through create_<type>
    // calls and selecting the Abstract directly via
    // set_current_abstract (this test's own flow, with no LEF file
    // involved) must be able to create_terminal_port -terminal $in for a
    // Terminal it just created.
    const le::LibraryId library_id = write().create_library({.name = "LIB"}).value_or(le::LibraryId{});
    ASSERT_NE(library_id.index, UINT32_MAX);
    const le::DesignId design_id = write().create_design({.library = library_id, .name = "CELL"}).value_or(le::DesignId{});
    ASSERT_NE(design_id.index, UINT32_MAX);
    // create_abstract's own -size/-origin/-bbox (create_abstract's
    // has_size/has_origin/has_bbox) are all dbu-using compound fields -
    // Klass.create_api_body() fetches dbu_per_um unconditionally
    // whenever a class has any such field, regardless of whether it was
    // actually provided (see Field.cmd_uses_dbu()'s own docstring), so a
    // Technology has to exist even though every one of them is omitted
    // (has_*=0) below - matches the original bug report's own script,
    // which also read_lef's a tech file before building anything.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = write().create_abstract({.design = design_id}).value_or(le::AbstractId{});
    ASSERT_NE(abstract_id.index, UINT32_MAX);

    ASSERT_TRUE(write().set_current_abstract(abstract_id).has_value());

    const le::TerminalId in_id = write().create_terminal({.abstract = abstract_id, .name = "IN", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    ASSERT_NE(in_id.index, UINT32_MAX);

    // Found by name in the current Abstract, as Tcl's terminal:IN resolves.
    const auto found = read().get_terminals({.name = "IN"});
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, std::vector<le::TerminalId>{in_id});

    const le::TerminalPortId port_id = write().create_terminal_port({.terminal = in_id}).value_or(le::TerminalPortId{});
    EXPECT_NE(port_id.index, UINT32_MAX);
}

TEST_F(ApiFixture, SearchTerminalFindsMatchesByFilterExpression)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId in0 = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    const le::TerminalId in1 = write().create_terminal({.abstract = abstract_id, .name = "IN1", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    write().create_terminal({.abstract = abstract_id, .name = "OUT0", .direction = le::SignalDirection::OUTPUT}).value_or(le::TerminalId{});

    const auto found = read().get_terminals({.of_abstract = abstract_id, .filter = ".name =~ IN*"});
    ASSERT_TRUE(found);
    ASSERT_EQ(found->size(), 2u);
    EXPECT_NE(std::ranges::find(*found, in0), found->end());
    EXPECT_NE(std::ranges::find(*found, in1), found->end());

    EXPECT_EQ(read().get_terminals({.of_abstract = abstract_id, .filter = ".direction == OUTPUT"})->size(), 1u);
    EXPECT_EQ(read().get_terminals({.of_abstract = abstract_id, .filter = ".name == DOES_NOT_EXIST"})->size(), 0u);
}

TEST_F(ApiFixture, SearchTerminalWithBadFilterExpressionFails)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_FALSE(read().get_terminals({.filter = "not a filter expression"}));
}

// --- TerminalPort/Obstruction CRUD + filter-search, and Abstract boundary
// update ---

namespace
{
    constexpr double kRect0[] = {0.1, 0.1, 0.3, 0.4};

    // Shape.layer is a required, resolved le::LayerId (readers error
    // rather than create a Shape with an unresolved layer - see its own
    // schema.py comment). Tests here use layer names like "M4"/"M5"/"M6"
    // that testcell.lef doesn't declare (only "M1" is), so this
    // resolves-or-creates a real Layer under the fixture's own
    // Technology to stand in for them. Idempotent (looks the name up first)
    // since several tests request the same name more than once.
    le::LayerId named_layer(le::test::SessionHelpers &s, const char *name)
    {
        const le::LayerId existing = s.layer_named(name);
        if (existing.index != UINT32_MAX)
            return existing;
        const le::TechnologyId technology = s.read().current_technology();
        return s.write().create_layer({.technology = technology, .name = name, .type = "ROUTING", .direction = le::RoutingDirection::NONE}).value_or(le::LayerId{});
    }

    // A TerminalPort with one rect Shape - the common case most tests
    // below want, without re-typing the sequence in every test.
    le::TerminalPortId create_terminal_port_with_rect(le::test::SessionHelpers &s, le::TerminalId terminal_id, const char *layer_name, const double rect_um[4])
    {
        const le::TerminalPortId port_id = s.write().create_terminal_port({.terminal = terminal_id}).value_or(le::TerminalPortId{});
        s.create_rects(le::ShapeOwner::terminal_port(port_id), named_layer(s, layer_name), std::span<const double>(rect_um, 4));
        return port_id;
    }

    le::ObstructionId create_obstruction_with_rect(le::test::SessionHelpers &s, le::AbstractId abstract_id, const char *layer_name, const double rect_um[4])
    {
        const le::ObstructionId obstruction_id = s.write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
        s.create_rects(le::ShapeOwner::obstruction(obstruction_id), named_layer(s, layer_name), std::span<const double>(rect_um, 4));
        return obstruction_id;
    }
}

TEST_F(ApiFixture, CreateTerminalPortSucceedsAndIsReadableViaProperties)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId terminal_id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});

    const le::TerminalPortId port_id = create_terminal_port_with_rect(*this, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);

    const int32_t count = static_cast<int32_t>(properties(le::ObjectId{port_id}).size());
    bool found_shapes_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(le::ObjectId{port_id}).at(i);
        if (std::string(property.name) == "shapes_count")
        {
            found_shapes_count = true;
            EXPECT_EQ(property.int_value, 1);
        }
    }
    EXPECT_TRUE(found_shapes_count);

    // The Terminal's own ports_count now reflects the new port.
    const int32_t terminal_property_count = static_cast<int32_t>(properties(le::ObjectId{terminal_id}).size());
    for (int32_t i = 0; i < terminal_property_count; ++i)
    {
        const le::PropertyValue property = properties(le::ObjectId{terminal_id}).at(i);
        if (std::string(property.name) == "ports_count")
            EXPECT_EQ(property.int_value, 1);
    }
}

TEST_F(ApiFixture, DeleteTerminalPortCascadesToItsShapesAndIsIdempotentlySafeAfterwards)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId terminal_id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    const le::TerminalPortId port_id = create_terminal_port_with_rect(*this, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);
    const le::ShapeId shape_id = child_at(&le::Root::get_terminal_port_shapes, port_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_terminal_port(port_id));
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{port_id}).size()), 0);
    // Cascade: the shape it owned is gone too, not left as unreachable garbage.
    EXPECT_EQ(read().root().get_shape(shape_id), nullptr);
    EXPECT_FALSE(write().delete_terminal_port(port_id));
}

TEST_F(ApiFixture, DeleteInstanceCascadesToItsPins)
{
    // create_instance's generated body needs a Technology present for
    // micron-to-dbu conversion of its own optional location field, even
    // though this test leaves location unset - same requirement every
    // other create_<type> with a dbu-typed compound field has.
    write().create_technology({.database_units_microns = 1000.0});
    const le::LibraryId library_id = write().create_library({.name = "LIB"}).value_or(le::LibraryId{});
    const le::DesignId design_id = write().create_design({.library = library_id, .name = "TOP"}).value_or(le::DesignId{});
    const le::SchematicId schematic_id = write().create_schematic({.design = design_id}).value_or(le::SchematicId{});
    const le::InstanceId instance_id = write().create_instance({.schematic = schematic_id, .name = "U1", .reference_name = "BUFX1"}).value_or(le::InstanceId{});
    const le::PinId pin_id = write().create_pin({.instance = instance_id, .name = "A"}).value_or(le::PinId{});
    ASSERT_NE(pin_id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_instance(instance_id));
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{instance_id}).size()), 0);
    // Cascade: the pin it owned is gone too, not left as unreachable garbage.
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{pin_id}).size()), 0);
    EXPECT_FALSE(write().delete_instance(instance_id));
}

TEST_F(ApiFixture, DeleteSchematicCascadesThroughPortsNetsInstancesAndTheirPins)
{
    // See DeleteInstanceCascadesToItsPins's own comment - create_instance
    // needs a Technology present regardless of whether location is used.
    write().create_technology({.database_units_microns = 1000.0});
    const le::LibraryId library_id = write().create_library({.name = "LIB"}).value_or(le::LibraryId{});
    const le::DesignId design_id = write().create_design({.library = library_id, .name = "TOP"}).value_or(le::DesignId{});
    const le::SchematicId schematic_id = write().create_schematic({.design = design_id}).value_or(le::SchematicId{});
    const le::NetId net_id = write().create_net({.schematic = schematic_id, .name = "clk"}).value_or(le::NetId{});
    const le::PortId port_id = write().create_port({.schematic = schematic_id, .name = "clk", .direction = le::SignalDirection::INPUT, .net = net_id}).value_or(le::PortId{});
    const le::InstanceId instance_id = write().create_instance({.schematic = schematic_id, .name = "U1", .reference_name = "BUFX1"}).value_or(le::InstanceId{});
    const le::PinId pin_id = write().create_pin({.instance = instance_id, .name = "A", .net = net_id}).value_or(le::PinId{});
    ASSERT_NE(port_id.index, UINT32_MAX);
    ASSERT_NE(net_id.index, UINT32_MAX);
    ASSERT_NE(pin_id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_schematic(schematic_id));
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{schematic_id}).size()), 0);
    // Cascade: every port/net/instance it owned, and every pin those
    // instances owned, is gone too - not left as unreachable garbage.
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{port_id}).size()), 0);
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{net_id}).size()), 0);
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{instance_id}).size()), 0);
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{pin_id}).size()), 0);
    EXPECT_FALSE(write().delete_schematic(schematic_id));
}

TEST_F(ApiFixture, SearchTerminalPortFindsMatchesUsingUpdatesMdItem15SExampleExpression)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId in0 = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    const le::TerminalId out0 = write().create_terminal({.abstract = abstract_id, .name = "OUT0", .direction = le::SignalDirection::OUTPUT}).value_or(le::TerminalId{});
    const le::TerminalPortId matching_port = create_terminal_port_with_rect(*this, in0, "M4", kRect0);
    create_terminal_port_with_rect(*this, in0, "M5", kRect0);  // wrong layer
    create_terminal_port_with_rect(*this, out0, "M4", kRect0); // wrong terminal name
    ASSERT_NE(matching_port.index, UINT32_MAX);

    write().set_current_abstract(abstract_id);
    const auto found = read().get_terminal_ports({.filter = ".terminal.name =~ IN* && .shapes.layer.name == M4"});
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, std::vector<le::TerminalPortId>{matching_port});
}

TEST_F(ApiFixture, SearchTerminalPortWithBadFilterExpressionFails)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_FALSE(read().get_terminal_ports({.filter = "not a filter expression"}));
}

TEST_F(ApiFixture, CreateObstructionSucceedsAndIsReadableViaProperties)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);

    const le::ObstructionId id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    ASSERT_NE(id.index, UINT32_MAX);

    const int32_t count = static_cast<int32_t>(properties(le::ObjectId{id}).size());
    bool found_shapes_count = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const le::PropertyValue property = properties(le::ObjectId{id}).at(i);
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
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    ASSERT_NE(id.index, UINT32_MAX);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_obstruction(id));
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{id}).size()), 0);
    EXPECT_EQ(read().root().get_shape(shape_id), nullptr);
    EXPECT_FALSE(write().delete_obstruction(id));
}

TEST_F(ApiFixture, SearchObstructionFindsMatchesByLayerName)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId matching = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    create_obstruction_with_rect(*this, abstract_id, "M5", kRect0);
    ASSERT_NE(matching.index, UINT32_MAX);

    const auto found = read().get_obstructions({.of_abstract = abstract_id, .filter = ".shapes.layer.name == M4"});
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, std::vector<le::ObstructionId>{matching});
}

// --- Shape CRUD, addressed by a stable id. Rects, polygons, and paths
// are all created/read/removed via their own symmetric set of calls -
// none baked into creation. ---

TEST_F(ApiFixture, TerminalPortShapeCountAndAtEnumerateAndReadBackWhatWasCreated)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::TerminalId terminal_id = write().create_terminal({.abstract = abstract_id, .name = "IN0", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{});
    const le::TerminalPortId port_id = create_terminal_port_with_rect(*this, terminal_id, "M4", kRect0);
    ASSERT_NE(port_id.index, UINT32_MAX);

    ASSERT_EQ(child_count(&le::Root::get_terminal_port_shapes, port_id), 1);
    const le::ShapeId shape_id = child_at(&le::Root::get_terminal_port_shapes, port_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(shape_layer_name(shape_id), "M4");
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 1);
    const le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.1);
    EXPECT_DOUBLE_EQ(rect.ll.y, 0.1);
    EXPECT_DOUBLE_EQ(rect.ur.x, 0.3);
    EXPECT_DOUBLE_EQ(rect.ur.y, 0.4);

    EXPECT_EQ(child_at(&le::Root::get_terminal_port_shapes, port_id, 1).index, UINT32_MAX);
}

TEST_F(ApiFixture, ObstructionShapeCountAndAtEnumerateAndReadBackWhatWasCreated)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M5", kRect0);
    ASSERT_NE(obstruction_id.index, UINT32_MAX);

    ASSERT_EQ(child_count(&le::Root::get_obstruction_shapes, obstruction_id), 1);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    EXPECT_EQ(shape_layer_name(shape_id), "M5");
}

TEST_F(ApiFixture, UpdateShapeLayerNameRenamesWithoutTouchingGeometryOrParent)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    const le::LayerId m6 = named_layer(*this, "M6");
    EXPECT_TRUE(write().update_shape(shape_id, {.layer = m6}));

    EXPECT_EQ(shape_layer_name(shape_id), "M6");
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 1); // untouched
    const le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.1); // untouched

    // Same shape id, same parent.
    EXPECT_EQ(child_count(&le::Root::get_obstruction_shapes, obstruction_id), 1);
    EXPECT_EQ(child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0).index, shape_id.index);
}

TEST_F(ApiFixture, CreateShapeWithRectsThenUpdateReplacesThemAndRemoveShapeRectWorks)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});

    constexpr double rects[] = {0.1, 0.1, 0.3, 0.4, 1.0, 1.0, 2.0, 2.0};
    const le::ShapeId shape_id = create_rects(le::ShapeOwner::obstruction(obstruction_id), named_layer(*this, "M4"), rects);
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 2);
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 1).ur.x, 2.0);

    // update_shape's rects replace the whole list, not append.
    const le::Rect replacement = units().to_dbu(le::RectUm{{5.0, 5.0}, {6.0, 6.0}});
    EXPECT_TRUE(write().update_shape(shape_id, {.rects = le::CompactVector<le::Rect>{replacement}}));
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 1);
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 6.0);

    EXPECT_FALSE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::RECT, 5));
    EXPECT_TRUE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::RECT, 0));
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 0);
}

TEST_F(ApiFixture, CreateShapeWithPolygonThenRemoveShapePolygonWorks)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
    const le::LayerId m4 = named_layer(*this, "M4");

    le::ShapeId shape_id;
    {
        le::WriteView view = write();
        shape_id = view.build_shape(le::ShapeOwner::obstruction(obstruction_id)).layer(m4).polygon({{0.0, 0.0}, {1.0, 0.0}, {0.5, 1.0}}).create().value_or(le::ShapeId{});
        // A polygon needs at least 3 points.
        EXPECT_FALSE(view.build_shape(le::ShapeOwner::obstruction(obstruction_id)).layer(m4).polygon({{0.0, 0.0}, {1.0, 0.0}}).create());
    }
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(static_cast<int>(shape(shape_id).polygons.size()), 1);
    ASSERT_EQ(static_cast<int>(shape(shape_id).polygons[0].points.size()), 3);
    const le::PointUm p1 = polygon_point_um(shape_id, 0, 1);
    EXPECT_DOUBLE_EQ(p1.x, 1.0);
    EXPECT_DOUBLE_EQ(p1.y, 0.0);

    EXPECT_FALSE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::POLYGON, 5));
    EXPECT_TRUE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::POLYGON, 0));
    EXPECT_EQ(static_cast<int>(shape(shape_id).polygons.size()), 0);
}

TEST_F(ApiFixture, CreateShapeWithPathThenRemoveShapePathWorks)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
    const le::LayerId m4 = named_layer(*this, "M4");

    le::ShapeId shape_id;
    {
        le::WriteView view = write();
        shape_id = view.build_shape(le::ShapeOwner::obstruction(obstruction_id)).layer(m4).path(0.1, {{0.0, 0.0}, {1.0, 0.0}}).create().value_or(le::ShapeId{});
        // A path needs at least 2 points.
        EXPECT_FALSE(view.build_shape(le::ShapeOwner::obstruction(obstruction_id)).layer(m4).path(0.1, {{0.0, 0.0}}).create());
    }
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(static_cast<int>(shape(shape_id).paths.size()), 1);
    EXPECT_DOUBLE_EQ(path_width_um(shape_id, 0), 0.1);
    ASSERT_EQ(static_cast<int>(shape(shape_id).paths[0].polygon.points.size()), 2);
    EXPECT_DOUBLE_EQ(path_point_um(shape_id, 0, 0).x, 0.0);
    EXPECT_DOUBLE_EQ(path_point_um(shape_id, 0, 1).x, 1.0);

    EXPECT_FALSE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::PATH, 5));
    EXPECT_TRUE(write().remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::PATH, 0));
    EXPECT_EQ(static_cast<int>(shape(shape_id).paths.size()), 0);
}

TEST_F(ApiFixture, RemovingAShapePieceIsUndoable)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
    const le::LayerId m4 = named_layer(*this, "M4");
    le::ShapeId shape_id;
    {
        le::WriteView view = write();
        shape_id = view.build_shape(le::ShapeOwner::obstruction(obstruction_id))
                       .layer(m4)
                       .rect(0.0, 0.0, 1.0, 1.0)
                       .rect(2.0, 2.0, 3.0, 3.0)
                       .polygon({{0.0, 0.0}, {1.0, 0.0}, {0.5, 1.0}})
                       .polygon({{5.0, 5.0}, {6.0, 5.0}, {5.5, 6.0}})
                       .path(0.1, {{0.0, 0.0}, {1.0, 0.0}})
                       .path(0.2, {{0.0, 4.0}, {1.0, 4.0}})
                       .create()
                       .value_or(le::ShapeId{});
    }
    ASSERT_NE(shape_id.index, UINT32_MAX);

    const auto count = [&](le::PieceKind kind) -> size_t
    {
        const le::ShapeData data = shape(shape_id);
        return kind == le::PieceKind::RECT ? data.rects.size() : kind == le::PieceKind::POLYGON ? data.polygons.size() : data.paths.size();
    };
    for (const le::PieceKind kind : {le::PieceKind::RECT, le::PieceKind::POLYGON, le::PieceKind::PATH})
    {
        ASSERT_TRUE(write("remove piece").remove_shape_piece(le::GeometryId::of(shape_id), kind, 0));
        EXPECT_EQ(count(kind), 1u);
        ASSERT_EQ(write().undo(), true);
        EXPECT_EQ(count(kind), 2u) << "undo puts the removed piece back";
        ASSERT_EQ(write().redo(), true);
        EXPECT_EQ(count(kind), 1u);
        ASSERT_EQ(write().undo(), true);
    }

    // Undo restores the order too, not just the count.
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 1.0);
    EXPECT_DOUBLE_EQ(polygon_point_um(shape_id, 0, 1).x, 1.0);
    EXPECT_DOUBLE_EQ(path_width_um(shape_id, 0), 0.1);
    ASSERT_TRUE(write("remove second rect").remove_shape_piece(le::GeometryId::of(shape_id), le::PieceKind::RECT, 0));
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 3.0) << "the later rect shifts down";
}

TEST_F(ApiFixture, DeleteShapeRemovesItAndParentCountDropsButParentSurvives)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_TRUE(write().delete_shape(shape_id));

    EXPECT_EQ(child_count(&le::Root::get_obstruction_shapes, obstruction_id), 0);
    EXPECT_EQ(read().root().get_shape(shape_id), nullptr);
    EXPECT_EQ(static_cast<int32_t>(properties(le::ObjectId{obstruction_id}).size()), 1); // Obstruction itself still exists (shapes_count now 0)
    EXPECT_FALSE(write().delete_shape(shape_id));
}

// --- Editing / undo-redo ---

TEST_F(ApiFixture, DeselectAllInEditModeIsANoOpEvenWithCtrlHeld)
{
    load_two_shapes_at_known_scale(*this);
    write().mouse_down(25, 175); // PIN A, in the default Select mode
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DESELECT_ALL);

    EXPECT_EQ(selection_count(), 1); // untouched
}

TEST_F(ApiFixture, UndoRedoRoundTripsAGeneratedUpdateCallThroughBeginEndCommand)
{
    // Confirms the generated recording hook actually fires end-to-end
    // through the real update_shape entry point, not just at the
    // Root layer directly (see editing_test.cpp for that).
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    EXPECT_EQ(read().can_undo(), 0);

    // The new layer is created in the same transaction: redo brings it
    // back at its id, so the update's recorded layer still names it.
    write().begin_command("update_shape test");
    const le::LayerId m6 = named_layer(*this, "M6");
    EXPECT_TRUE(write().update_shape(shape_id, {.layer = m6}));
    write().end_command(1);

    EXPECT_EQ(shape_layer_name(shape_id), "M6");
    EXPECT_NE(read().can_undo(), 0);
    EXPECT_EQ(read().can_redo(), 0);

    EXPECT_NE(write().undo(), 0);
    EXPECT_EQ(shape_layer_name(shape_id), "M4"); // restored
    EXPECT_EQ(read().can_undo(), 0);
    EXPECT_NE(read().can_redo(), 0);

    EXPECT_NE(write().redo(), 0);
    EXPECT_EQ(shape_layer_name(shape_id), "M6");
}

TEST_F(ApiFixture, CommandHistoryRecordsBothSuccessfulAndFailedCommands)
{
    // A failed command is exactly the one a
    // user most wants back, to recall and edit into a working one, so it
    // stays in the recall log same as a successful one.
    EXPECT_EQ(static_cast<int>(read().command_history_size()), 0);

    write().begin_command("ok_command");
    write().end_command(1);
    ASSERT_EQ(static_cast<int>(read().command_history_size()), 1);
    EXPECT_EQ(read().command_history_at(0), "ok_command");

    write().begin_command("bad_command");
    write().end_command(0);
    ASSERT_EQ(static_cast<int>(read().command_history_size()), 2);
    EXPECT_EQ(read().command_history_at(1), "bad_command");

}

TEST_F(ApiFixture, EndCommandWithNoBeginCommandIsANoOp)
{
    write().end_command(1);
    EXPECT_EQ(static_cast<int>(read().command_history_size()), 0);
    EXPECT_EQ(read().can_undo(), 0);
}

TEST_F(ApiFixture, MoveTranslatesSelectedShapeGeometryAndIsUndoable)
{
    // Own obstruction+rect at a known dbu location (testcell.lef is
    // DATABASE MICRONS 1000, so kRect0 (0.1,0.1)-(0.3,0.4) um is
    // (100,100)-(300,400) dbu), rather than the LEF-fixture pins
    // load_two_shapes_at_known_scale sets up, so the exact rect can be
    // read back and compared by value below.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M4", kRect0);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    // scale 0.1 (10 dbu/px), pan (0,0) - "anchor at the bottom-left image
    // corner" trick (see load_two_shapes_at_known_scale's own comment):
    // zoom's factor is multiplicative (new_scale = old_scale*(1+factor)),
    // so -0.9 takes the default scale 1.0 to 0.1; anchoring at (0,
    // viewport_height) - dbu (0,0) at the already-(0,0) pan - keeps pan
    // pinned at (0,0) across the zoom.
    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200);

    // dbu (200,250) - inside the (100,100)-(300,400) rect - is device
    // pixel (20,175) at this scale/pan (pre-flip: x_px=dbu_x*scale,
    // y_px=viewport_height-dbu_y*scale). Select it (default Select mode).
    write().mouse_down(20, 175);
    write().mouse_up(20, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    EXPECT_NE(read().move_armed(), 0);

    // First click sets the anchor at dbu (200,250) - same point as the
    // selecting click above (Move reads the *stored* mouse position, not
    // this call's own x/y - set_mouse_position must precede it, unlike
    // a plain Select-mode click - see mouse_up's own doc comment).
    write().set_mouse_position(20, 175);
    write().mouse_down(20, 175);
    write().mouse_up(20, 175);

    // Second click at dbu (250,260) - dx=50, dy=10, orthogonal delta
    // (50,0) dbu = (0.05,0) um - commits the move.
    write().set_mouse_position(25, 174);
    write().mouse_down(25, 174);
    write().mouse_up(25, 174);

    // Move stays armed after a successful commit -
    // ready for an immediate follow-up move on the same selection,
    // without re-arming, until Escape (see below).
    EXPECT_NE(read().move_armed(), 0);
    ASSERT_EQ(selection_count(), 1); // selection untouched by Move

    le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.15);
    EXPECT_DOUBLE_EQ(rect.ll.y, 0.1);
    EXPECT_DOUBLE_EQ(rect.ur.x, 0.35);
    EXPECT_DOUBLE_EQ(rect.ur.y, 0.4);

    // A follow-up move, with no re-arming call in between - proves Move
    // really does stay armed for another gesture. dbu (300,150) (device
    // (30,185)) is inside the moved rect; dbu (300,200) (device (30,180))
    // is a pure +50 dbu y-delta (dx=0, dy=50).
    write().set_mouse_position(30, 185);
    write().mouse_down(30, 185);
    write().mouse_up(30, 185);
    write().set_mouse_position(30, 180);
    write().mouse_down(30, 180);
    write().mouse_up(30, 180);

    EXPECT_NE(read().move_armed(), 0); // still armed
    rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.15);
    EXPECT_DOUBLE_EQ(rect.ll.y, 0.15);
    EXPECT_DOUBLE_EQ(rect.ur.x, 0.35);
    EXPECT_DOUBLE_EQ(rect.ur.y, 0.45);

    // Escape (le::Key::FINISH_RULER's handler) disarms it.
    write().key_down(le::Key::FINISH_RULER);
    EXPECT_EQ(read().move_armed(), 0);

    // Undo unwinds the two moves LIFO - the second move first.
    ASSERT_NE(read().can_undo(), 0);
    ASSERT_NE(write().undo(), 0);
    rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.15); // back to after the first move only
    EXPECT_DOUBLE_EQ(rect.ll.y, 0.1);

    ASSERT_NE(write().undo(), 0);
    rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.1); // back to the original position
    EXPECT_DOUBLE_EQ(rect.ur.x, 0.3);

    ASSERT_NE(write().redo(), 0);
    ASSERT_NE(write().redo(), 0);
    rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.15); // both moves re-applied
    EXPECT_DOUBLE_EQ(rect.ll.y, 0.15);
}

namespace
{
    // Placement-move fixture: TESTCELL (CLASS CORE,
    // 10x10um) placed N as U1 at (2,0) in ROW0 (N, y=0), with ROW1 (FS) at
    // y=10, both built from a 1x10um CORE site of the given SYMMETRY. U1 is
    // selected and the handle is in Edit mode, Move not armed, at 200
    // dbu/px with pan (0,0): pixel (x,y) = dbu (200x, 200(100-y)).
    le::PlacementId select_placement_between_two_rows(le::test::SessionHelpers &s, const std::string &lef_path, int32_t sym_r90, int32_t sym_x, int32_t sym_y)
    {
        EXPECT_TRUE(s.read_lef(lef_path, "test_lib"));
        const le::test::SessionTest::DesignInfo testcell_design = s.library_design_at(0, 0);
        s.create_site("CORE", le::PointUm{1.0, 10.0}, le::Symmetry{.r90 = sym_r90 != 0, .x = sym_x != 0, .y = sym_y != 0});
        const le::LibraryId top_design_library = s.write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = s.write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
        const le::LayoutId top_layout = s.write().create_layout({.design = top_design}).value_or(le::LayoutId{});
        s.create_row(top_layout, "ROW0", "CORE", le::PointUm{0.0, 0.0}, le::Orientation::N, 100);
        s.create_row(top_layout, "ROW1", "CORE", le::PointUm{0.0, 10.0}, le::Orientation::FS, 100);
        const le::PlacementId placement_id = s.create_placement(top_layout, testcell_design.id, "U1", le::PointUm{2.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);

        EXPECT_TRUE(s.open_layout(top_design));
        s.write().set_hierarchy_depth(1);
        s.write().set_viewport_size(100, 100);
        s.write().zoom(0.005 - 1.0, 0, 100);
        s.write().mouse_down(15, 85); // dbu (3000,3000) - inside U1
        s.write().mouse_up(15, 85);
        s.write().set_mode(le::InteractionMode::EDIT);
        return placement_id;
    }

    std::string placement_property(le::test::SessionHelpers &s, le::PlacementId id, const char *path)
    {
        const auto p = s.read().property(le::ObjectId{id}, path);
        return p && *p ? (*p)->string_value : std::string("<null>");
    }

    void click_at(le::test::SessionHelpers &s, int32_t x, int32_t y)
    {
        s.write().set_mouse_position(x, y);
        s.write().mouse_down(x, y);
        s.write().mouse_up(x, y);
    }

    constexpr int32_t kRotateBit = 1 << static_cast<int>(le::OrientationOp::ROTATE_CCW);
    constexpr int32_t kFlipHBit = 1 << static_cast<int>(le::OrientationOp::FLIP_HORIZONTAL);
    constexpr int32_t kFlipVBit = 1 << static_cast<int>(le::OrientationOp::FLIP_VERTICAL);
}

// A permitted flip commits straight away - no Move involved - in place,
// as one undoable edit.
TEST_F(ApiFixture, PlacementFlipCommitsImmediatelyAndIsUndoable)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    ASSERT_EQ(selection_count(), 1);
    ASSERT_TRUE(is<le::PlacementId>(selected(0)));
    EXPECT_EQ(read().move_armed(), 0);

    ASSERT_TRUE(write().apply_placement_orientation_op(le::OrientationOp::FLIP_HORIZONTAL));
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FN");
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}"); // a flip keeps the bbox

    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "N");
}

// Under site snapping, only the ops the row's site SYMMETRY permits are
// enabled (Y only here: horizontal flip); any other snap mode enables all
// three. A disabled op is refused and changes nothing.
TEST_F(ApiFixture, PlacementRotateAndFlipAreGatedBySiteSymmetryOnlyUnderSiteSnapping)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    ASSERT_EQ(read().placement_snap_mode(), le::PlacementSnapMode::SITE);
    EXPECT_EQ(read().placement_orientation_ops_enabled(), kFlipHBit);

    EXPECT_EQ(write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW).error_or(le::OrientationOpError{}), le::OrientationOpError::NOT_AVAILABLE);
    EXPECT_EQ(write().apply_placement_orientation_op(le::OrientationOp::FLIP_VERTICAL).error_or(le::OrientationOpError{}), le::OrientationOpError::NOT_AVAILABLE);
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "N");
    EXPECT_EQ(read().can_undo(), 0);

    write().set_placement_snap_mode(le::PlacementSnapMode::NONE);
    EXPECT_EQ(read().placement_orientation_ops_enabled(), kRotateBit | kFlipHBit | kFlipVBit);
    ASSERT_TRUE(write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW));
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "W");
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}"); // square cell - rotating about its center keeps the bbox
}

TEST_F(ApiFixture, PlacementSiteSymmetryXYR90EnablesEveryOpUnderSiteSnapping)
{
    select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 1, 1, 1);
    EXPECT_EQ(read().placement_orientation_ops_enabled(), kRotateBit | kFlipHBit | kFlipVBit);
}

// Arming Move alone leaves rotate/flip enabled (and committing
// immediately); only once the move's first click has anchored it - the
// ghost showing - are they disabled, until that move completes or is
// cancelled.
TEST_F(ApiFixture, PlacementRotateAndFlipAreDisabledOnlyWhileAMoveIsUnderWay)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 1, 1, 1);
    write().set_placement_snap_mode(le::PlacementSnapMode::NONE);
    write().arm_move();
    ASSERT_NE(read().move_armed(), 0);
    EXPECT_EQ(read().move_anchored(), 0);

    EXPECT_EQ(read().placement_orientation_ops_enabled(), kRotateBit | kFlipHBit | kFlipVBit);
    ASSERT_TRUE(write().apply_placement_orientation_op(le::OrientationOp::FLIP_HORIZONTAL));
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FN");

    click_at(*this, 15, 85); // first click - anchors the move
    ASSERT_NE(read().move_anchored(), 0);
    EXPECT_EQ(read().placement_orientation_ops_enabled(), 0);
    EXPECT_EQ(write().apply_placement_orientation_op(le::OrientationOp::FLIP_HORIZONTAL).error_or(le::OrientationOpError{}), le::OrientationOpError::NOT_AVAILABLE);
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FN");

    click_at(*this, 20, 85); // second click - commits dbu (+1000, 0), keeping the flip
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{3 0}");
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FN");
    EXPECT_NE(read().move_armed(), 0); // still armed for a follow-up move...
    EXPECT_EQ(read().placement_orientation_ops_enabled(), kRotateBit | kFlipHBit | kFlipVBit); // ...but no longer under way

    click_at(*this, 20, 85); // anchor a second move, then cancel it
    EXPECT_EQ(read().placement_orientation_ops_enabled(), 0);
    write().cancel_move();
    EXPECT_EQ(read().placement_orientation_ops_enabled(), kRotateBit | kFlipHBit | kFlipVBit);
}

// A site-snapped Move into the FS row takes that row's orientation (N is
// outside the FS row's family - the site is only SYMMETRY Y), as one undo
// step with the location.
TEST_F(ApiFixture, PlacementMoveSnapsToTheRowSiteGridAndOrientationAndIsUndoable)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    EXPECT_NE(read().placement_snap_mode_available(le::PlacementSnapMode::SITE), 0);
    EXPECT_EQ(read().placement_snap_mode_available(le::PlacementSnapMode::FIN_GRID), 0);

    write().arm_move();
    click_at(*this, 15, 85); // anchor at dbu (3000,3000)
    click_at(*this, 15, 36); // dbu (3000,12800) - delta (0,9800), snapped onto ROW1's y=10000

    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 10}");
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FS");

    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}");
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "N");
}

namespace
{
    void set_status(le::test::SessionHelpers &s, le::PlacementId id, const char *status)
    {
        ASSERT_TRUE(s.write().update_placement(id, {.placement_status = le::placement_status_from_string(status)}));
    }
}

TEST_F(ApiFixture, MovingAnUnplacedPlacementPlacesItUndoablyAndShowsPlacedPlacements)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    set_status(*this, placement_id, "UNPLACED");
    write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "PLACED", 0);

    write().arm_move();
    click_at(*this, 15, 85);
    click_at(*this, 15, 36);
    EXPECT_EQ(placement_property(*this, placement_id, ".placement_status"), "PLACED");
    EXPECT_EQ(read().object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "PLACED"), 1);

    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(placement_property(*this, placement_id, ".placement_status"), "UNPLACED");
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}");
}

TEST_F(ApiFixture, FixedAndCoverPlacementsCannotBeMoved)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    EXPECT_EQ(static_cast<int>(read().selected_locked_placement_count()), 0);
    for (const char *status : {"FIXED", "COVER"})
    {
        set_status(*this, placement_id, status);
        EXPECT_EQ(static_cast<int>(read().selected_locked_placement_count()), 1) << status;
        write().arm_move();
        EXPECT_EQ(read().move_armed(), 0) << status;
        click_at(*this, 15, 85);
        click_at(*this, 15, 36);
        EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}") << status;
    }

    set_status(*this, placement_id, "PLACED");
    write().arm_move();
    EXPECT_NE(read().move_armed(), 0);
}

TEST_F(ApiFixture, FixedAndCoverPlacementsCannotBeRotatedOrFlipped)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 1, 1, 1);
    ASSERT_NE(read().placement_orientation_ops_enabled(), 0);
    for (const char *status : {"FIXED", "COVER"})
    {
        set_status(*this, placement_id, status);
        EXPECT_EQ(read().placement_orientation_ops_enabled(), 0) << status;
        EXPECT_EQ(write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW).error_or(le::OrientationOpError{}), le::OrientationOpError::LOCKED) << status;
        EXPECT_EQ(write().apply_placement_orientation_op(le::OrientationOp::FLIP_HORIZONTAL).error_or(le::OrientationOpError{}), le::OrientationOpError::LOCKED) << status;
        EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "N") << status;
    }
}

TEST_F(ApiFixture, ALockedPlacementsLocationAndOrientationCannotBeUpdated)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, 1);
    set_status(*this, placement_id, "FIXED");
    auto update = [&](const char *status, int32_t has_location, double x, const char *orientation) -> bool
    {
        le::PlacementChanges changes;
        if (status)
            changes.placement_status = le::placement_status_from_string(status);
        if (has_location)
            changes.location = units().to_dbu(le::PointUm{x, 0.0});
        if (orientation)
            changes.orientation = le::orientation_from_string(orientation);
        return write().update_placement(placement_id, changes).has_value();
    };
    EXPECT_FALSE(update(nullptr, 1, 5.0, nullptr));
    EXPECT_FALSE(update(nullptr, 0, 0.0, "FS"));
    EXPECT_FALSE(update("COVER", 1, 5.0, nullptr)) << "still locked after the update";
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}");
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "N");

    EXPECT_TRUE(update(nullptr, 1, 2.0, "N")) << "unchanged values are fine";
    EXPECT_TRUE(update("PLACED", 1, 5.0, nullptr)) << "unlocking in the same update";
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{5 0}");

    set_status(*this, placement_id, "UNPLACED");
    EXPECT_TRUE(update("FIXED", 1, 2.0, "FS")) << "placing and locking in the same update";
    EXPECT_EQ(placement_property(*this, placement_id, ".placement_status"), "FIXED");
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}");
    EXPECT_EQ(placement_property(*this, placement_id, ".orientation"), "FS");
}

TEST_F(ApiFixture, APlacementLockedDuringAMoveIsLeftWhereItIs)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, 1);
    write().arm_move();
    click_at(*this, 15, 85);
    ASSERT_NE(read().move_anchored(), 0);
    set_status(*this, placement_id, "FIXED");
    click_at(*this, 15, 36);
    EXPECT_EQ(placement_property(*this, placement_id, ".location"), "{2 0}");
}

TEST_F(ApiFixture, SelectedPlacementsAreAllFixedOnlyWhenEveryOneIs)
{
    const le::PlacementId placement_id = select_placement_between_two_rows(*this, fixture_path("testcell.lef"), 0, 0, /*sym_y=*/1);
    EXPECT_EQ(read().selected_placements_all_fixed(), 0); // PLACED
    set_status(*this, placement_id, "FIXED");
    EXPECT_EQ(read().selected_placements_all_fixed(), 1);
    write().clear_selection();
    EXPECT_EQ(read().selected_placements_all_fixed(), 0); // nothing selected
}

TEST_F(ApiFixture, DragSelectSkipsAHiddenLayerCreatedAfterTheLefRead)
{
    // "M4" isn't in testcell.lef - named_layer creates it after the read,
    // so the hit-test must refresh the view layers to see it's hidden.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    create_obstruction_with_rect(*this, testcell_abstract_id(*this), "M4", kRect0);
    write().set_viewport_size(200, 200);
    write().fit_view(10);
    const auto drag_select_everything = [&]
    {
        write().mouse_down(0, 200);
        write().mouse_up(200, 0);
    };

    write().set_layer_visible("M4", false);
    drag_select_everything();
    const int32_t with_m4_hidden = selection_count();

    write().clear_selection();
    write().set_layer_visible("M4", true);
    drag_select_everything();
    EXPECT_EQ(selection_count(), with_m4_hidden + 1);
}

TEST_F(ApiFixture, ArmedMoveRendersADashedTranslucentGhostAtTheOffsetPositionBeforeCommitting)
{
    // Same recipe as MoveTranslatesSelectedShapeGeometryAndIsUndoable
    // above, on the "M1" layer testcell.lef declares.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ObstructionId obstruction_id = create_obstruction_with_rect(*this, abstract_id, "M1", kRect0);
    const le::ShapeId shape_id = child_at(&le::Root::get_obstruction_shapes, obstruction_id, 0);
    ASSERT_NE(shape_id.index, UINT32_MAX);

    // scale 0.1 (10 dbu/px), pan (0,0) - same as
    // MoveTranslatesSelectedShapeGeometryAndIsUndoable's own recipe.
    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200);

    // dbu (200,250), inside kRect0 (100,100)-(300,400) - select, arm, and
    // set the anchor there (same point three times, matching Move's own
    // "anchor = the position at arm time" convention already verified
    // above).
    write().mouse_down(20, 175);
    write().mouse_up(20, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    ASSERT_NE(read().move_armed(), 0);

    write().set_mouse_position(20, 175);
    write().mouse_down(20, 175);
    write().mouse_up(20, 175);

    // Move the mouse to dbu (300,150) [device (30,185)] *without*
    // clicking - updates the live delta but doesn't commit. Orthogonal
    // constraint picks the larger-magnitude axis: dx=+100, dy=-100 from
    // the (200,250) anchor, a tie broken toward dx (LeHandle::move_delta's
    // own >= comparison) - so the ghost should show at offset (+100, 0)
    // dbu, i.e. kRect0 shifted to (200,100)-(400,400).
    write().set_mouse_position(30, 185);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // The shifted rect's own right edge (dbu x=400, well outside kRect0's
    // own original 100-300 range, so this can't be catching the original
    // rect's own selection outline instead) at dbu y=250 - device
    // (400*0.1, 200-250*0.1) = (40, 175).
    EXPECT_TRUE(region_has_move_ghost_pixel(buffer, 36, 171, 44, 179));

    // Not committed yet - the real geometry hasn't moved.
    const le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 0.1);
    EXPECT_DOUBLE_EQ(rect.ur.x, 0.3);
}

TEST_F(ApiFixture, CommittedRulerSegmentRendersAsAnOpaqueOrangeLine)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    // scale 0.1 (10 dbu/px), pan (0,0) - same recipe as the Move ghost
    // test above.
    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200);

    write().set_mode(le::InteractionMode::RULER);

    // dbu (200,1000) [device (20,100)] to dbu (1200,1000) [device
    // (120,100)] - a horizontal 1000-dbu (1 um, DATABASE MICRONS 1000)
    // segment, both endpoints already multiples of the default 5-dbu
    // minor grid spacing so snapping doesn't move them.
    write().set_mouse_position(20, 100);
    write().mouse_down(20, 100);
    write().mouse_up(20, 100);
    ASSERT_EQ(static_cast<int>(read().rulers().size()), 1);

    write().set_mouse_position(120, 100);
    write().mouse_down(120, 100);
    write().mouse_up(120, 100);
    ASSERT_EQ(ruler_point_count(0), 2);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // The segment's own midpoint, device (70,100) - well clear of either
    // endpoint's own point marker or tick/label chrome, so this can only
    // be catching the line itself.
    EXPECT_TRUE(region_has_ruler_pixel(buffer, 66, 96, 74, 104));
}

TEST_F(ApiFixture, LiveRulerGhostSegmentRendersAsATranslucentOrangeLineBeforeTheNextPointCommits)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200);

    write().set_mode(le::InteractionMode::RULER);

    write().set_mouse_position(20, 100); // dbu (200,1000)
    write().mouse_down(20, 100);
    write().mouse_up(20, 100);
    ASSERT_EQ(static_cast<int>(read().rulers().size()), 1);

    // Move the mouse toward dbu (1200,1000) [device (120,100)] *without*
    // clicking - updates the live ghost segment but commits nothing.
    write().set_mouse_position(120, 100);
    ASSERT_EQ(ruler_point_count(0), 1); // still just the one committed point

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_ruler_ghost_pixel(buffer, 66, 96, 74, 104));
}

TEST_F(ApiFixture, RenderPixelBufferShowsMajorGridDotsAndTheAbstractOriginMarker)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);

    // Zoom scale 1.0 -> 2.0, anchored at the bottom-left image corner
    // (same trick RenderPixelBufferShowsMinorGridDotsOnceZoomedInEnough
    // uses) so dbu (0,0) stays at device (0,200). At this scale BOTH
    // tiers clear kMinGridDotPixelSpacing (8): minor*scale=5*2=10,
    // major*scale=50*2=100 - so the major-spacing dots really do render
    // in kMajorGridColor (draw_grid_blend2d falls back to
    // kMinorGridColor for "major" dots only when the minor tier itself
    // isn't visible, which isn't the case here).
    write().zoom(1.0, 0, 200);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // dbu (50,50), a major-spacing lattice point, off both axes and
    // well clear of PIN A's own geometry (testcell.lef's PIN A rect is
    // at (2000,2000)-(8000,8000) dbu) - device (100, 200-100) = (100,100),
    // exactly on a pixel corner shared by (99,99)/(100,99)/(99,100)/
    // (100,100) - sampling all four is what actually catches the dot's
    // own (necessarily partial-coverage) ink.
    EXPECT_TRUE(region_has_major_grid_pixel(buffer, 99, 99, 100, 100));

    // testcell.lef declares no LEF ORIGIN statement, so the marker sits
    // at dbu (0,0) - device (0,200), the viewport's own bottom-left
    // corner. Its own fixed 16px size (kOriginMarkerSizePx) means the
    // vertical stroke (dbu x=0, y in [-16,+16]) extends up to device
    // y=200-(-16)=216, well past this 200px-tall buffer - sampling the
    // in-bounds tail of it (y:185-199, near x=0) still reliably catches
    // real marker ink without needing the out-of-bounds portion.
    EXPECT_TRUE(region_has_origin_marker_pixel(buffer, 0, 185, 3, 199));
}

TEST_F(ApiFixture, RenderPixelBufferShowsMinorGridDotsOnceZoomedInEnough)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));

    write().set_viewport_size(200, 200);
    // scale 1.0 -> 2.0 ("anchor at the bottom-left image corner" trick,
    // load_two_shapes_at_known_scale's own comment) - minor*scale=5*2=10
    // >= 8, now visible.
    write().zoom(1.0, 0, 200);

    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);

    // dbu (5,5) - a minor-only lattice point (not a multiple of the
    // major spacing, 50), off both axes, clear of PIN A's own geometry -
    // device (5*2, 200-5*2) = (10,190).
    EXPECT_TRUE(region_has_minor_grid_pixel(buffer, 8, 188, 12, 192));
}

TEST_F(ApiFixture, ArmMoveWithEmptySelectionOrOutsideEditModeIsANoOp)
{
    load_two_shapes_at_known_scale(*this);

    // Select mode, nothing selected.
    write().arm_move();
    EXPECT_EQ(read().move_armed(), 0);

    // Selected, but still Select mode (not Edit).
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    ASSERT_EQ(selection_count(), 1);
    write().arm_move();
    EXPECT_EQ(read().move_armed(), 0);

    // Edit mode, but nothing selected.
    write().key_down(le::Key::DESELECT_ALL); // not Ctrl-gated via direct call below
    write().clear_selection();
    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    EXPECT_EQ(read().move_armed(), 0);
}

TEST_F(ApiFixture, CancelMoveViaEscapeClearsArmedState)
{
    load_two_shapes_at_known_scale(*this);
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    ASSERT_NE(read().move_armed(), 0);

    write().key_down(le::Key::FINISH_RULER); // Escape - also cancels an in-progress move
    EXPECT_EQ(read().move_armed(), 0);
}

TEST_F(ApiFixture, KeyDownMoveWithoutCtrlOrWithShiftAlsoHeldIsANoOp)
{
    load_two_shapes_at_known_scale(*this);
    write().mouse_down(25, 175);
    write().mouse_up(25, 175);
    write().set_mode(le::InteractionMode::EDIT);
    ASSERT_EQ(selection_count(), 1);

    write().key_down(le::Key::MOVE); // Ctrl never held
    EXPECT_EQ(read().move_armed(), 0);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::SHIFT);
    write().key_down(le::Key::MOVE); // Ctrl+Shift both held
    EXPECT_EQ(read().move_armed(), 0);

    write().key_up(le::Key::SHIFT);
    write().key_down(le::Key::MOVE); // Ctrl alone - actually arms
    EXPECT_NE(read().move_armed(), 0);
}

TEST_F(ApiFixture, ClickSelectsAndMovesOnlyOneRectOfATwoRectShapeNotBothOrTheWrongOne)
{
    // Selection/Move are piece-granular - clicking one rect of a Shape
    // that bundles 2+ rects together
    // (e.g. several RECT statements under one LEF OBS LAYER line) must
    // select and move only that raw piece, at its real stored index, not
    // the whole Shape or the wrong sibling.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);

    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
    const double rects_um[8] = {0.1, 0.1, 0.3, 0.4, 0.5, 0.1, 0.7, 0.4};
    const le::ShapeId shape_id = create_rects(le::ShapeOwner::obstruction(obstruction_id), named_layer(*this, "M4"), std::span<const double>(rects_um, 8));
    ASSERT_NE(shape_id.index, UINT32_MAX);
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 2);

    // scale 0.1 (10 dbu/px), pan (0,0) - same recipe as
    // MoveTranslatesSelectedShapeGeometryAndIsUndoable above.
    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200);

    // Click rect index 1's center - dbu (600, 250), device (60, 175)
    // (pre-flip: x_px=dbu_x*scale, y_px=viewport_height-dbu_y*scale).
    // Rect index 0 occupies device x 10-30; rect index 1 occupies device
    // x 50-70 - unambiguous which one this hits.
    write().mouse_down(60, 175);
    write().mouse_up(60, 175);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();

    // Anchor at dbu (600,250), commit at dbu (650,250) - dx=50, dy=0.
    write().set_mouse_position(60, 175);
    write().mouse_down(60, 175);
    write().mouse_up(60, 175);
    write().set_mouse_position(65, 175);
    write().mouse_down(65, 175);
    write().mouse_up(65, 175);

    // Only rect index 1 (the clicked piece) moved - rect index 0 is
    // untouched, and both remain real rects at their original indices.
    ASSERT_EQ(static_cast<int>(shape(shape_id).rects.size()), 2);
    const le::RectUm rect0 = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect0.ll.x, 0.1); // untouched sibling rect
    EXPECT_DOUBLE_EQ(rect0.ur.x, 0.3);
    const le::RectUm rect1 = rect_um(shape_id, 1);
    EXPECT_DOUBLE_EQ(rect1.ll.x, 0.55); // moved by +0.05 um (50 dbu)
    EXPECT_DOUBLE_EQ(rect1.ur.x, 0.75);
    EXPECT_EQ(static_cast<int>(shape(shape_id).polygons.size()), 0);
}

TEST_F(ApiFixture, DragSelectEnclosingTwoPiecesOfTheSameShapeSelectsBothAsSeparateEntries)
{
    // Selection is piece-granular - a drag enclosing
    // two rects that happen to belong to the same Shape must select both
    // as independent entries, not dedup down to one whole-shape entry.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);

    const le::ObstructionId obstruction_id = write().create_obstruction({.abstract = abstract_id}).value_or(le::ObstructionId{});
    const double rects_um[8] = {0.1, 0.1, 0.3, 0.4, 0.5, 0.1, 0.7, 0.4};
    const le::ShapeId shape_id = create_rects(le::ShapeOwner::obstruction(obstruction_id), named_layer(*this, "M4"), std::span<const double>(rects_um, 8));
    ASSERT_NE(shape_id.index, UINT32_MAX);

    write().set_viewport_size(200, 200);
    write().zoom(-0.9, 0, 200); // scale 0.1, pan (0,0)

    // Drag a rectangle enclosing both rects: device (0,200)-(80,0) - dbu
    // (0,0)-(800,2000), comfortably containing both (100,100)-(300,400)
    // and (500,100)-(700,400).
    write().mouse_down(0, 200);
    write().mouse_up(80, 0);

    EXPECT_EQ(selection_count(), 2);
}

// --- write_lef/write_def ---

namespace
{
    bool file_is_nonempty(const std::string &path)
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0;
    }
}

TEST_F(ApiFixture, WriteLefWithAnExplicitAbstractSucceedsAndProducesARealFile)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    ASSERT_NE(abstract_id.index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_explicit.lef");
    EXPECT_TRUE(write().write_lef(out_path, std::span<const le::AbstractId>(&abstract_id, 1), le::LibraryId{}, le::LefLayerWriteMode::NONE));
    EXPECT_TRUE(file_is_nonempty(out_path));
}

TEST_F(ApiFixture, WriteLefWithNoAbstractOrLibraryGivenAndNoCurrentAbstractSetFailsWithAMessage)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(read().current_abstract().index, UINT32_MAX); // nothing selected yet

    EXPECT_FALSE(write().write_lef(le::test::scratch_path("le_write_lef_no_current.lef"), {}, le::LibraryId{}, le::LefLayerWriteMode::NONE));
}

TEST_F(ApiFixture, WriteLefFallsBackToTheCurrentAbstractWhenNoneIsGiven)
{
    // With -abstract omitted, write_lef uses current_abstract.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_abstract(design.id));
    ASSERT_NE(read().current_abstract().index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_current_fallback.lef");
    EXPECT_TRUE(write().write_lef(out_path, {}, le::LibraryId{}, le::LefLayerWriteMode::NONE));
    EXPECT_TRUE(file_is_nonempty(out_path));
}

TEST_F(ApiFixture, WriteLefTechnologyOnlyModeSucceedsWithNoAbstractAndNoCurrentAbstractAtAll)
{
    // le::LefLayerWriteMode::TECHNOLOGY_ONLY ignores abstract_id/current
    // Abstract entirely (LEFWriter::LayerWriteMode::TechnologyOnly's own
    // doc comment) - the one mode that doesn't need either set.
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(read().current_abstract().index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_tech_only.lef");
    EXPECT_TRUE(write().write_lef(out_path, {}, le::LibraryId{}, le::LefLayerWriteMode::TECHNOLOGY_ONLY));
    EXPECT_TRUE(file_is_nonempty(out_path));
}

// --- -library/-abstracts (multiple MACROs in one file) ---

TEST_F(ApiFixture, WriteLefWithALibraryWritesAMacroForEveryAbstractInEveryDesign)
{
    // testcell.lef/othercell.lef each read into their *own* Library
    // (read_lef's own library name is the LEF file's stem - see
    // api.cpp) - not useful here, this test needs two Designs sharing one
    // real Library, so it builds both from scratch instead (still needs
    // testcell.lef read first, purely to establish a real Technology -
    // create_abstract fails with no Technology present).
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));

    const le::LibraryId library_id = write().create_library({.name = "MULTI_LIB"}).value_or(le::LibraryId{});
    ASSERT_NE(library_id.index, UINT32_MAX);
    const le::DesignId design1_id = write().create_design({.library = library_id, .name = "DESIGN_ONE"}).value_or(le::DesignId{});
    ASSERT_NE(design1_id.index, UINT32_MAX);
    const le::AbstractId abstract1_id = write().create_abstract({.design = design1_id}).value_or(le::AbstractId{});
    ASSERT_NE(abstract1_id.index, UINT32_MAX);
    const le::DesignId design2_id = write().create_design({.library = library_id, .name = "DESIGN_TWO"}).value_or(le::DesignId{});
    ASSERT_NE(design2_id.index, UINT32_MAX);
    const le::AbstractId abstract2_id = write().create_abstract({.design = design2_id}).value_or(le::AbstractId{});
    ASSERT_NE(abstract2_id.index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_library.lef");
    ASSERT_TRUE(write().write_lef(out_path, {}, library_id, le::LefLayerWriteMode::INCLUDE_WITH_ABSTRACT));
    ASSERT_TRUE(file_is_nonempty(out_path));

    // Both DESIGN_ONE and DESIGN_TWO's own MACRO should be present - a
    // real read-back is a stronger check than counting "MACRO" substrings
    // (also survives the file happening to have neither string spelled
    // that way for some unrelated reason).
    le::Session reread;
    ASSERT_TRUE(reread.write().read_lef(out_path, "test_lib"));
    EXPECT_EQ(reread.read().root().get_design_ids().size(), 2u);
}

TEST_F(ApiFixture, WriteLefWithAnExplicitAbstractListWritesOnlyThoseAbstracts)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_lef(fixture_path("othercell.lef"), "othercell"));
    const le::AbstractId testcell_id = testcell_abstract_id(*this);
    ASSERT_NE(testcell_id.index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_explicit_list.lef");
    ASSERT_TRUE(write().write_lef(out_path, std::span<const le::AbstractId>(&testcell_id, 1), le::LibraryId{}, le::LefLayerWriteMode::INCLUDE_WITH_ABSTRACT));
    ASSERT_TRUE(file_is_nonempty(out_path));

    le::Session reread;
    ASSERT_TRUE(reread.write().read_lef(out_path, "test_lib"));
    EXPECT_EQ(reread.read().root().get_design_ids().size(), 1u); // only TESTCELL, not OTHERCELL
}

TEST_F(ApiFixture, WriteLefWithALibraryThatHasNoAbstractsWritesNoMacrosAndIsNotAnError)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId library_id = library_at(0).id;
    ASSERT_NE(library_id.index, UINT32_MAX);
    // A hand-built Design with no Abstract at all yet - not an error.
    const le::DesignId empty_design_id = write().create_design({.library = library_id, .name = "EMPTY_DESIGN"}).value_or(le::DesignId{});
    ASSERT_NE(empty_design_id.index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_lef_library_partial.lef");
    ASSERT_TRUE(write().write_lef(out_path, {}, library_id, le::LefLayerWriteMode::INCLUDE_WITH_ABSTRACT));

    le::Session reread;
    ASSERT_TRUE(reread.write().read_lef(out_path, "test_lib"));
    EXPECT_EQ(reread.read().root().get_design_ids().size(), 1u); // only TESTCELL - EMPTY_DESIGN has no Abstract to write a MACRO from
}

TEST_F(ApiFixture, WriteDefWithAnExplicitLayoutSucceedsAndProducesARealFile)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));
    const le::LayoutId layout_id = library_design_at(0, 0).layout_id;
    ASSERT_NE(layout_id.index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_def_explicit.def");
    EXPECT_TRUE(write().write_def(out_path.c_str(), layout_id).has_value());
    EXPECT_TRUE(file_is_nonempty(out_path));
}

TEST_F(ApiFixture, WriteDefWithNoLayoutGivenAndNoCurrentLayoutSetFailsWithAMessage)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));
    EXPECT_EQ(read().current_layout().index, UINT32_MAX); // nothing selected yet

    EXPECT_FALSE(write().write_def(le::test::scratch_path("le_write_def_no_current.def").c_str(), le::LayoutId{}).has_value());
}

TEST_F(ApiFixture, WriteDefFallsBackToTheCurrentLayoutWhenNoneIsGiven)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "testcell"));
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    ASSERT_TRUE(open_layout(design.id));
    ASSERT_NE(read().current_layout().index, UINT32_MAX);

    const std::string out_path = le::test::scratch_path("le_write_def_current_fallback.def");
    EXPECT_TRUE(write().write_def(out_path.c_str(), le::LayoutId{}).has_value());
    EXPECT_TRUE(file_is_nonempty(out_path));
}

// A free-standing shape (Abstract.free_shapes) renders in its layer's own
// color on the CUSTOM_SHAPE column, and hiding that purpose hides it -
// rasterized end to end through Renderer::render.
TEST_F(ApiFixture, FreeShapeOnALayerRendersAndHidesWithTheCustomShapePurpose)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);

    auto region_has_colored_pixel = [](const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[3] > 0 && p[0] != p[1])
                    return true;
            }
        return false;
    };

    // The macro's own empty corner (FitRectUsesTheGivenRectNotTheDesignsOwnBbox
    // proves it has no colored pixel) - nothing there until the free shape.
    fit_rect(0.1, 0.1, 1.1, 1.1, 0);
    ASSERT_FALSE(region_has_colored_pixel(render(), 50, 50, 150, 150));

    const le::AbstractId abstract_id = read().current_abstract();
    const le::LayerId m1 = layer_named("M1");
    const double rect_um[] = {0.2, 0.2, 1.0, 1.0};
    const le::ShapeId free_shape = create_rects(le::ShapeOwner::in_abstract(abstract_id), m1, std::span<const double>(rect_um, 4));
    ASSERT_NE(free_shape.index, UINT32_MAX);

    EXPECT_TRUE(region_has_colored_pixel(render(), 50, 50, 150, 150));

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(12 /* CUSTOM_SHAPE */), 0);
    EXPECT_FALSE(region_has_colored_pixel(render(), 50, 50, 150, 150));
}

// --- Custom library naming ---

TEST_F(ApiFixture, ReadRequiresALibraryName)
{
    EXPECT_FALSE(read_lef(fixture_path("testcell.lef"), ""));
    EXPECT_FALSE(read_def(fixture_path("testcell.def"), ""));
    const std::string paths[] = {fixture_path("testcell.def")};
    EXPECT_FALSE(write().read_verilog(paths, false, ""));
    EXPECT_EQ(library_count(), 0);
}

TEST_F(ApiFixture, ReadLefCreatesTheNamedLibraryOnceThenReusesIt)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    ASSERT_EQ(library_count(), 1);
    EXPECT_EQ(library_at(0).name, "cells");

    ASSERT_TRUE(read_lef(fixture_path("othercell.lef"), "cells"));
    ASSERT_EQ(library_count(), 1); // reused, not a second "cells"
    EXPECT_EQ(library_design_count(0), 2);
}

TEST_F(ApiFixture, ReadingAViewTheDesignAlreadyHasIsSkippedForLefAndAnErrorForDef)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    // A repeated MACRO is skipped with a warning, like a repeated LAYER.
    EXPECT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    EXPECT_EQ(library_design_count(0), 1);

    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "cells"));
    EXPECT_FALSE(read_def(fixture_path("testcell.def"), "cells"));
}

// Abstract (LEF), Layout (DEF) and Schematic (Verilog) all land on the one
// TESTCELL design; a second Schematic read into the same library is refused
// before it creates anything, while one into another library makes that
// library its own TESTCELL.
TEST_F(ApiFixture, AbstractLayoutAndSchematicViewsCombineOnOneDesign)
{
    const std::filesystem::path verilog_path = le::test::scratch_path("le_library_naming_testcell.v");
    {
        std::ofstream out(verilog_path);
        out << "module TESTCELL(input A);\nendmodule\n";
    }
    const std::string paths[] = {verilog_path.string()};

    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    ASSERT_TRUE(read_def(fixture_path("testcell.def"), "cells"));
    ASSERT_TRUE(write().read_verilog(paths, false, "cells"));

    ASSERT_EQ(library_count(), 1);
    ASSERT_EQ(library_design_count(0), 1);
    const le::test::SessionTest::DesignInfo design = library_design_at(0, 0);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX);
    EXPECT_NE(design.layout_id.index, UINT32_MAX);

    EXPECT_FALSE(write().read_verilog(paths, false, "cells"));
    EXPECT_EQ(library_design_count(0), 1);

    ASSERT_TRUE(write().read_verilog(paths, false, "other"));
    ASSERT_EQ(library_count(), 2);
    EXPECT_EQ(library_design_count(0), 1);
    EXPECT_EQ(library_design_count(1), 1);
    const le::DesignId other_design = library_design_at(1, 0).id;
    const le::ReadView view = read();
    EXPECT_FALSE(le::resolve_design_reference(view.root(), "TESTCELL").valid()); // ambiguous now
    EXPECT_EQ(le::resolve_design_reference(view.root(), "cells/TESTCELL"), design.id);
    EXPECT_EQ(le::resolve_design_reference(view.root(), "other/TESTCELL"), other_design);

    std::filesystem::remove(verilog_path);
}

// A new design goes into the named library even when other libraries exist.
TEST_F(ApiFixture, NewDesignsGoIntoTheNamedLibrary)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    ASSERT_TRUE(read_lef(fixture_path("othercell.lef"), "more_cells"));
    ASSERT_EQ(library_count(), 2);
    for (int32_t i = 0; i < 2; ++i)
        EXPECT_EQ(library_design_count(i), 1);
}

// A LEF pin whose bracketed index doesn't fit an int is an ordinary name
// to the netlist read's stub modules, not a crash.
TEST_F(ApiFixture, ANetlistReadSurvivesAPinIndexOutOfIntRange)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    le::AbstractId abstract{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            if (const le::test::SessionTest::DesignInfo info = library_design_at(l, d); std::string(info.name) == "TESTCELL")
                abstract = info.abstract_id;
    ASSERT_NE(abstract.index, UINT32_MAX);
    ASSERT_NE(write().create_terminal({.abstract = abstract, .name = "A[99999999999]", .direction = le::SignalDirection::INPUT}).value_or(le::TerminalId{})
                  .index,
              UINT32_MAX);

    const std::filesystem::path verilog_path = le::test::scratch_path("le_huge_index_top.v");
    {
        std::ofstream out(verilog_path);
        out << "module TOP();\n  wire n1;\n  TESTCELL U1(.A(n1));\nendmodule\n";
    }
    const std::string paths[] = {verilog_path.string()};
    EXPECT_TRUE(write().read_verilog(paths, true, "top"));
    std::filesystem::remove(verilog_path);
    int32_t top = 0;
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            top += std::string(library_design_at(l, d).name) == "TOP";
    EXPECT_EQ(top, 1);
}

// read_verilog -rtl of two modules instantiating each other, then link:
// one instance stays unresolved and link terminates.
TEST_F(ApiFixture, LinkingARecursiveRtlHierarchyLeavesTheRecursiveInstanceUnresolved)
{
    const std::filesystem::path verilog_path = le::test::scratch_path("le_recursive.v");
    {
        std::ofstream out(verilog_path);
        out << "module a();\n  b u_b();\nendmodule\nmodule b();\n  a u_a();\nendmodule\n";
    }
    const std::string paths[] = {verilog_path.string()};
    ASSERT_TRUE(write().read_verilog(paths, false, "rtl"));
    std::filesystem::remove(verilog_path);

    le::DesignId a{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            if (const le::test::SessionTest::DesignInfo info = library_design_at(l, d); std::string(info.name) == "a")
                a = info.id;
    ASSERT_NE(a.index, UINT32_MAX);
    const le::LayoutId layout = write().create_layout({.design = a}).value_or(le::LayoutId{});
    ASSERT_NE(layout.index, UINT32_MAX);

    write().link_unresolved_instances().value_or(0);
    int32_t unresolved = 0;
    handle->root.for_each_instance_id([&](le::InstanceId id) { unresolved += !handle->root.get_instance(id)->reference_design.valid(); });
    EXPECT_EQ(unresolved, 1);
    EXPECT_LE(handle->root.get_layout_placements(le::LayoutId{layout.index, layout.generation}).size(), 2u);
}

// --- Flightlines ---

namespace
{
    // A light-blue-ish pixel (FLIGHTLINE's own (135,206,250) - blue
    // clearly above red, whatever the premultiplication/coverage).
    bool region_has_flightline_pixel(const le::RenderedFrame &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[3] > 50 && p[2] > 90 && p[2] > p[0] + 30)
                    return true;
            }
        return false;
    }
}

// TOP instantiates TESTCELL twice, U1.A and U2.A both on n1; placed 30um
// apart and linked, selecting U1 draws a light blue line between the two
// pin centers - but only once the (hidden-by-default) FLIGHTLINE purpose
// is made visible.
TEST_F(ApiFixture, SelectedPlacementDrawsFlightlinesOnlyWhenTheFlightlinePurposeIsVisible)
{
    const std::filesystem::path verilog_path = le::test::scratch_path("le_flightline_top.v");
    {
        std::ofstream out(verilog_path);
        out << "module TOP();\n  wire n1;\n  TESTCELL U1(.A(n1));\n  TESTCELL U2(.A(n1));\nendmodule\n";
    }
    const std::string paths[] = {verilog_path.string()};

    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    ASSERT_TRUE(write().read_verilog(paths, true, "top"));
    std::filesystem::remove(verilog_path);

    le::DesignId top_design{.index = UINT32_MAX, .generation = 0};
    le::DesignId testcell_design{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
        {
            const le::test::SessionTest::DesignInfo info = library_design_at(l, d);
            if (std::string(info.name) == "TOP")
                top_design = info.id;
            else if (std::string(info.name) == "TESTCELL")
                testcell_design = info.id;
        }
    ASSERT_NE(top_design.index, UINT32_MAX);
    ASSERT_NE(testcell_design.index, UINT32_MAX);

    const le::LayoutId layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    ASSERT_NE(create_placement(layout, testcell_design, "U1", le::PointUm{0.0, 0.0}).index, UINT32_MAX);
    ASSERT_NE(create_placement(layout, testcell_design, "U2", le::PointUm{30.0, 0.0}).index, UINT32_MAX);
    write().link_unresolved_instances().value_or(0); // resolves Placement.instance by name

    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    write().zoom(0.002 - 1.0, 0, 100); // 500 dbu/px, pan (0,0): pixel (x,y) = dbu (500x, 500(100-y))
    write().mouse_down(10, 95);        // dbu (5000,2500) - inside U1
    write().mouse_up(10, 95);
    ASSERT_EQ(selection_count(), 1);

    // Pin A's center is (5,5)um in each: the line runs y=90px from x=10 to
    // x=70 - sample its middle, clear of both cells' own geometry.
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_flightline_pixel(buffer, 30, 87, 50, 93)); // hidden by default

    write().set_purpose_visible(static_cast<le::ViewLayerPurpose>(/*FLIGHTLINE=*/14), 1);
    buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_flightline_pixel(buffer, 30, 87, 50, 93));

    write().clear_selection();
    buffer = render();
    EXPECT_FALSE(region_has_flightline_pixel(buffer, 30, 87, 50, 93));
}

// --- Shape resizing ---

namespace
{
    // A mouse click, Move/Resize style: position first (Move/Resize read the
    // stored mouse position), then a click-sized down/up.
    void resize_click(le::test::SessionHelpers &s, int32_t x, int32_t y)
    {
        s.write().set_mouse_position(x, y);
        s.write().mouse_down(x, y);
        s.write().mouse_up(x, y);
    }

    // TESTCELL's pin A rect (2,2)-(8,8)um selected, Edit mode, Resize armed,
    // at 50 dbu/px with pan (0,0) - pixel (x,y) = dbu (50x, 50(200-y)) - and
    // a 200-dbu user grid. Returns the rect's Shape.
    le::ShapeId arm_resize_on_testcell_pin(le::test::SessionHelpers &s, const std::string &lef_path)
    {
        EXPECT_TRUE(s.read_lef(lef_path, "testcell"));
        EXPECT_TRUE(s.open_abstract(size_t(0)));
        const le::AbstractId abstract_id = s.library_design_at(0, 0).abstract_id;
        const le::TerminalId terminal = s.child_at(&le::Root::get_abstract_terminals, abstract_id, 0);
        const le::ShapeId shape_id = s.child_at(&le::Root::get_terminal_port_shapes, s.child_at(&le::Root::get_terminal_ports, terminal, 0), 0);
        s.select(shape_id);
        s.write().set_viewport_size(200, 200);
        s.write().zoom(0.02 - 1.0, 0, 200);
        s.write().set_minor_grid_spacing(200);
        s.write().set_mode(le::InteractionMode::EDIT);
        s.write().arm_resize();
        return shape_id;
    }
}

// Hovering the rect's right edge (x=8000 dbu = pixel 160) highlights it and
// reports a left/right resize; the interior reports nothing.
TEST_F(ApiFixture, ResizeHoverIndicatesTheGrabbableEdgeUnderTheMouse)
{
    arm_resize_on_testcell_pin(*this, fixture_path("testcell.lef"));
    ASSERT_NE(read().resize_armed(), 0);
    EXPECT_EQ(read().move_armed(), 0);
    EXPECT_EQ(read().selected_piece_kinds(), 1 << static_cast<int>(le::PieceKind::RECT));

    write().set_mouse_position(100, 100);
    EXPECT_EQ(read().resize_hover_axis(), std::nullopt);
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 157, 60, 163, 140));

    write().set_mouse_position(162, 100);
    EXPECT_EQ(read().resize_hover_axis(), le::ResizeAxis::X);
    buffer = render();
    EXPECT_TRUE(region_has_yellow_hover_pixel(buffer, 157, 60, 163, 140));

    write().set_mouse_position(100, 41); // near the top edge (y=8000 dbu = pixel 40)
    EXPECT_EQ(read().resize_hover_axis(), le::ResizeAxis::Y);
}

// Two clicks: the first grabs the right edge (the ghost then follows the
// mouse, the real rect untouched), the second commits it - snapped to the
// 200-dbu user grid - as one undoable edit. Resize stays armed.
TEST_F(ApiFixture, ResizeGrabsOnTheFirstClickAndCommitsOnTheSecond)
{
    const le::ShapeId shape_id = arm_resize_on_testcell_pin(*this, fixture_path("testcell.lef"));

    // A click on nothing grabbable grabs nothing; nor does the next click.
    resize_click(*this, 100, 100);
    resize_click(*this, 120, 100);
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 8.0);

    resize_click(*this, 160, 100);
    EXPECT_EQ(read().resize_hover_axis(), std::nullopt); // no hover while grabbed
    write().set_mouse_position(187, 100); // 9350 dbu -> 9400 on the grid = pixel 188
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_move_ghost_pixel(buffer, 186, 60, 190, 140));
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 8.0);

    resize_click(*this, 187, 100);
    const le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 2.0);
    EXPECT_DOUBLE_EQ(rect.ll.y, 2.0);
    EXPECT_DOUBLE_EQ(rect.ur.x, 9.4);
    EXPECT_DOUBLE_EQ(rect.ur.y, 8.0);
    EXPECT_NE(read().resize_armed(), 0);

    ASSERT_NE(write().undo(), 0);
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 8.0);
}

// Escape after the first click cancels that resize (Resize stays armed);
// a second Escape, with nothing grabbed, disarms.
TEST_F(ApiFixture, EscapeCancelsAResizeInProgressThenDisarms)
{
    const le::ShapeId shape_id = arm_resize_on_testcell_pin(*this, fixture_path("testcell.lef"));

    resize_click(*this, 160, 100);
    write().set_mouse_position(187, 100);
    write().key_down(le::Key::FINISH_RULER);
    write().key_up(le::Key::FINISH_RULER);
    EXPECT_NE(read().resize_armed(), 0);

    resize_click(*this, 190, 100); // now just a click on nothing grabbable - no commit
    EXPECT_DOUBLE_EQ(rect_um(shape_id, 0).ur.x, 8.0);
    EXPECT_EQ(read().can_undo(), 0);

    write().key_down(le::Key::FINISH_RULER);
    EXPECT_EQ(read().resize_armed(), 0);
}

TEST_F(ApiFixture, ShapeSnapModesArePerKindAndOnlyAcceptModesTheKindOffers)
{
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::USER_GRID);
    write().set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::TRACKS);
    write().set_shape_snap_mode(le::PieceKind::RECT, le::ShapeSnapMode::TRACKS); // rects don't snap to tracks - ignored
    write().set_shape_snap_mode(le::PieceKind::POLYGON, le::ShapeSnapMode::NONE);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::TRACKS);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::USER_GRID);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::POLYGON), le::ShapeSnapMode::NONE);

    EXPECT_NE(read().shape_snap_mode_available(le::PieceKind::RECT, le::ShapeSnapMode::USER_GRID), 0);
    EXPECT_EQ(read().shape_snap_mode_available(le::PieceKind::RECT, le::ShapeSnapMode::MANUFACTURING_GRID), 0); // no technology yet
    EXPECT_EQ(read().shape_snap_mode_available(le::PieceKind::PATH, le::ShapeSnapMode::FIN_GRID), 0);      // not offered for paths
}

// A route whose three wire runs are separate paths (as DEF stores them):
// moving the middle run up drags the ends of the runs meeting it along,
// so the route stays connected - in the ghost and the committed result,
// as one undo step.
TEST_F(ApiFixture, ResizingAPathSegmentKeepsTheRouteConnected)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::LibraryId top_design_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    const le::RouteId route = write().create_route({.layout = layout, .name = "NET1"}).value_or(le::RouteId{});
    // In um: up (1,1)-(1,5), across (1,5)-(8,5), down (8,5)-(8,1).
    const le::LayerId m1 = layer_named("M1");
    le::ShapeId shape_id;
    {
        le::WriteView view = write();
        shape_id = view.build_shape(le::ShapeOwner::route(route))
                       .layer(m1)
                       .path(0.2, {{1, 1}, {1, 5}})
                       .path(0.2, {{1, 5}, {8, 5}})
                       .path(0.2, {{8, 5}, {8, 1}})
                       .create()
                       .value_or(le::ShapeId{});
    }
    ASSERT_NE(shape_id.index, UINT32_MAX);

    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(200, 200);
    write().zoom(0.02 - 1.0, 0, 200); // 50 dbu/px, pan (0,0): pixel (x,y) = um (x/20, (200-y)/20)
    write().set_minor_grid_spacing(200);
    ASSERT_TRUE(select(shape_id));
    write().set_mode(le::InteractionMode::EDIT);
    write().arm_resize();
    ASSERT_NE(read().resize_armed(), 0);

    write().set_mouse_position(90, 100); // on the across run, y=5um
    EXPECT_EQ(read().resize_hover_axis(), le::ResizeAxis::Y);
    resize_click(*this, 90, 100);
    write().set_mouse_position(90, 60); // y=7um
    // The ghost includes the stretched up/down runs, e.g. the up run at x=1um (pixel 20) above y=5um.
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_move_ghost_pixel(buffer, 17, 64, 23, 76));
    resize_click(*this, 90, 60);

    const auto point = [&](int path, int index)
    {
        const le::PointUm p = path_point_um(shape_id, path, index);
        return std::make_pair(p.x, p.y);
    };
    EXPECT_EQ(point(1, 0), std::make_pair(1.0, 7.0));
    EXPECT_EQ(point(1, 1), std::make_pair(8.0, 7.0));
    EXPECT_EQ(point(0, 1), std::make_pair(1.0, 7.0)); // the up run's top followed
    EXPECT_EQ(point(2, 0), std::make_pair(8.0, 7.0)); // the down run's top followed
    EXPECT_EQ(point(0, 0), std::make_pair(1.0, 1.0)); // far ends untouched
    EXPECT_EQ(point(2, 1), std::make_pair(8.0, 1.0));

    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(point(0, 1), std::make_pair(1.0, 5.0));
    EXPECT_EQ(point(1, 0), std::make_pair(1.0, 5.0));
}

// Ctrl-R arms Resize (and leaves the mode alone); a bare R still switches
// to Ruler mode.
TEST_F(ApiFixture, CtrlRArmsResizeWhileBareRStillSwitchesToRulerMode)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const le::AbstractId abstract_id = testcell_abstract_id(*this);
    const le::ShapeId shape_id = child_at(&le::Root::get_terminal_port_shapes, child_at(&le::Root::get_terminal_ports, child_at(&le::Root::get_abstract_terminals, abstract_id, 0), 0), 0);
    ASSERT_TRUE(select(shape_id));
    write().set_mode(le::InteractionMode::EDIT);

    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::RULER_MODE);
    write().key_up(le::Key::RULER_MODE);
    write().key_up(le::Key::CTRL);
    EXPECT_NE(read().resize_armed(), 0);
    EXPECT_EQ(read().mode(), le::InteractionMode::EDIT);

    write().key_down(le::Key::RULER_MODE);
    write().key_up(le::Key::RULER_MODE);
    EXPECT_EQ(read().mode(), le::InteractionMode::RULER);
    EXPECT_EQ(read().resize_armed(), 0); // leaving Edit mode disarms
}

// Resize won't arm while a placement
// is selected, even alongside a resizable shape.
TEST_F(ApiFixture, ResizeDoesNotArmWhileAPlacementIsSelected)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    const le::PlacementId placement_id = create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
    ASSERT_NE(placement_id.index, UINT32_MAX);
    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    const double route_rect_um[4] = {20.0, 20.0, 26.0, 26.0};
    const le::ShapeId route_shape_id = create_rects(le::ShapeOwner::route(route_id), layer_named("M1"), std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape_id.index, UINT32_MAX);
    ASSERT_TRUE(open_layout(top_design));
    write().set_mode(le::InteractionMode::EDIT);

    ASSERT_TRUE(select(route_shape_id));
    ASSERT_TRUE(select(placement_id));
    write().arm_resize();
    EXPECT_EQ(read().resize_armed(), 0);

    write().clear_selection();
    ASSERT_TRUE(select(route_shape_id));
    write().arm_resize();
    EXPECT_NE(read().resize_armed(), 0);
}

// Placements snap to sites/rows and
// shapes to their own grids, so Move refuses the two selected together -
// but still arms for either on its own.
TEST_F(ApiFixture, MoveDoesNotArmWithPlacementsAndOtherObjectsSelectedTogether)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    const le::test::SessionTest::DesignInfo testcell_design = library_design_at(0, 0);
    const le::LibraryId top_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    const le::PlacementId placement_id = create_placement(top_layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
    ASSERT_NE(placement_id.index, UINT32_MAX);
    const le::RouteId route_id = write().create_route({.layout = top_layout, .name = "NET1"}).value_or(le::RouteId{});
    const double route_rect_um[4] = {20.0, 20.0, 26.0, 26.0};
    const le::ShapeId route_shape_id = create_rects(le::ShapeOwner::route(route_id), layer_named("M1"), std::span<const double>(route_rect_um, 4));
    ASSERT_NE(route_shape_id.index, UINT32_MAX);
    ASSERT_TRUE(open_layout(top_design));
    write().set_mode(le::InteractionMode::EDIT);

    ASSERT_TRUE(select(route_shape_id));
    ASSERT_TRUE(select(placement_id));
    write().arm_move();
    EXPECT_EQ(read().move_armed(), 0);

    write().clear_selection();
    ASSERT_TRUE(select(placement_id));
    write().arm_move();
    EXPECT_NE(read().move_armed(), 0);
    write().cancel_move();

    write().clear_selection();
    ASSERT_TRUE(select(route_shape_id));
    write().arm_move();
    EXPECT_NE(read().move_armed(), 0);
}

// Paths, vias and via arrays share one snap mode, so a Move of wires and
// vias together snaps them all the same way.
TEST_F(ApiFixture, PathsViasAndViaArraysShareOneSnapMode)
{
    write().set_shape_snap_mode(le::PieceKind::VIA, le::ShapeSnapMode::MANUFACTURING_GRID);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::MANUFACTURING_GRID);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::VIA_ITERATE), le::ShapeSnapMode::MANUFACTURING_GRID);
    write().set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::NONE);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::VIA), le::ShapeSnapMode::NONE);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::USER_GRID); // rects stay separate
}

TEST_F(ApiFixture, FlightlineMaxFanoutDefaultsToTenAndRejectsNegativeValues)
{
    EXPECT_EQ(read().flightline_max_fanout(), 10);
    write().set_flightline_max_fanout(3);
    EXPECT_EQ(read().flightline_max_fanout(), 3);
    write().set_flightline_max_fanout(-1); // ignored
    EXPECT_EQ(read().flightline_max_fanout(), 3);
    write().set_flightline_max_fanout(0); // no limit
    EXPECT_EQ(read().flightline_max_fanout(), 0);
}

// --- Selecting and moving vias ---

namespace
{
    // via_cell.lef's VIACELL open in the Abstract view at 50 dbu/px, pan
    // (0,0) - pixel (x,y) = um (x/20, (200-y)/20). Pin A's port Shape has an
    // M1 rect at (1,1)-(3,3)um and a VIA12 (1um square) at (7,7)um =
    // pixel (140, 60).
    le::ShapeId open_via_cell(le::test::SessionHelpers &s, const std::string &lef_path)
    {
        EXPECT_TRUE(s.read_lef(lef_path, "via_cell"));
        EXPECT_TRUE(s.open_abstract(size_t(0)));
        const le::AbstractId abstract_id = s.library_design_at(0, 0).abstract_id;
        const le::TerminalId terminal = s.child_at(&le::Root::get_abstract_terminals, abstract_id, 0);
        s.write().set_viewport_size(200, 200);
        s.write().zoom(0.02 - 1.0, 0, 200);
        return s.child_at(&le::Root::get_terminal_port_shapes, s.child_at(&le::Root::get_terminal_ports, terminal, 0), 0);
    }
}

// A click on the via selects it (the via piece alone, not the pin's
// rect), and the selection outline traces its geometry.
TEST_F(ApiFixture, ClickingAViaSelectsItAndOutlinesIt)
{
    open_via_cell(*this, fixture_path("via_cell.lef"));


    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(is<le::ShapeId>(selected(0)));

    // The white outline runs along the via's 1um square: its left edge at
    // x = 6.5um = pixel 130.
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 128, 55, 132, 65));
    // ...and the pin's rect (x 20..60px) isn't outlined.
    EXPECT_FALSE(region_has_white_selection_pixel(buffer, 18, 150, 22, 170));

    // A drag-select around just the via selects just it too.
    write().mouse_down(120, 40);
    write().set_mouse_position(160, 80);
    write().mouse_up(160, 80);
    EXPECT_EQ(selection_count(), 1);
}

// Move a selected via: its origin moves (the pin's rect stays), shown by a
// ghost first, as one undoable edit.
TEST_F(ApiFixture, MovingASelectedViaMovesItsOriginAndIsUndoable)
{
    const le::ShapeId shape_id = open_via_cell(*this, fixture_path("via_cell.lef"));
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    write().set_minor_grid_spacing(100);
    write().set_mouse_position(140, 60); // anchor at (7,7)um
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    write().set_mouse_position(140, 100); // (7,5)um - a 2um move down

    // Ghost: the via's square around (7,5)um = pixel (140,100), its top edge at y = 5.5um = pixel 90.
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_move_ghost_pixel(buffer, 132, 88, 148, 92));

    write().mouse_down(140, 100);
    write().mouse_up(140, 100);

    const le::RectUm rect = rect_um(shape_id, 0);
    EXPECT_DOUBLE_EQ(rect.ll.x, 1.0); // the pin's rect didn't move
    EXPECT_DOUBLE_EQ(rect.ur.y, 3.0);

    // Selecting the via again at its new place proves the move landed.
    write().set_mode(le::InteractionMode::SELECT);
    write().clear_selection();
    write().mouse_down(140, 100);
    write().mouse_up(140, 100);
    EXPECT_EQ(selection_count(), 1);

    ASSERT_NE(write().undo(), 0);
    write().clear_selection();
    write().mouse_down(140, 100);
    write().mouse_up(140, 100);
    EXPECT_EQ(selection_count(), 0); // back at (7,7) - nothing at (7,5)
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    EXPECT_EQ(selection_count(), 1);
}

// --- Selecting and moving via arrays ---
// via_array_cell.lef: the same cell and 50 dbu/px view as open_via_cell,
// with a 2x2 VIA12 array (instances at (5,5)..(7,7)um) instead of the single
// via - its box is (4.5,4.5)-(7.5,7.5)um = pixels x 90..150, y 50..110.

// A click anywhere in the array's box - here its centre, between the four
// instances - selects the whole array as one piece and outlines it; so does
// a drag around it.
TEST_F(ApiFixture, ClickingAViaArraySelectsTheWholeArray)
{
    open_via_cell(*this, fixture_path("via_array_cell.lef"));

    write().mouse_down(120, 80); // (6,6)um
    write().mouse_up(120, 80);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(is<le::ShapeId>(selected(0)));

    // Outlined: the lower-left instance's left edge (x = 4.5um = pixel 90)
    // and the upper-right one's right edge (x = 7.5um = pixel 150).
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 88, 95, 92, 105));
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 148, 55, 152, 65));

    write().clear_selection();
    write().mouse_down(80, 40);
    write().set_mouse_position(160, 120);
    write().mouse_up(160, 120);
    EXPECT_EQ(selection_count(), 1); // the array, not four vias
}

// Moving a selected via array moves every instance (its origin), as one
// undoable edit.
TEST_F(ApiFixture, MovingASelectedViaArrayMovesEveryInstanceAndIsUndoable)
{
    open_via_cell(*this, fixture_path("via_array_cell.lef"));
    write().mouse_down(120, 80);
    write().mouse_up(120, 80);
    ASSERT_EQ(selection_count(), 1);

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    write().set_minor_grid_spacing(100);
    write().set_mouse_position(120, 80); // anchor at (6,6)um
    write().mouse_down(120, 80);
    write().mouse_up(120, 80);
    write().set_mouse_position(120, 120); // (6,4)um - a 2um move down
    write().mouse_down(120, 120);
    write().mouse_up(120, 120);

    // The array's box is now (4.5,2.5)-(7.5,5.5)um: (6,3)um hits it,
    // (6,7)um - inside the old box only - doesn't.
    auto selects_at = [&](int32_t x, int32_t y)
    {
        write().clear_selection();
        write().mouse_down(x, y);
        write().mouse_up(x, y);
        return selection_count();
    };
    write().set_mode(le::InteractionMode::SELECT);
    EXPECT_EQ(selects_at(120, 140), 1); // (6,3)um
    EXPECT_EQ(selects_at(120, 60), 0);  // (6,7)um

    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(selects_at(120, 140), 0);
    EXPECT_EQ(selects_at(120, 60), 1);
}

// Layout view: a DEF route's via (N1's VIA12 at (7,7)um, between an M1 wire
// ending there and an M2 wire leaving it) is selected by a click - rather
// than the M1 wire under it - and moves on its own.
TEST_F(ApiFixture, ARoutesViaIsSelectableAndMovableInTheLayoutView)
{
    ASSERT_TRUE(read_lef(fixture_path("via_cell.lef"), "via_cell"));
    ASSERT_TRUE(read_def(fixture_path("via_route.def"), "top"));
    le::DesignId top_design{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            if (std::string(library_design_at(l, d).name) == "VIATOP")
                top_design = library_design_at(l, d).id;
    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(200, 200);
    write().zoom(0.02 - 1.0, 0, 200); // pixel (x,y) = um (x/20, (200-y)/20)

    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    ASSERT_EQ(selection_count(), 1);
    le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 128, 55, 132, 65));  // the via's outline
    EXPECT_FALSE(region_has_white_selection_pixel(buffer, 55, 45, 65, 55));   // not the M1 wire's

    write().set_mode(le::InteractionMode::EDIT);
    write().arm_move();
    write().set_minor_grid_spacing(100);
    write().set_mouse_position(140, 60);
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    write().set_mouse_position(140, 100); // 2um down
    write().mouse_down(140, 100);
    write().mouse_up(140, 100);

    write().set_mode(le::InteractionMode::SELECT);
    write().clear_selection();
    write().mouse_down(140, 100);
    write().mouse_up(140, 100);
    ASSERT_EQ(selection_count(), 1); // the via, now at (7,5)um - on the M2 wire, but the via wins
    write().set_mouse_position(5, 195);
    buffer = render();
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 128, 95, 132, 105));
}

// --- Move snaps routes and vias ---

namespace
{
    // via_route.def's VIATOP open in the Layout view, same 20 px/um view as
    // ARoutesViaIsSelectableAndMovableInTheLayoutView - pixel (x,y) = um
    // (x/20, (200-y)/20). M1's LEF PITCH 2um gives tracks every 2um from
    // 1um (the default offset, pitch/2): ... 5, 7 ... um.
    void open_via_route(le::test::SessionHelpers &s, const std::string &lef_path, const std::string &def_path)
    {
        EXPECT_TRUE(s.read_lef(lef_path, "via_cell"));
        EXPECT_TRUE(s.read_def(def_path, "top"));
        for (int32_t l = 0; l < s.library_count(); ++l)
            for (int32_t d = 0; d < s.library_design_count(l); ++d)
                if (std::string(s.library_design_at(l, d).name) == "VIATOP")
                    EXPECT_TRUE(s.open_layout(s.library_design_at(l, d).id));
        s.write().set_viewport_size(200, 200);
        s.write().zoom(0.02 - 1.0, 0, 200);
        s.write().set_minor_grid_spacing(100);
    }

    // Moves the selection from pixel `from` to `to` (two Move clicks).
    void move_selection(le::test::SessionHelpers &s, int32_t from_x, int32_t from_y, int32_t to_x, int32_t to_y)
    {
        s.write().set_mode(le::InteractionMode::EDIT);
        s.write().arm_move();
        s.write().set_mouse_position(from_x, from_y);
        s.write().mouse_down(from_x, from_y);
        s.write().mouse_up(from_x, from_y);
        s.write().set_mouse_position(to_x, to_y);
        s.write().mouse_down(to_x, to_y);
        s.write().mouse_up(to_x, to_y);
        s.write().set_mouse_position(5, 195);
    }
}

// The via at (7,7)um moved 1.5um down: on the user grid it would land at
// (7,5.5); snapping its origin to M1's tracks puts it at (7,5) instead.
TEST_F(ApiFixture, MovingAViaSnapsItsOriginToTracksWhenAsked)
{
    open_via_route(*this, fixture_path("via_cell.lef"), fixture_path("via_route.def"));
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_EQ(read().selected_move_snap_piece_kinds(), 1 << static_cast<int>(le::PieceKind::PATH)); // vias share the routing (path) group

    ASSERT_NE(read().shape_snap_mode_available(le::PieceKind::VIA, le::ShapeSnapMode::TRACKS), 0);
    write().set_shape_snap_mode(le::PieceKind::VIA, le::ShapeSnapMode::TRACKS);
    move_selection(*this, 140, 60, 140, 90); // raw (7,5.5)um

    // Still selected: its outline's top edge is at 5.5um = pixel 90, not
    // 6um = pixel 80 (where the user grid would have put it).
    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 134, 88, 146, 92));
    EXPECT_FALSE(region_has_white_selection_pixel(buffer, 134, 78, 146, 82));
}

TEST_F(ApiFixture, MovingAViaSnapsItsOriginToTheUserGridByDefault)
{
    open_via_route(*this, fixture_path("via_cell.lef"), fixture_path("via_route.def"));
    write().mouse_down(140, 60);
    write().mouse_up(140, 60);
    ASSERT_EQ(read().shape_snap_mode(le::PieceKind::VIA), le::ShapeSnapMode::USER_GRID);
    move_selection(*this, 140, 60, 140, 90);

    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 134, 78, 146, 82)); // top edge at 6um
}

// N1's M1 wire (centreline y = 7um, width 1um) moved 1.5um down with paths
// snapping to tracks: its centreline lands on the 5um track, so its top
// edge is at 5.5um = pixel 90.
TEST_F(ApiFixture, MovingARoutesPathSnapsItsCentrelineToTracksWhenAsked)
{
    open_via_route(*this, fixture_path("via_cell.lef"), fixture_path("via_route.def"));
    write().mouse_down(60, 60); // (3,7)um - on the M1 wire only
    write().mouse_up(60, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_EQ(read().selected_move_snap_piece_kinds(), 1 << static_cast<int>(le::PieceKind::PATH));

    write().set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::TRACKS);
    move_selection(*this, 60, 60, 60, 90);

    const le::RenderedFrame buffer = render();
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(buffer, 40, 88, 80, 92));
    EXPECT_FALSE(region_has_white_selection_pixel(buffer, 40, 78, 80, 82));
}

// --- Click cycling: a plain click steps through everything under the mouse ---

namespace
{
    uint32_t selected_index(le::test::SessionHelpers &s)
    {
        if (s.selection_count() != 1)
            return UINT32_MAX;
        return std::visit([](auto id) { return id.index; }, *s.selected(0));
    }
}

// PIN A's rect (2,2)-(8,8)um with an M1 obstruction (4,4)-(6,6)um on top
// of it: repeated clicks at (5,5)um alternate between the two, one at a
// time; shift-clicks add them to the selection one by one instead.
TEST_F(ApiFixture, RepeatedClicksCycleThroughOverlappingShapes)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    const double obstruction_um[4] = {4.0, 4.0, 6.0, 6.0};
    create_obstruction_with_rect(*this, testcell_abstract_id(*this), "M1", obstruction_um);
    write().set_viewport_size(100, 100);
    write().zoom(100.0 / 10000.0 - 1.0, 0, 100); // 100 dbu/px: (5,5)um = pixel (50,50)

    click(50, 50);
    const uint32_t first = selected_index(*this);
    ASSERT_NE(first, UINT32_MAX);
    click(50, 50);
    const uint32_t second = selected_index(*this);
    ASSERT_NE(second, UINT32_MAX);
    EXPECT_NE(second, first); // the other overlapping shape
    click(50, 50);
    EXPECT_EQ(selected_index(*this), first); // wrapped round

    // Shift-click adds the first object under the mouse not already
    // selected: with just the pin selected, the obstruction; after that
    // both objects at (5,5) are selected, so another shift-click adds
    // nothing.
    click(30, 70); // (3,3)um - only the pin
    const uint32_t pin = selected_index(*this);
    ASSERT_NE(pin, UINT32_MAX);
    write().key_down(le::Key::SHIFT);
    click(50, 50);
    EXPECT_EQ(selection_count(), 2);
    EXPECT_NE(std::visit([](auto id) { return id.index; }, *selected(1)), pin); // the obstruction was added
    click(50, 50);
    EXPECT_EQ(selection_count(), 2);
    write().key_up(le::Key::SHIFT);

    // A click on nothing clears the selection; from there two shift-clicks
    // at (5,5) add both stacked objects, one each.
    click(95, 5);
    EXPECT_EQ(selection_count(), 0);
    write().key_down(le::Key::SHIFT);
    click(50, 50);
    EXPECT_EQ(selection_count(), 1);
    click(50, 50);
    EXPECT_EQ(selection_count(), 2);
    write().key_up(le::Key::SHIFT);
}

// Layout view: at N1's via (7,7)um, the via comes first, then the wires
// meeting it, each once, then back to the via.
TEST_F(ApiFixture, ClickCyclingInTheLayoutViewStartsWithTheViaAndVisitsEachObjectOnce)
{
    ASSERT_TRUE(read_lef(fixture_path("via_cell.lef"), "via_cell"));
    ASSERT_TRUE(read_def(fixture_path("via_route.def"), "top"));
    le::DesignId top_design{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            if (std::string(library_design_at(l, d).name) == "VIATOP")
                top_design = library_design_at(l, d).id;
    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(200, 200);
    write().zoom(0.02 - 1.0, 0, 200); // (7,7)um = pixel (140,60)

    // The via is told apart by its V1 cut's outline (6.8-7.2um square,
    // left edge at pixel 136) - the only outline no wire shares; its 1um
    // metal squares coincide with the 1um-wide wires' own edges.
    const auto via_selected = [&]
    {
        const le::RenderedFrame buffer = render();
        return buffer.data && region_has_white_selection_pixel(buffer, 135, 57, 137, 63);
    };

    click(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(via_selected()); // the via first

    // Then the M1 wire and the M2 wire meeting it (separate Shapes), one
    // at a time, then back to the via.
    click(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_FALSE(via_selected());
    const std::optional<le::ObjectId> wire_a = selected(0);
    click(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_FALSE(via_selected());
    EXPECT_NE(selected(0), wire_a);
    click(140, 60);
    ASSERT_EQ(selection_count(), 1);
    EXPECT_TRUE(via_selected());
}

// --- Settings ---

namespace
{
    std::string read_file(const std::string &path)
    {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void write_file(const std::string &path, const std::string &text)
    {
        std::ofstream(path, std::ios::trunc) << text;
    }
}

// Every setting survives a save to JSON and a load into a fresh handle.
TEST_F(ApiFixture, SettingsSaveThenLoadRoundTripsEverySetting)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell")); // 1000 dbu/um
    write().set_grid_spacing_um(0.2, 2.0);
    write().set_ruler_label_size(14.0);
    write().set_label_min_size(10.0);
    write().set_label_max_size(18.0);
    write().set_hierarchy_depth(3);
    write().set_flightline_max_fanout(7);
    write().set_max_concurrency(5); // the Settings panel's "CPUs"
    write().set_placement_snap_mode(le::PlacementSnapMode::MANUFACTURING_GRID);
    write().set_shape_snap_mode(le::PieceKind::POLYGON, le::ShapeSnapMode::NONE);
    write().set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::TRACKS);
    EXPECT_EQ(read().minor_grid_spacing(), 200);
    EXPECT_EQ(read().major_grid_spacing(), 2000);

    const std::string path = le::test::scratch_path("le_settings_round_trip/settings.json");
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value()); // creates the directory
    const std::string text = read_file(path);
    EXPECT_NE(text.find("\"minor_um\": 0.2"), std::string::npos) << text;
    EXPECT_NE(text.find("\"placement_snap_mode\": \"manufacturing\""), std::string::npos) << text;
    EXPECT_EQ(text.find("hierarchy_depth"), std::string::npos) << text;

    le::test::SessionHelpers other;
    ASSERT_TRUE(other.read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(other.write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(other.read().minor_grid_spacing(), 200);
    EXPECT_EQ(other.read().major_grid_spacing(), 2000);
    EXPECT_DOUBLE_EQ(other.read().ruler_label_size(), 14.0);
    EXPECT_DOUBLE_EQ(other.read().label_min_size(), 10.0);
    EXPECT_DOUBLE_EQ(other.read().label_max_size(), 18.0);
    EXPECT_EQ(other.read().hierarchy_depth(), 0); // a per-session view choice, not saved
    EXPECT_EQ(other.read().flightline_max_fanout(), 7);
    EXPECT_EQ(other.read().max_concurrency(), 5);
    EXPECT_EQ(other.read().placement_snap_mode(), le::PlacementSnapMode::MANUFACTURING_GRID);
    EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::POLYGON), le::ShapeSnapMode::NONE);
    EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::TRACKS);
    EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::VIA), le::ShapeSnapMode::TRACKS); // shares the path mode
    EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::USER_GRID);
}

// Loaded before any LEF (le_shell loads the default file at startup), the
// um grid spacing waits for a Technology to convert it to dbu.
TEST_F(ApiFixture, SettingsLoadedBeforeAnyTechnologyApplyTheGridOnceOneIsRead)
{
    const std::string path = le::test::scratch_path("le_settings_pending.json");
    write_file(path, R"({"grid": {"minor_um": 0.2, "major_um": 2.0}})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_DOUBLE_EQ(read().grid_spacing_um(0), 0.2);
    EXPECT_EQ(read().minor_grid_spacing(), 5); // unchanged - no dbu scale yet

    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(read().minor_grid_spacing(), 200);
    EXPECT_EQ(read().major_grid_spacing(), 2000);
    EXPECT_DOUBLE_EQ(read().grid_spacing_um(1), 2.0);
}

// A key with the wrong type or an unknown value is skipped, the rest still
// load; a file that isn't a JSON object fails the load and changes nothing.
TEST_F(ApiFixture, SettingsLoadSkipsInvalidKeysAndRejectsMalformedFiles)
{
    const std::string path = le::test::scratch_path("le_settings_partial.json");
    write_file(path, R"({"label_max_size_px": "big", "flightline_max_fanout": 2, "placement_snap_mode": "nowhere",
                          "shape_snap_modes": {"rect": "tracks", "path": "none"}})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_DOUBLE_EQ(read().label_max_size(), 24.0);
    EXPECT_EQ(read().flightline_max_fanout(), 2);
    EXPECT_EQ(read().placement_snap_mode(), le::PlacementSnapMode::SITE);
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::USER_GRID); // rects can't snap to tracks
    EXPECT_EQ(read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::NONE);

    write_file(path, "not json");
    EXPECT_FALSE(write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(read().flightline_max_fanout(), 2);
    EXPECT_FALSE(write().load_settings(le::test::scratch_path("le_settings_missing.json").c_str()).has_value());
}

// The label font size caps how large a label renders: zoomed in on
// TESTCELL's pin (its own label wants more than the 24px default cap), a
// 12px cap draws a visibly different (smaller) label.
TEST_F(ApiFixture, LabelSizeCapsTheRenderedLabelSize)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);
    fit_rect(2.0, 2.0, 8.0, 8.0, 0);

    const auto render_bytes = [&]
    {
        const le::RenderedFrame buffer = render();
        return std::vector<uint8_t>(buffer.data, buffer.data + static_cast<size_t>(buffer.height) * static_cast<size_t>(buffer.row_bytes));
    };
    const std::vector<uint8_t> large = render_bytes();
    write().set_label_max_size(12.0);
    const std::vector<uint8_t> small = render_bytes();
    ASSERT_EQ(large.size(), small.size());
    size_t differing = 0;
    for (size_t i = 0; i < large.size(); i += 4)
        differing += std::memcmp(&large[i], &small[i], 4) != 0 ? 1 : 0;
    EXPECT_GT(differing, 50u);

    write().set_label_max_size(24.0); // back to the default - identical to the first render
    EXPECT_EQ(render_bytes(), large);
}

// The min label font size floors how small a label renders: zoomed out
// (~1.8 px/um) TESTCELL's 6um pin label wants ~6.5px, so it's drawn at the
// 12px default floor - a 20px floor draws a visibly larger one.
TEST_F(ApiFixture, LabelMinSizeFloorsTheRenderedLabelSize)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);
    fit_rect(-50.0, -50.0, 60.0, 60.0, 0);

    const auto render_bytes = [&]
    {
        const le::RenderedFrame buffer = render();
        return std::vector<uint8_t>(buffer.data, buffer.data + static_cast<size_t>(buffer.height) * static_cast<size_t>(buffer.row_bytes));
    };
    const std::vector<uint8_t> floored = render_bytes();
    write().set_label_min_size(20.0);
    const std::vector<uint8_t> larger = render_bytes();
    ASSERT_EQ(floored.size(), larger.size());
    size_t differing = 0;
    for (size_t i = 0; i < floored.size(); i += 4)
        differing += std::memcmp(&floored[i], &larger[i], 4) != 0 ? 1 : 0;
    EXPECT_GT(differing, 20u);

    write().set_label_min_size(12.0); // back to the default - identical to the first render
    EXPECT_EQ(render_bytes(), floored);
}

// The Settings panel's "use manufacturing grid" button reads the LEF's own
// MANUFACTURINGGRID in um - 0 before any Technology, or when it has none.
TEST_F(ApiFixture, ManufacturingGridUmIsTheLefValueOrZero)
{
    EXPECT_EQ(manufacturing_grid_um(), 0.0);

    le::test::SessionHelpers no_grid;
    ASSERT_TRUE(no_grid.read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(no_grid.manufacturing_grid_um(), 0.0);

    const std::string path = le::test::scratch_path("le_mfg_grid.lef");
    write_file(path, "VERSION 5.8 ;\nUNITS\n  DATABASE MICRONS 2000 ;\nEND UNITS\nMANUFACTURINGGRID 0.005 ;\nEND LIBRARY\n");
    ASSERT_TRUE(read_lef(path, "tech"));
    EXPECT_DOUBLE_EQ(manufacturing_grid_um(), 0.005);
}

// A layer's picked color replaces its
// default everywhere - the Layers panel row and the rendered image - and
// reset_layer_color brings the default back. One picked before the LEF is
// read applies once it is.
TEST_F(ApiFixture, LayerColorAppliesToTheRowAndTheRenderAndResets)
{
    set_layer_color("M1", 1, 2, 3); // no Technology yet - kept for later
    EXPECT_EQ(layer_color_rgb("M1"), -1);
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(layer_color_rgb("M1"), 0x010203);

    write().reset_layer_color("M1");
    const int32_t default_rgb = layer_color_rgb("M1");
    EXPECT_NE(default_rgb, 0x010203);
    EXPECT_EQ(default_rgb, 0xff0000); // the palette's first slot

    ASSERT_TRUE(open_abstract(size_t(0)));
    write().set_viewport_size(200, 200);
    fit_rect(0.0, 0.0, 10.0, 10.0, 0);
    const auto render_bytes = [&]
    {
        const le::RenderedFrame buffer = render();
        return std::vector<uint8_t>(buffer.data, buffer.data + static_cast<size_t>(buffer.height) * static_cast<size_t>(buffer.row_bytes));
    };
    const std::vector<uint8_t> red = render_bytes();
    set_layer_color("M1", 0, 0, 255);
    EXPECT_EQ(layer_color_rgb("M1"), 0x0000ff);
    EXPECT_NE(render_bytes(), red);
    write().reset_layer_color("M1");
    EXPECT_EQ(render_bytes(), red);

    EXPECT_EQ(layer_color_rgb("NO_SUCH_LAYER"), -1);
}

// Picked layer colors are saved as "#rrggbb" and loaded back; a loaded file's
// colors replace the current ones.
TEST_F(ApiFixture, SettingsSaveThenLoadRoundTripsLayerColors)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    set_layer_color("M1", 0x12, 0x34, 0xab);
    const std::string path = le::test::scratch_path("le_settings_layer_colors.json");
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    EXPECT_NE(read_file(path).find("\"M1\": \"#1234ab\""), std::string::npos) << read_file(path);

    le::test::SessionHelpers other;
    ASSERT_TRUE(other.read_lef(fixture_path("testcell.lef"), "testcell"));
    other.set_layer_color("BOUNDARY", 9, 9, 9); // replaced by the load
    ASSERT_TRUE(other.write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(other.layer_color_rgb("M1"), 0x1234ab);
    EXPECT_NE(other.layer_color_rgb("BOUNDARY"), 0x090909);
}

// What the exit confirmation counts as unsaved. Only a .led file holds the
// whole database: the design is unsaved after reading LEF/DEF/Verilog or any
// edit, until write_db (write_def/write_lef don't save it); a setting is
// until save_settings/load_settings.
TEST_F(ApiFixture, UnsavedChangesTrackEditsWritesAndSettings)
{
    EXPECT_EQ(read().has_unsaved_database(), 0); // a new, empty session
    EXPECT_EQ(read().has_unsaved_settings(), 0);
    EXPECT_FALSE(read_lef(le::test::scratch_path("no_such_file.lef"), "missing"));
    EXPECT_EQ(read().has_unsaved_database(), 0); // a failed read changes nothing
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    EXPECT_EQ(read().has_unsaved_database(), 1); // read, but not in a .led file yet
    EXPECT_EQ(read().has_unsaved_settings(), 0);         // its grid spacing in um isn't a setting change

    const le::LibraryId top_design_library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
    const le::DesignId top_design = write().create_design({.library = top_design_library, .name = "TOP"}).value_or(le::DesignId{});
    const le::LayoutId top_layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});
    ASSERT_TRUE(write().write_def(le::test::scratch_path("le_unsaved_changes.def").c_str(), top_layout).has_value());
    EXPECT_EQ(read().has_unsaved_database(), 1) << "write_def doesn't save the design";
    ASSERT_TRUE(write().write_lef(le::test::scratch_path("le_unsaved_changes.lef"), {}, le::LibraryId{UINT32_MAX, 0}, le::LefLayerWriteMode::TECHNOLOGY_ONLY));
    EXPECT_EQ(read().has_unsaved_database(), 1) << "nor does write_lef";
    ASSERT_TRUE(write().write_db(le::test::scratch_path("le_unsaved_changes.led").c_str(), 0).has_value());
    EXPECT_EQ(read().has_unsaved_database(), 0);

    const std::string extra_lef = le::test::scratch_path("le_unsaved_extra.lef");
    write_file(extra_lef, "VERSION 5.8 ;\nMACRO EXTRA\n  SIZE 1 BY 1 ;\nEND EXTRA\nEND LIBRARY\n");
    ASSERT_TRUE(read_lef(extra_lef, "extra"));
    EXPECT_EQ(read().has_unsaved_database(), 1); // a read after the save is unsaved again

    write().set_label_max_size(30.0);
    EXPECT_EQ(read().has_unsaved_settings(), 1);
    const std::string settings_path = le::test::scratch_path("le_unsaved_settings.json");
    ASSERT_TRUE(write().save_settings(settings_path.c_str()).has_value());
    EXPECT_EQ(read().has_unsaved_settings(), 0);
    set_layer_color("M1", 1, 2, 3);
    EXPECT_EQ(read().has_unsaved_settings(), 1);
    ASSERT_TRUE(write().load_settings(settings_path.c_str()).has_value());
    EXPECT_EQ(read().has_unsaved_settings(), 0);

}

// close_gui's request is one-shot, like show_gui's.
TEST_F(ApiFixture, CloseGuiRequestIsTakenOnce)
{
    EXPECT_EQ(session.requests().take_close_gui_request(), 0);
    session.requests().request_close_gui();
    EXPECT_EQ(session.requests().take_close_gui_request(), 1);
    EXPECT_EQ(session.requests().take_close_gui_request(), 0);
}

// A render with nothing loaded yet (the
// GUI opened by show_gui before any read_lef) must not leave the view
// blank once a design is loaded and opened.
TEST_F(ApiFixture, RenderingBeforeAnythingIsLoadedDoesNotBlankLaterRenders)
{
    const auto load_and_render = [&](le::test::SessionHelpers &h, bool render_empty_first)
    {
        h.write().set_viewport_size(200, 200);
        if (render_empty_first)
            h.render(); // the GUI's first frame, before any LEF
        EXPECT_TRUE(h.read_lef(fixture_path("testcell.lef"), "testcell"));
        EXPECT_TRUE(h.open_abstract(size_t(0)));
        h.write().fit_view(10);
        const le::RenderedFrame buffer = h.render();
        return std::vector<uint8_t>(buffer.data, buffer.data + static_cast<size_t>(buffer.height) * static_cast<size_t>(buffer.row_bytes));
    };
    le::test::SessionHelpers fresh;
    const std::vector<uint8_t> expected = load_and_render(fresh, false);
    EXPECT_EQ(load_and_render(*this, true), expected);
}

// Settings files saved before the min/max split carry one "label_size_px" -
// the max.
TEST_F(ApiFixture, SettingsLoadReadsTheOldSingleLabelSizeAsTheMax)
{
    const std::string path = le::test::scratch_path("le_settings_old_label_size.json");
    write_file(path, R"({"label_size_px": 30})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_DOUBLE_EQ(read().label_max_size(), 30.0);
    EXPECT_DOUBLE_EQ(read().label_min_size(), 12.0);
}

// Whether Save asks before overwriting is a saved setting, on by default.
TEST_F(ApiFixture, ConfirmOverwriteIsASavedSettingOnByDefault)
{
    EXPECT_EQ(read().confirm_overwrite(), 1);
    write().set_confirm_overwrite(0);
    EXPECT_EQ(read().has_unsaved_settings(), 1);
    const std::string path = le::test::scratch_path("le_settings_confirm_overwrite.json");
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    le::test::SessionHelpers other;
    EXPECT_EQ(other.read().confirm_overwrite(), 1);
    ASSERT_TRUE(other.write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(other.read().confirm_overwrite(), 0);
}

// The GUI color theme is a saved setting, gray by default; an unknown name
// is refused and keeps the current theme.
TEST_F(ApiFixture, ThemeIsASavedSettingGrayByDefault)
{
    EXPECT_EQ(theme(), "gray");
    EXPECT_EQ(le::kThemes[0].name, "gray");
    EXPECT_FALSE(write().set_theme("orange"));
    EXPECT_EQ(theme(), "gray");
    ASSERT_TRUE(write().set_theme("purple"));
    EXPECT_EQ(theme(), "purple");
    EXPECT_EQ(read().has_unsaved_settings(), 1);
    const std::string path = le::test::scratch_path("le_settings_theme.json");
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    le::test::SessionHelpers other;
    ASSERT_TRUE(other.write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(other.theme(), "purple");
}

// A settings file naming a theme this build doesn't have keeps the current one.
TEST_F(ApiFixture, SettingsLoadSkipsAnUnknownTheme)
{
    ASSERT_TRUE(write().set_theme("blue"));
    const std::string path = le::test::scratch_path("le_settings_unknown_theme.json");
    write_file(path, R"({"theme": "orange"})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(theme(), "blue");
}

// Every settings format ever written has a golden file in
// fixtures/settings/; each must keep loading into the same settings.
TEST_F(ApiFixture, SettingsGoldenFilesFromEveryFormatVersionLoad)
{
    for (const char *name : {"v1_original.json", "v1_latest.json", "v2.json", "v3.json"})
    {
        SCOPED_TRACE(name);
        le::test::SessionHelpers other;
        ASSERT_TRUE(other.read_lef(fixture_path("testcell.lef"), "testcell"));
        ASSERT_TRUE(other.write().load_settings(fixture_path(std::string("settings/") + name).c_str()).has_value());
        EXPECT_EQ(other.read().minor_grid_spacing(), 200);
        EXPECT_EQ(other.read().major_grid_spacing(), 2000);
        EXPECT_DOUBLE_EQ(other.read().ruler_label_size(), 14.0);
        EXPECT_EQ(other.read().hierarchy_depth(), 0); // a v1/v2 file's hierarchy_depth isn't restored
        EXPECT_EQ(other.read().flightline_max_fanout(), 7);
        EXPECT_EQ(other.read().placement_snap_mode(), le::PlacementSnapMode::MANUFACTURING_GRID);
        EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::USER_GRID);
        EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::POLYGON), le::ShapeSnapMode::NONE);
        EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::TRACKS);
        // v1's own "via" mode is dropped: vias share the path mode.
        EXPECT_EQ(other.read().shape_snap_mode(le::PieceKind::VIA), le::ShapeSnapMode::TRACKS);
        if (std::string(name) == "v1_original.json")
        {
            EXPECT_DOUBLE_EQ(other.read().label_min_size(), 12.0);
            EXPECT_DOUBLE_EQ(other.read().label_max_size(), 30.0); // label_size_px
        }
        else
        {
            EXPECT_DOUBLE_EQ(other.read().label_min_size(), 10.0);
            EXPECT_DOUBLE_EQ(other.read().label_max_size(), 18.0);
            EXPECT_EQ(other.read().max_concurrency(), 5);
            EXPECT_EQ(other.layer_color_rgb("M1"), 0xff8000);
        }
        // Older files have no undo_limit: the default.
        EXPECT_EQ(other.read().undo_limit(), std::string(name) == "v3.json" ? 50 : 200);
        // Nor a theme.
        EXPECT_EQ(other.theme(), std::string(name) == "v3.json" ? "teal" : "gray");
        // Migrated on load, so nothing is left over to keep as unknown.
        const std::string saved = le::test::scratch_path("le_settings_golden_resave.json");
        ASSERT_TRUE(other.write().save_settings(saved.c_str()).has_value());
        EXPECT_EQ(read_file(saved).find("\"label_size_px\""), std::string::npos);
        EXPECT_EQ(read_file(saved).find("\"via\""), std::string::npos);
        EXPECT_EQ(read_file(saved).find("\"hierarchy_depth\""), std::string::npos);
    }
}

// What save_settings writes must match the current format's golden file.
// If this fails because the format changed: adding a key only needs the
// golden updated; renaming, moving or redefining one needs kSettingsVersion
// bumped, a migrate_settings_json step, and a new golden beside the old.
TEST_F(ApiFixture, SettingsSaveMatchesTheCurrentFormatsGoldenFile)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "testcell"));
    ASSERT_TRUE(write().load_settings(fixture_path("settings/v3.json").c_str()).has_value());
    const std::string path = le::test::scratch_path("le_settings_current_format.json");
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    EXPECT_EQ(read_file(path), read_file(fixture_path("settings/v3.json")));
}

// A file from a newer layout_engine loads the settings this one knows, and
// saving keeps the top-level keys it doesn't, so they survive a round trip
// through an older version.
TEST_F(ApiFixture, SettingsFromANewerVersionLoadAndKeepUnknownKeysOnSave)
{
    const std::string path = le::test::scratch_path("le_settings_newer.json");
    write_file(path, R"({"version": 99, "flightline_max_fanout": 2, "future_panel": {"open": true}, "future_size": 3})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(read().flightline_max_fanout(), 2);
    EXPECT_EQ(read().has_unsaved_settings(), 0);

    write().set_flightline_max_fanout(4);
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    const std::string text = read_file(path);
    EXPECT_NE(text.find("\"future_panel\": {\n    \"open\": true\n  }"), std::string::npos) << text;
    EXPECT_NE(text.find("\"future_size\": 3"), std::string::npos) << text;
    EXPECT_NE(text.find("\"flightline_max_fanout\": 4"), std::string::npos) << text;
    EXPECT_NE(text.find("\"version\": 3"), std::string::npos) << text; // this version's format

    // Loading a file without them forgets them.
    write_file(path, R"({"version": 3})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    EXPECT_EQ(read_file(path).find("future"), std::string::npos);
}

// A migration never overwrites a key that's already there - the shape of
// a newer file re-saved by an older version, which writes its own older
// key beside the newer one it kept. The newer one wins.
TEST_F(ApiFixture, SettingsMigrationPrefersANewerKeyAlreadyPresent)
{
    const std::string path = le::test::scratch_path("le_settings_both_label_keys.json");
    write_file(path, R"({"version": 1, "label_size_px": 30, "label_max_size_px": 18})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_DOUBLE_EQ(read().label_max_size(), 18.0);
}

// The hierarchy depth is a per-session view choice: a v2 file's saved depth
// is dropped on load, so it isn't restored or written back, and changing
// the depth leaves the settings saved.
TEST_F(ApiFixture, SettingsDropASavedHierarchyDepth)
{
    const std::string path = le::test::scratch_path("le_settings_v2_depth.json");
    write_file(path, R"({"version": 2, "hierarchy_depth": 3, "flightline_max_fanout": 7})");
    ASSERT_TRUE(write().load_settings(path.c_str()).has_value());
    EXPECT_EQ(read().hierarchy_depth(), 0);
    EXPECT_EQ(read().flightline_max_fanout(), 7);
    write().set_hierarchy_depth(2);
    EXPECT_EQ(read().has_unsaved_settings(), 0);
    ASSERT_TRUE(write().save_settings(path.c_str()).has_value());
    EXPECT_EQ(read_file(path).find("hierarchy_depth"), std::string::npos) << read_file(path);
}

// --- Placement.type / Route.use filters ---
// TESTCELL (CLASS CORE, 10x10 um) placed at (0,0) and a POWER route rect
// (12,2)-(18,4) um on M1, viewed at 0.005 px/dbu over 100x100 px: device
// (x, y) is dbu (200 x, 200 (100 - y)).
namespace
{
    constexpr int32_t kOnPlacement[2] = {25, 75}; // (5,5) um
    constexpr int32_t kOnRoute[2] = {75, 85};     // (15,3) um

    void load_filter_layout(le::test::SessionHelpers &s)
    {
        ASSERT_TRUE(s.read_lef(fixture_path("testcell.lef"), "testcell"));
        const le::test::SessionTest::DesignInfo testcell_design = s.library_design_at(0, 0);
        const le::LibraryId top_library = s.write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
        const le::DesignId top_design = s.write().create_design({.library = top_library, .name = "TOP"}).value_or(le::DesignId{});
        const le::LayoutId layout = s.write().create_layout({.design = top_design}).value_or(le::LayoutId{});
        s.create_placement(layout, testcell_design.id, "U1", le::PointUm{0.0, 0.0}, le::Orientation::N, le::PlacementStatus::PLACED);
        const le::RouteId route = s.write().create_route({.layout = layout, .name = "VDD", .use = le::SignalUse::POWER}).value_or(le::RouteId{});
        const le::LayerId m1 = named_layer(s, "M1");
        const le::Rect rect = s.units().to_dbu(le::RectUm{{12.0, 2.0}, {18.0, 4.0}});
        ASSERT_TRUE(s.write().create_shape({.owner = le::ShapeOwner::route(route),
                                            .layer = m1,
                                            .rects = {rect},
                                            .routing_status = le::RoutingStatus::FIXED,
                                            .route_shape = le::RouteShape::STRIPE,
                                            .is_special = true}));
        ASSERT_TRUE(s.open_layout(top_design));
        s.write().set_viewport_size(100, 100);
        s.write().zoom(0.005 - 1.0, 0, 100);
    }

    // The class of what a click at `at` selects; empty if nothing.
    std::string click_kind(le::test::SessionHelpers &s, const int32_t at[2])
    {
        s.write().clear_selection();
        s.write().mouse_down(at[0], at[1]);
        s.write().mouse_up(at[0], at[1]);
        return s.selection_count() == 0 ? std::string() : std::string(le::object_kind_name(*s.selected(0)));
    }

    // Kinds selected by a drag over the whole (0,0)-(20,20) um view.
    std::multiset<std::string> drag_all_kinds(le::test::SessionHelpers &s)
    {
        s.write().clear_selection();
        s.write().mouse_down(0, 100);
        s.write().mouse_up(100, 0);
        std::multiset<std::string> kinds;
        for (int32_t i = 0; i < s.selection_count(); ++i)
            kinds.insert(std::string(le::object_kind_name(*s.selected(i))));
        return kinds;
    }
}

TEST_F(ApiFixture, ObjectFilterValuesListPlacementTypesAndRouteUses)
{
    load_filter_layout(*this);
    const auto values = [&](le::ObjectFilter filter)
    {
        const le::ReadView view = read();
        const std::span<const std::string> found = view.object_filter_values(filter);
        return std::vector<std::string>(found.begin(), found.end());
    };

    const std::vector<std::string> uses = values(le::ObjectFilter::ROUTE_USE);
    ASSERT_GT(uses.size(), 1u);
    EXPECT_EQ(uses.front(), "SIGNAL");
    EXPECT_EQ(uses.back(), "RESET");

    // TESTCELL is CORE; TOP has no Abstract.
    EXPECT_EQ(values(le::ObjectFilter::PLACEMENT_TYPE), (std::vector<std::string>{"CORE", "UNSET"}));
}

TEST_F(ApiFixture, ObjectFilterValuesListStatusesAndShapes)
{
    const auto values = [&](le::ObjectFilter filter)
    {
        const le::ReadView view = read();
        const std::span<const std::string> found = view.object_filter_values(filter);
        return std::vector<std::string>(found.begin(), found.end());
    };
    EXPECT_EQ(values(le::ObjectFilter::PLACEMENT_STATUS), (std::vector<std::string>{"UNPLACED", "PLACED", "FIXED", "COVER", "SOFTFIXED"}));
    EXPECT_EQ(values(le::ObjectFilter::ROUTING_STATUS), (std::vector<std::string>{"ROUTED", "FIXED", "COVER", "NOSHIELD", "SHIELD"}));
    const std::vector<std::string> shapes = values(le::ObjectFilter::ROUTE_SHAPE);
    ASSERT_EQ(shapes.size(), 13u);
    EXPECT_EQ(shapes.front(), "RING");
    EXPECT_EQ(shapes.back(), "UNSET");
}

TEST_F(ApiFixture, AHiddenOrUnselectableRoutingStatusOrShapeCannotBeClickedOrDragSelected)
{
    load_filter_layout(*this);
    ASSERT_EQ(click_kind(*this, kOnRoute), "shape");

    write().set_object_filter_visible(le::ObjectFilter::ROUTING_STATUS, "ROUTED", 0); // the route shape is FIXED
    write().set_object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "UNSET", 0);
    EXPECT_EQ(click_kind(*this, kOnRoute), "shape");

    write().set_object_filter_selectable(le::ObjectFilter::ROUTING_STATUS, "FIXED", 0);
    EXPECT_EQ(click_kind(*this, kOnRoute), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"placement"});

    write().set_object_filter_selectable(le::ObjectFilter::ROUTING_STATUS, "FIXED", 1);
    write().set_object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "STRIPE", 0);
    EXPECT_EQ(click_kind(*this, kOnRoute), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"placement"});
}

TEST_F(ApiFixture, AHiddenOrUnselectablePlacementStatusCannotBeClickedOrDragSelected)
{
    load_filter_layout(*this);
    ASSERT_EQ(click_kind(*this, kOnPlacement), "placement");

    write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "FIXED", 0); // U1 is PLACED
    EXPECT_EQ(click_kind(*this, kOnPlacement), "placement");

    write().set_object_filter_selectable(le::ObjectFilter::PLACEMENT_STATUS, "PLACED", 0);
    EXPECT_EQ(click_kind(*this, kOnPlacement), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"shape"});
}

TEST_F(ApiFixture, HidingARoutingStatusRemovesItsShapesFromTheRender)
{
    load_filter_layout(*this);
    le::RenderedFrame shown = render();
    ASSERT_NE(shown.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(shown, 62, 82, 88, 88)); // inside the route rect

    write().set_object_filter_visible(le::ObjectFilter::ROUTING_STATUS, "fixed", 0);
    le::RenderedFrame hidden = render();
    ASSERT_NE(hidden.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(hidden, 62, 82, 88, 88));
}

TEST_F(ApiFixture, ObjectFilterValuesAreVisibleAndSelectableByDefaultAndMatchCaseInsensitively)
{
    EXPECT_EQ(read().object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "CORE"), 1);
    EXPECT_EQ(read().object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER"), 1);
    write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "core", 0);
    EXPECT_EQ(read().object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "CORE"), 0);
    EXPECT_EQ(read().object_filter_visible(le::ObjectFilter::ROUTE_USE, "CORE"), 1); // the other filter
    write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "CORE", 1);
    EXPECT_EQ(read().object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "core"), 1);

}

TEST_F(ApiFixture, AHiddenOrUnselectablePlacementTypeCannotBeClickedOrDragSelected)
{
    load_filter_layout(*this);
    ASSERT_EQ(click_kind(*this, kOnPlacement), "placement");

    write().set_object_filter_selectable(le::ObjectFilter::PLACEMENT_TYPE, "CORE", 0);
    EXPECT_EQ(click_kind(*this, kOnPlacement), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"shape"}); // the route piece only

    write().set_object_filter_selectable(le::ObjectFilter::PLACEMENT_TYPE, "CORE", 1);
    write().set_object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "CORE", 0);
    EXPECT_EQ(click_kind(*this, kOnPlacement), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"shape"});
}

TEST_F(ApiFixture, AHiddenOrUnselectableRouteUseCannotBeClickedOrDragSelected)
{
    load_filter_layout(*this);
    ASSERT_EQ(click_kind(*this, kOnRoute), "shape");

    write().set_object_filter_visible(le::ObjectFilter::ROUTE_USE, "SIGNAL", 0); // not this route's use
    EXPECT_EQ(click_kind(*this, kOnRoute), "shape");

    write().set_object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER", 0);
    EXPECT_EQ(click_kind(*this, kOnRoute), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"placement"});

    write().set_object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER", 1);
    write().set_object_filter_visible(le::ObjectFilter::ROUTE_USE, "POWER", 0);
    EXPECT_EQ(click_kind(*this, kOnRoute), "");
    EXPECT_EQ(drag_all_kinds(*this), std::multiset<std::string>{"placement"});
}

TEST_F(ApiFixture, HidingARouteUseRemovesItsShapesFromTheRender)
{
    load_filter_layout(*this);
    le::RenderedFrame shown = render();
    ASSERT_NE(shown.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(shown, 62, 82, 88, 88)); // inside the route rect

    write().set_object_filter_visible(le::ObjectFilter::ROUTE_USE, "POWER", 0);
    le::RenderedFrame hidden = render();
    ASSERT_NE(hidden.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(hidden, 62, 82, 88, 88));
}
