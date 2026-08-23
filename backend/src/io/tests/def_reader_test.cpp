#include <gtest/gtest.h>
#include "../def_reader.hpp"

namespace le
{
    namespace
    {
        std::string complete_fixture_path()
        {
            return std::string(DEF_TEST_DIR) + "/complete.5.8.def";
        }
    }

    class DEFReaderCompleteFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // complete.5.8.def references real layer/macro names
            // (TRACKS/VIAS/PINS/BLOCKAGES/NETS/SPECIALNETS LAYER M1/M2/M3/
            // METAL1/V1; COMPONENTS macro A/B/CHK3A) but - being the
            // vendored parser's own grammar-coverage fixture, not a real
            // design - has no companion LEF of its own. Shape.layer/
            // Placement.reference_design both require an already-resolved
            // reference now (readers error and skip rather than create an
            // unresolved one - see their own schema.py comments), matching
            // the real-world requirement that a tech/cell LEF is always
            // read before the DEF that references it - so this fixture
            // pre-populates exactly what complete.5.8.def needs, the same
            // role a real LEF read would otherwise play.
            const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            for (const char *name : {"M1", "M2", "M3", "METAL1", "V1"})
                root.create_layer(LayerData{.technology = technology_id, .name = name, .type = "ROUTING"});
            const LibraryId library_id = root.create_library(LibraryData{.name = "test_lib"});
            for (const char *name : {"A", "B", "CHK3A"})
                root.create_design(DesignData{.library = library_id, .name = name});

            result = reader.read_def(complete_fixture_path(), root, "test_lib");
        }

        Root root;
        DEFReader reader;
        int result = -1;
    };

    TEST_F(DEFReaderCompleteFixture, ParsesSuccessfully)
    {
        EXPECT_EQ(result, 0);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesTheDesignAndItsLayout)
    {
        const DesignId design_id = root.get_design_by_name("design");
        ASSERT_TRUE(design_id.valid());
        const LayoutId layout_id = root.get_design_layout(design_id);
        EXPECT_TRUE(layout_id.valid());
    }

    TEST_F(DEFReaderCompleteFixture, DieAreaBecomesABoundaryShapeWithEveryPoint)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id));
        ASSERT_NE(diearea, nullptr);
        ASSERT_TRUE(diearea->purpose.has_value());
        EXPECT_EQ(*diearea->purpose, ShapePurpose::BOUNDARY);
        ASSERT_EQ(diearea->polygons.size(), 1u);
        // DIEAREA ( -190000 -120000 ) ( -190000 350000 ) ( 190000 350000 )
        //         ( 190000 190000 ) ( 190360 190000 ) ( 190360 -120000 ) ;
        const Polygon &polygon = diearea->polygons.front();
        ASSERT_EQ(polygon.points.size(), 6u);
        EXPECT_EQ(polygon.points[0].x, -190000);
        EXPECT_EQ(polygon.points[0].y, -120000);
        EXPECT_EQ(polygon.points[5].x, 190360);
        EXPECT_EQ(polygon.points[5].y, -120000);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryRowWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<RowId> row_ids = root.get_layout_rows(layout_id);
        ASSERT_EQ(row_ids.size(), 24u);

        const RowData *row1 = nullptr;
        const RowData *row2 = nullptr;
        for (const RowId id : row_ids)
        {
            const RowData *row = root.get_row(id);
            ASSERT_NE(row, nullptr);
            if (row->name == "ROW_1")
                row1 = row;
            else if (row->name == "ROW_2")
                row2 = row;
        }

        ASSERT_NE(row1, nullptr);
        EXPECT_EQ(row1->site_name, "CORE");
        ASSERT_TRUE(row1->origin.has_value());
        EXPECT_EQ(row1->origin->x, 1000);
        EXPECT_EQ(row1->origin->y, 1000);
        EXPECT_EQ(row1->orientation, Orientation::N);
        ASSERT_TRUE(row1->num_x.has_value());
        ASSERT_TRUE(row1->num_y.has_value());
        EXPECT_EQ(*row1->num_x, 100);
        EXPECT_EQ(*row1->num_y, 1);
        ASSERT_TRUE(row1->step_x.has_value());
        ASSERT_TRUE(row1->step_y.has_value());
        EXPECT_EQ(*row1->step_x, 700);
        EXPECT_EQ(*row1->step_y, 0);

        ASSERT_NE(row2, nullptr);
        EXPECT_EQ(row2->orientation, Orientation::S);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryTrackWithLayersAndMasks)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<TrackId> track_ids = root.get_layout_tracks(layout_id);
        // TRACKS Y 52 DO 857 STEP 104 MASK 1 ;
        // TRACKS Y 52 DO 857 STEP 104 MASK 1 SAMEMASK LAYER M1 M2 ;
        // TRACKS X 52 DO 1720 STEP 104 MASK 2 LAYER M2 ;
        // TRACKS X 52 DO 1720 STEP 104 ;
        ASSERT_EQ(track_ids.size(), 4u);

        const TrackData *first = root.get_track(track_ids[0]);
        ASSERT_NE(first, nullptr);
        EXPECT_FALSE(first->is_x);
        EXPECT_EQ(first->start, 52);
        EXPECT_EQ(first->count, 857);
        EXPECT_EQ(first->step, 104);
        ASSERT_TRUE(first->mask.has_value());
        EXPECT_EQ(*first->mask, 1);
        EXPECT_FALSE(first->same_mask);

        const TrackData *second = root.get_track(track_ids[1]);
        ASSERT_NE(second, nullptr);
        EXPECT_TRUE(second->same_mask);
        ASSERT_EQ(second->layer_names.size(), 2u);
        EXPECT_EQ(second->layer_names[0], "M1");
        EXPECT_EQ(second->layer_names[1], "M2");

        const TrackData *last = root.get_track(track_ids[3]);
        ASSERT_NE(last, nullptr);
        EXPECT_TRUE(last->is_x);
        EXPECT_FALSE(last->mask.has_value());
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryGcellGrid)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<GCellGridId> grid_ids = root.get_layout_gcell_grids(layout_id);
        // GCELLGRID X 0 DO 100 STEP 600 ;
        // GCELLGRID Y 10 DO 120 STEP 400 ;
        ASSERT_EQ(grid_ids.size(), 2u);

        const GCellGridData *x_grid = root.get_g_cell_grid(grid_ids[0]);
        ASSERT_NE(x_grid, nullptr);
        EXPECT_TRUE(x_grid->is_x);
        EXPECT_EQ(x_grid->start, 0);
        EXPECT_EQ(x_grid->count, 100);
        EXPECT_EQ(x_grid->step, 600);

        const GCellGridData *y_grid = root.get_g_cell_grid(grid_ids[1]);
        ASSERT_NE(y_grid, nullptr);
        EXPECT_FALSE(y_grid->is_x);
        EXPECT_EQ(y_grid->start, 10);
        EXPECT_EQ(y_grid->count, 120);
        EXPECT_EQ(y_grid->step, 400);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryPlacementWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<PlacementId> placement_ids = root.get_layout_placements(layout_id);
        // COMPONENTS declares a stale count of 13, but the fixture actually
        // has 43 real "- ..." entries (mostly bracket/escaping edge cases
        // for name parsing) - the vendored parser doesn't enforce the
        // declared count.
        ASSERT_EQ(placement_ids.size(), 43u);

        auto find_by_name = [&](const std::string &name) -> const PlacementData *
        {
            for (const PlacementId id : placement_ids)
            {
                const PlacementData *placement = root.get_placement(id);
                if (placement && placement->name == name)
                    return placement;
            }
            return nullptr;
        };

        const PlacementData *i1 = find_by_name("I1");
        ASSERT_NE(i1, nullptr);
        ASSERT_TRUE(i1->reference_design.valid());
        EXPECT_EQ(root.get_design(i1->reference_design)->name, "B");
        EXPECT_EQ(i1->placement_status, PlacementStatus::PLACED);
        ASSERT_TRUE(i1->location.has_value());
        EXPECT_EQ(i1->location->x, 100);
        EXPECT_EQ(i1->location->y, 100);
        ASSERT_TRUE(i1->orientation.has_value());
        EXPECT_EQ(*i1->orientation, Orientation::N);
        ASSERT_TRUE(i1->weight.has_value());
        EXPECT_EQ(*i1->weight, 100.0);
        ASSERT_TRUE(i1->source.has_value());
        EXPECT_EQ(*i1->source, "NETLIST");

        const PlacementData *i2 = find_by_name("I2");
        ASSERT_NE(i2, nullptr);
        ASSERT_TRUE(i2->reference_design.valid());
        EXPECT_EQ(root.get_design(i2->reference_design)->name, "A");
        ASSERT_TRUE(i2->orientation.has_value());
        EXPECT_EQ(*i2->orientation, Orientation::S);
        ASSERT_TRUE(i2->source.has_value());
        EXPECT_EQ(*i2->source, "DIST");

        const PlacementData *i9 = find_by_name("I9");
        ASSERT_NE(i9, nullptr);
        EXPECT_EQ(i9->placement_status, PlacementStatus::FIXED);

        const PlacementData *i10 = find_by_name("I10");
        ASSERT_NE(i10, nullptr);
        EXPECT_EQ(i10->placement_status, PlacementStatus::COVER);

        const PlacementData *i11 = find_by_name("I11");
        ASSERT_NE(i11, nullptr);
        EXPECT_EQ(i11->placement_status, PlacementStatus::UNPLACED);
        EXPECT_FALSE(i11->location.has_value());
        EXPECT_FALSE(i11->orientation.has_value());

        // Bracket/escaping edge cases in instance names parse without
        // crashing or truncating - a handful of representative ones.
        EXPECT_NE(find_by_name("I12[0]"), nullptr);
        EXPECT_NE(find_by_name("I13[0][10]"), nullptr);
        EXPECT_NE(find_by_name("I14\\[1\\]"), nullptr);
        EXPECT_NE(find_by_name("vectormodule[1]/scalarname"), nullptr);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryPhysicalPortWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<PhysicalPortId> port_ids = root.get_layout_physical_ports(layout_id);
        // PINS declares a stale count of 11 (same kind of mismatch as
        // COMPONENTS' own stale "13") - the fixture actually has 19 real
        // "- ..." entries.
        ASSERT_EQ(port_ids.size(), 19u);

        auto find_id_by_name = [&](const std::string &name) -> PhysicalPortId
        {
            for (const PhysicalPortId id : port_ids)
            {
                const PhysicalPortData *port = root.get_physical_port(id);
                if (port && port->name == name)
                    return id;
            }
            return PhysicalPortId{};
        };

        // P0: 5.7+ multi-port pin - 3 PORTs, each independently placed
        // (FIXED/COVER/PLACED at 3 different locations) with its own
        // single-layer rect. Note: each PORT also has a "+ VIA ..." entry
        // this round doesn't model yet (via placements standalone within
        // a PORT, not nested under a "+ LAYER" the way Shape.vias
        // represents a VIA within a LEF PIN/OBS geometry stream - a
        // different shape needing its own schema support later).
        const PhysicalPortId p0_id = find_id_by_name("P0");
        ASSERT_TRUE(p0_id.valid());
        const PhysicalPortData *p0 = root.get_physical_port(p0_id);
        ASSERT_NE(p0, nullptr);
        ASSERT_TRUE(p0->net_name.has_value());
        EXPECT_EQ(*p0->net_name, "N0");
        ASSERT_TRUE(p0->direction.has_value());
        EXPECT_EQ(*p0->direction, SignalDirection::INPUT);
        ASSERT_TRUE(p0->use.has_value());
        EXPECT_EQ(*p0->use, "SIGNAL");
        // No top-level placement statement - it's all per-PORT.
        EXPECT_FALSE(p0->location.has_value());

        const std::vector<PhysicalPortSegmentId> p0_segments = root.get_physical_port_segments(p0_id);
        ASSERT_EQ(p0_segments.size(), 3u);

        const PhysicalPortSegmentData *segment0 = root.get_physical_port_segment(p0_segments[0]);
        ASSERT_NE(segment0, nullptr);
        ASSERT_TRUE(segment0->placement_status.has_value());
        EXPECT_EQ(*segment0->placement_status, PlacementStatus::FIXED);
        ASSERT_TRUE(segment0->location.has_value());
        EXPECT_EQ(segment0->location->x, 45);
        EXPECT_EQ(segment0->location->y, -2160);
        ASSERT_TRUE(segment0->orientation.has_value());
        EXPECT_EQ(*segment0->orientation, Orientation::N);
        const std::vector<ShapeId> segment0_shape_ids = root.get_physical_port_segment_shapes(p0_segments[0]);
        ASSERT_EQ(segment0_shape_ids.size(), 1u);
        const Shape *segment0_shape = root.get_shape(segment0_shape_ids[0]);
        ASSERT_NE(segment0_shape, nullptr);
        EXPECT_EQ(root.get_layer(segment0_shape->layer)->name, "M2");
        ASSERT_EQ(segment0_shape->rects.size(), 1u);
        EXPECT_EQ(segment0_shape->rects[0].ll.x, 0);
        EXPECT_EQ(segment0_shape->rects[0].ll.y, 0);
        EXPECT_EQ(segment0_shape->rects[0].ur.x, 30);
        EXPECT_EQ(segment0_shape->rects[0].ur.y, 135);

        const PhysicalPortSegmentData *segment1 = root.get_physical_port_segment(p0_segments[1]);
        ASSERT_NE(segment1, nullptr);
        ASSERT_TRUE(segment1->placement_status.has_value());
        EXPECT_EQ(*segment1->placement_status, PlacementStatus::COVER);

        const PhysicalPortSegmentData *segment2 = root.get_physical_port_segment(p0_segments[2]);
        ASSERT_NE(segment2, nullptr);
        ASSERT_TRUE(segment2->placement_status.has_value());
        EXPECT_EQ(*segment2->placement_status, PlacementStatus::PLACED);

        // P1: simple (no PORT wrapper) pin with POLYGON geometry directly
        // attached and its own top-level placement.
        const PhysicalPortId p1_id = find_id_by_name("P1");
        ASSERT_TRUE(p1_id.valid());
        const PhysicalPortData *p1 = root.get_physical_port(p1_id);
        ASSERT_NE(p1, nullptr);
        ASSERT_TRUE(p1->direction.has_value());
        EXPECT_EQ(*p1->direction, SignalDirection::OUTPUT);
        ASSERT_TRUE(p1->use.has_value());
        EXPECT_EQ(*p1->use, "POWER");
        ASSERT_TRUE(p1->placement_status.has_value());
        EXPECT_EQ(*p1->placement_status, PlacementStatus::PLACED);
        ASSERT_TRUE(p1->location.has_value());
        EXPECT_EQ(p1->location->x, 45);
        EXPECT_EQ(p1->location->y, -2160);

        const std::vector<PhysicalPortSegmentId> p1_segments = root.get_physical_port_segments(p1_id);
        ASSERT_EQ(p1_segments.size(), 1u);
        // The synthetic segment for a simple pin has no placement of its
        // own - it lives on the parent PhysicalPort instead.
        EXPECT_FALSE(root.get_physical_port_segment(p1_segments[0])->placement_status.has_value());
        const std::vector<ShapeId> p1_shape_ids = root.get_physical_port_segment_shapes(p1_segments[0]);
        ASSERT_EQ(p1_shape_ids.size(), 1u);
        const Shape *p1_shape = root.get_shape(p1_shape_ids[0]);
        ASSERT_NE(p1_shape, nullptr);
        EXPECT_EQ(root.get_layer(p1_shape->layer)->name, "M2");
        ASSERT_EQ(p1_shape->polygons.size(), 1u);
        EXPECT_EQ(p1_shape->polygons[0].points.size(), 6u);

        // P2/P2.extra1: DIRECTION INOUT, USE GROUND, simple LAYER rect,
        // COVER placement - and a name with a literal "." in it (a real
        // DEF-legal identifier character), confirming name parsing
        // doesn't misinterpret it as a hierarchy separator.
        const PhysicalPortData *p2 = root.get_physical_port(find_id_by_name("P2"));
        ASSERT_NE(p2, nullptr);
        ASSERT_TRUE(p2->direction.has_value());
        EXPECT_EQ(*p2->direction, SignalDirection::INOUT);
        ASSERT_TRUE(p2->placement_status.has_value());
        EXPECT_EQ(*p2->placement_status, PlacementStatus::COVER);
        EXPECT_TRUE(find_id_by_name("P2.extra1").valid());

        // P3: DIRECTION FEEDTHRU, USE CLOCK, no geometry and no placement
        // statement at all - placement_status defaults to UNPLACED
        // (mirrors Placement's own no-statement-at-all handling), and the
        // one synthetic segment simply has no shapes.
        const PhysicalPortId p3_id = find_id_by_name("P3");
        ASSERT_TRUE(p3_id.valid());
        const PhysicalPortData *p3 = root.get_physical_port(p3_id);
        ASSERT_NE(p3, nullptr);
        ASSERT_TRUE(p3->direction.has_value());
        EXPECT_EQ(*p3->direction, SignalDirection::FEEDTHRU);
        ASSERT_TRUE(p3->use.has_value());
        EXPECT_EQ(*p3->use, "CLOCK");
        ASSERT_TRUE(p3->placement_status.has_value());
        EXPECT_EQ(*p3->placement_status, PlacementStatus::UNPLACED);
        EXPECT_FALSE(p3->location.has_value());
        const std::vector<PhysicalPortSegmentId> p3_segments = root.get_physical_port_segments(p3_id);
        ASSERT_EQ(p3_segments.size(), 1u);
        EXPECT_EQ(root.get_physical_port_segment_shapes(p3_segments[0]).size(), 0u);

        // Bracket/angle-bracket edge cases in pin names parse without
        // crashing or truncating.
        EXPECT_TRUE(find_id_by_name("ARRAYPIN[0][10]").valid());
        EXPECT_TRUE(find_id_by_name("OUTBUS<1>").valid());
        EXPECT_TRUE(find_id_by_name("vectorpin[0]").valid());
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryBlockageWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<BlockageId> blockage_ids = root.get_layout_blockages(layout_id);
        // BLOCKAGES declares a stale count of 8 (same stale-count pattern
        // as COMPONENTS/PINS) - the fixture actually has 11 real entries
        // (6 LAYER, 5 PLACEMENT), in file order.
        ASSERT_EQ(blockage_ids.size(), 11u);

        auto shapes_of = [&](BlockageId id) -> std::vector<const Shape *>
        {
            std::vector<const Shape *> result;
            for (const ShapeId shape_id : root.get_blockage_shapes(id))
                if (const Shape *shape = root.get_shape(shape_id))
                    result.push_back(shape);
            return result;
        };

        // 1st: LAYER METAL1, single RECT, no component/spacing/width.
        const BlockageData *first = root.get_blockage(blockage_ids[0]);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->kind, BlockageKind::ROUTING);
        ASSERT_TRUE(first->layer_name.has_value());
        EXPECT_EQ(*first->layer_name, "METAL1");
        EXPECT_FALSE(first->placement.valid());
        EXPECT_FALSE(first->spacing.has_value());
        EXPECT_FALSE(first->design_rule_width.has_value());
        EXPECT_FALSE(first->is_soft);
        const std::vector<const Shape *> first_shapes = shapes_of(blockage_ids[0]);
        ASSERT_EQ(first_shapes.size(), 1u);
        ASSERT_EQ(first_shapes[0]->rects.size(), 1u);
        EXPECT_EQ(first_shapes[0]->rects[0].ll.x, 60);
        EXPECT_EQ(first_shapes[0]->rects[0].ur.y, 90);

        // 2nd: LAYER M2 + COMPONENT I1, POLYGON geometry (6 points).
        const BlockageData *second = root.get_blockage(blockage_ids[1]);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(second->layer_name.has_value());
        EXPECT_EQ(*second->layer_name, "M2");
        ASSERT_TRUE(second->placement.valid());
        const PlacementData *second_placement = root.get_placement(second->placement);
        ASSERT_NE(second_placement, nullptr);
        EXPECT_EQ(second_placement->name, "I1");
        const std::vector<const Shape *> second_shapes = shapes_of(blockage_ids[1]);
        ASSERT_EQ(second_shapes.size(), 1u);
        ASSERT_EQ(second_shapes[0]->polygons.size(), 1u);
        EXPECT_EQ(second_shapes[0]->polygons[0].points.size(), 6u);

        // 5th: LAYER M1 + SPACING 3.
        const BlockageData *fifth = root.get_blockage(blockage_ids[4]);
        ASSERT_NE(fifth, nullptr);
        ASSERT_TRUE(fifth->spacing.has_value());
        EXPECT_EQ(*fifth->spacing, 3);
        EXPECT_FALSE(fifth->design_rule_width.has_value());

        // 6th: LAYER M1 + DESIGNRULEWIDTH 45.
        const BlockageData *sixth = root.get_blockage(blockage_ids[5]);
        ASSERT_NE(sixth, nullptr);
        ASSERT_TRUE(sixth->design_rule_width.has_value());
        EXPECT_EQ(*sixth->design_rule_width, 45);
        EXPECT_FALSE(sixth->spacing.has_value());

        // 7th: bare PLACEMENT, 4 RECTs, no component/density/soft.
        const BlockageData *seventh = root.get_blockage(blockage_ids[6]);
        ASSERT_NE(seventh, nullptr);
        EXPECT_EQ(seventh->kind, BlockageKind::PLACEMENT);
        EXPECT_FALSE(seventh->layer_name.has_value());
        EXPECT_FALSE(seventh->placement.valid());
        EXPECT_FALSE(seventh->is_soft);
        EXPECT_FALSE(seventh->placement_max_density.has_value());
        EXPECT_EQ(shapes_of(blockage_ids[6])[0]->rects.size(), 4u);

        // 8th: PLACEMENT + PARTIAL 0.40 + COMPONENT I1.
        const BlockageData *eighth = root.get_blockage(blockage_ids[7]);
        ASSERT_NE(eighth, nullptr);
        ASSERT_TRUE(eighth->placement.valid());
        EXPECT_EQ(root.get_placement(eighth->placement)->name, "I1");
        ASSERT_TRUE(eighth->placement_max_density.has_value());
        EXPECT_DOUBLE_EQ(*eighth->placement_max_density, 0.40);

        // 10th: PLACEMENT + SOFT.
        const BlockageData *tenth = root.get_blockage(blockage_ids[9]);
        ASSERT_NE(tenth, nullptr);
        EXPECT_TRUE(tenth->is_soft);

        // 11th: PLACEMENT + PARTIAL 0.40 (no COMPONENT).
        const BlockageData *eleventh = root.get_blockage(blockage_ids[10]);
        ASSERT_NE(eleventh, nullptr);
        ASSERT_TRUE(eleventh->placement_max_density.has_value());
        EXPECT_DOUBLE_EQ(*eleventh->placement_max_density, 0.40);
        EXPECT_FALSE(eleventh->placement.valid());
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryLayoutViaWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<LayoutViaId> via_ids = root.get_layout_vias(layout_id);
        ASSERT_EQ(via_ids.size(), 11u);

        auto find_id_by_name = [&](const std::string &name) -> LayoutViaId
        {
            for (const LayoutViaId id : via_ids)
            {
                const LayoutViaData *via = root.get_layout_via(id);
                if (via && via->name == name)
                    return id;
            }
            return LayoutViaId{};
        };
        auto find_layer = [&](LayoutViaId id, const std::string &layer_name) -> const ViaLayerData *
        {
            for (const ViaLayerId layer_id : root.get_layout_via_layers(id))
                if (const ViaLayerData *layer = root.get_via_layer(layer_id); layer && layer->layer_name == layer_name)
                    return layer;
            return nullptr;
        };

        // VIAGEN12_0: 4 RECT statements on V1 alone, grouped into one
        // ViaLayer (not 4 separate ones) - same grouping idiom as
        // shapes_from_pin_like.
        const LayoutViaId viagen0_id = find_id_by_name("VIAGEN12_0");
        ASSERT_TRUE(viagen0_id.valid());
        ASSERT_EQ(root.get_layout_via_layers(viagen0_id).size(), 3u);
        const ViaLayerData *viagen0_v1 = find_layer(viagen0_id, "V1");
        ASSERT_NE(viagen0_v1, nullptr);
        EXPECT_EQ(viagen0_v1->rects.size(), 4u);
        const ViaLayerData *viagen0_metal1 = find_layer(viagen0_id, "METAL1");
        ASSERT_NE(viagen0_metal1, nullptr);
        ASSERT_EQ(viagen0_metal1->rects.size(), 1u);
        EXPECT_EQ(viagen0_metal1->rects[0].ll.x, -4400);
        EXPECT_EQ(viagen0_metal1->rects[0].ur.y, 3800);

        // VIAGEN12_4: VIARULE-generated, no RECT layers of its own.
        const LayoutViaId viagen4_id = find_id_by_name("VIAGEN12_4");
        ASSERT_TRUE(viagen4_id.valid());
        EXPECT_EQ(root.get_layout_via_layers(viagen4_id).size(), 0u);
        const ViaRuleReferenceId via_rule_id = root.get_layout_via_via_rule(viagen4_id);
        ASSERT_TRUE(via_rule_id.valid());
        const ViaRuleReferenceData *via_rule = root.get_via_rule_reference(via_rule_id);
        ASSERT_NE(via_rule, nullptr);
        EXPECT_EQ(via_rule->via_rule_name, "VIAGEN12");
        ASSERT_TRUE(via_rule->cut_size.has_value());
        EXPECT_EQ(via_rule->cut_size->x, 1600);
        EXPECT_EQ(via_rule->cut_size->y, 1600);
        EXPECT_EQ(via_rule->bot_layer_name, "M1");
        EXPECT_EQ(via_rule->cut_layer_name, "V1");
        EXPECT_EQ(via_rule->top_layer_name, "M2");
        ASSERT_TRUE(via_rule->cut_spacing.has_value());
        EXPECT_EQ(via_rule->cut_spacing->x, 5600);
        EXPECT_EQ(via_rule->cut_spacing->y, 6100);
        ASSERT_TRUE(via_rule->bot_enclosure.has_value());
        EXPECT_EQ(via_rule->bot_enclosure->x, 100);
        ASSERT_TRUE(via_rule->top_enclosure.has_value());
        EXPECT_EQ(via_rule->top_enclosure->x, 150);

        // VIAGEN12_1: one POLYGON layer plus two RECT layers.
        const LayoutViaId viagen1_id = find_id_by_name("VIAGEN12_1");
        ASSERT_TRUE(viagen1_id.valid());
        const ViaLayerData *viagen1_metal1 = find_layer(viagen1_id, "METAL1");
        ASSERT_NE(viagen1_metal1, nullptr);
        ASSERT_EQ(viagen1_metal1->polygons.size(), 1u);
        EXPECT_EQ(viagen1_metal1->polygons[0].points.size(), 6u);
        EXPECT_EQ(viagen1_metal1->rects.size(), 0u);
        ASSERT_NE(find_layer(viagen1_id, "M2"), nullptr);
        ASSERT_NE(find_layer(viagen1_id, "V1"), nullptr);

        // CUSTOMVIA: POLYGON only, no via_rule.
        const LayoutViaId custom_id = find_id_by_name("CUSTOMVIA");
        ASSERT_TRUE(custom_id.valid());
        ASSERT_EQ(root.get_layout_via_layers(custom_id).size(), 1u);
        EXPECT_FALSE(root.get_layout_via_via_rule(custom_id).valid());

        // myvia1: 3 RECT layers, no via_rule.
        const LayoutViaId myvia1_id = find_id_by_name("myvia1");
        ASSERT_TRUE(myvia1_id.valid());
        ASSERT_EQ(root.get_layout_via_layers(myvia1_id).size(), 3u);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryRegionWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<RegionId> region_ids = root.get_layout_regions(layout_id);
        ASSERT_EQ(region_ids.size(), 2u);

        auto find_by_name = [&](const std::string &name) -> const RegionData *
        {
            for (const RegionId id : region_ids)
            {
                const RegionData *region = root.get_region(id);
                if (region && region->name == name)
                    return region;
            }
            return nullptr;
        };

        // region1: 4 coordinate points = 2 rects (each a pair of opposite
        // corners), TYPE FENCE.
        const RegionData *region1 = find_by_name("region1");
        ASSERT_NE(region1, nullptr);
        ASSERT_TRUE(region1->region_type.has_value());
        EXPECT_EQ(*region1->region_type, "FENCE");
        ASSERT_EQ(region1->rects.size(), 2u);
        EXPECT_EQ(region1->rects[0].ll.x, -500);
        EXPECT_EQ(region1->rects[0].ur.x, 300);
        EXPECT_EQ(region1->rects[1].ll.x, 500);
        EXPECT_EQ(region1->rects[1].ur.x, 1000);

        // region2: 1 rect, TYPE GUIDE.
        const RegionData *region2 = find_by_name("region2");
        ASSERT_NE(region2, nullptr);
        ASSERT_TRUE(region2->region_type.has_value());
        EXPECT_EQ(*region2->region_type, "GUIDE");
        ASSERT_EQ(region2->rects.size(), 1u);
        EXPECT_EQ(region2->rects[0].ll.x, 4000);
        EXPECT_EQ(region2->rects[0].ur.y, 1000);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesRoutesWithRoutedGeometry)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<RouteId> route_ids = root.get_layout_routes(layout_id);

        auto find_id_by_name_and_kind = [&](const std::string &name, bool is_special) -> RouteId
        {
            for (const RouteId id : route_ids)
            {
                const RouteData *route = root.get_route(id);
                if (route && route->name == name && route->is_special == is_special)
                    return id;
            }
            return RouteId{};
        };
        auto shapes_of = [&](RouteId id) -> std::vector<const Shape *>
        {
            std::vector<const Shape *> result;
            for (const ShapeId shape_id : root.get_route_shapes(id))
                if (const Shape *shape = root.get_shape(shape_id))
                    result.push_back(shape);
            return result;
        };

        // DUMMY (SPECIALNETS): a single "+ ROUTED M1 100 + SHAPE FILLWIRE
        // ( 0 0 ) ( 100 0 ) ;" - one Shape on M1, one 2-point Path, width
        // 100. SHAPE FILLWIRE itself isn't modeled (no schema field).
        const RouteId dummy_id = find_id_by_name_and_kind("DUMMY", true);
        ASSERT_TRUE(dummy_id.valid());
        const RouteData *dummy = root.get_route(dummy_id);
        ASSERT_NE(dummy, nullptr);
        EXPECT_TRUE(dummy->is_special);
        const std::vector<const Shape *> dummy_shapes = shapes_of(dummy_id);
        ASSERT_EQ(dummy_shapes.size(), 1u);
        EXPECT_EQ(root.get_layer(dummy_shapes[0]->layer)->name, "M1");
        ASSERT_EQ(dummy_shapes[0]->paths.size(), 1u);
        EXPECT_EQ(dummy_shapes[0]->paths[0].width, 100);
        ASSERT_EQ(dummy_shapes[0]->paths[0].polygon.points.size(), 2u);
        EXPECT_EQ(dummy_shapes[0]->paths[0].polygon.points[0].x, 0);
        EXPECT_EQ(dummy_shapes[0]->paths[0].polygon.points[1].x, 100);

        // N6 (NETS): 3 separate top-level ROUTED statements, all on M1 -
        // grouped into one Shape (find_or_create by layer name), each its
        // own Path (3 total), no explicit WIDTH given -> defaults to 0.
        const RouteId n6_id = find_id_by_name_and_kind("N6", false);
        ASSERT_TRUE(n6_id.valid());
        EXPECT_FALSE(root.get_route(n6_id)->is_special);
        const std::vector<const Shape *> n6_shapes = shapes_of(n6_id);
        ASSERT_EQ(n6_shapes.size(), 1u);
        EXPECT_EQ(root.get_layer(n6_shapes[0]->layer)->name, "M1");
        ASSERT_EQ(n6_shapes[0]->paths.size(), 3u);
        for (const Path &path : n6_shapes[0]->paths)
        {
            EXPECT_EQ(path.width, 0);
            EXPECT_EQ(path.polygon.points.size(), 2u);
        }

        // N4 (NETS): USE GROUND, + a routed path with a VIA (VIAGEN12) in
        // the middle - confirms VIA attachment at the last point seen.
        const RouteId n4_id = find_id_by_name_and_kind("N4", false);
        ASSERT_TRUE(n4_id.valid());
        ASSERT_TRUE(root.get_route(n4_id)->use.has_value());
        EXPECT_EQ(*root.get_route(n4_id)->use, "GROUND");
        const std::vector<const Shape *> n4_shapes = shapes_of(n4_id);
        bool found_via = false;
        for (const Shape *shape : n4_shapes)
            if (!shape->vias.empty())
                found_via = true;
        EXPECT_TRUE(found_via);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryNonDefaultRuleWithCorrectFields)
    {
        // Scoped to the shared Technology (reused/created the same way
        // LEFReader does), not the Layout - find both rules by name
        // directly via Root's own by-name lookup (NonDefaultRule.name is
        // a plain index=True field, not unique_per_parent).
        const NonDefaultRuleId default_id = root.get_non_default_rule_by_name("DEFAULT");
        ASSERT_TRUE(default_id.valid());
        const NonDefaultRuleId rule2_id = root.get_non_default_rule_by_name("RULE2");
        ASSERT_TRUE(rule2_id.valid());

        const NonDefaultRuleData *default_rule = root.get_non_default_rule(default_id);
        ASSERT_NE(default_rule, nullptr);
        EXPECT_FALSE(default_rule->hard_spacing);
        const NonDefaultRuleData *rule2 = root.get_non_default_rule(rule2_id);
        ASSERT_NE(rule2, nullptr);
        EXPECT_TRUE(rule2->hard_spacing);

        // Both rules have the same layers/vias/viarules/mincuts - check
        // DEFAULT in full. WIDTH/DIAGWIDTH/SPACING/WIREEXT are written in
        // microns (10.1/8.01/2.2/1.1) - layerWidthVal()/etc. (the "Val"
        // accessors, as opposed to the obsolete plain double ones)
        // already convert to database units using the file's own UNITS
        // DISTANCE MICRONS 1000, same convention confirmed for ROW/
        // TRACKS/GCELLGRID earlier.
        ASSERT_EQ(default_rule->use_via_names.size(), 2u);
        EXPECT_EQ(default_rule->use_via_names[0], "M1_M2");
        EXPECT_EQ(default_rule->use_via_names[1], "M2_M3");
        ASSERT_EQ(default_rule->use_via_rule_names.size(), 1u);
        EXPECT_EQ(default_rule->use_via_rule_names[0], "VIAGEN12");
        ASSERT_EQ(default_rule->min_cuts.size(), 1u);
        EXPECT_EQ(default_rule->min_cuts[0].cut_layer_name, "V1");
        EXPECT_EQ(default_rule->min_cuts[0].num_cuts, 2);

        const std::vector<NonDefaultRuleLayerId> layer_ids = root.get_non_default_rule_layers(default_id);
        ASSERT_EQ(layer_ids.size(), 3u);

        auto find_layer = [&](const std::string &name) -> const NonDefaultRuleLayerData *
        {
            for (const NonDefaultRuleLayerId id : layer_ids)
                if (const NonDefaultRuleLayerData *layer = root.get_non_default_rule_layer(id); layer && layer->layer_name == name)
                    return layer;
            return nullptr;
        };

        const NonDefaultRuleLayerData *metal1 = find_layer("METAL1");
        ASSERT_NE(metal1, nullptr);
        ASSERT_TRUE(metal1->width.has_value());
        EXPECT_EQ(*metal1->width, 10100);
        ASSERT_TRUE(metal1->diag_width.has_value());
        EXPECT_EQ(*metal1->diag_width, 8010);
        ASSERT_TRUE(metal1->spacing.has_value());
        EXPECT_EQ(*metal1->spacing, 2200);
        ASSERT_TRUE(metal1->wire_extension.has_value());
        EXPECT_EQ(*metal1->wire_extension, 1100);

        const NonDefaultRuleLayerData *m2 = find_layer("M2");
        ASSERT_NE(m2, nullptr);
        ASSERT_TRUE(m2->width.has_value());
        EXPECT_EQ(*m2->width, 10100);
        EXPECT_FALSE(m2->diag_width.has_value());
        EXPECT_FALSE(m2->wire_extension.has_value());
    }

    TEST(DEFReaderErrors, FileNotFoundReturnsOne)
    {
        Root root;
        DEFReader reader;
        EXPECT_EQ(reader.read_def("/nonexistent/path.def", root, "test_lib"), 1);
    }

    // unit_scale_mismatch.def declares UNITS DISTANCE MICRONS 500 - coarser
    // than the pre-existing Technology's own 1000 (as if a LEF tech file
    // had already been read) - DEFReader must rescale every raw value it
    // reads (2x here: 1000/500) onto the Technology's actual grid rather
    // than storing them as if they were already at that scale (the
    // previous, buggy behavior - see defrUnitsCbkFn's own comment).
    TEST(DEFReaderUnitScale, RescalesGeometryWhenDefUnitsDifferFromTechnologyUnits)
    {
        Root root;
        root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        DEFReader reader;
        ASSERT_EQ(reader.read_def(std::string(IO_TEST_FIXTURES_DIR) + "/unit_scale_mismatch.def", root, "test_lib"), 0);

        const DesignId design_id = root.get_design_by_name("unit_scale_test");
        ASSERT_TRUE(design_id.valid());
        const LayoutId layout_id = root.get_design_layout(design_id);
        ASSERT_TRUE(layout_id.valid());

        const ShapeId diearea_id = root.get_layout_diearea(layout_id);
        const ShapeData *diearea = root.get_shape(diearea_id);
        ASSERT_TRUE(diearea && !diearea->polygons.empty());
        // The plain 2-corner DIEAREA shorthand goes through
        // Geometry::rect_to_polygon (a closed 5-point ring: ll, ll/ur.y,
        // ur, ur.x/ll.y, ll again) rather than staying a 2-point polygon.
        ASSERT_EQ(diearea->polygons.front().points.size(), 5u);
        EXPECT_EQ(diearea->polygons.front().points[0].x, 0);
        EXPECT_EQ(diearea->polygons.front().points[0].y, 0);
        EXPECT_EQ(diearea->polygons.front().points[2].x, 5000); // 2000 * (1000/400)
        EXPECT_EQ(diearea->polygons.front().points[2].y, 5000);

        const auto &row_ids = root.get_layout_rows(layout_id);
        ASSERT_EQ(row_ids.size(), 1u);
        const RowData *row = root.get_row(row_ids.front());
        ASSERT_TRUE(row && row->origin.has_value());
        EXPECT_EQ(row->origin->x, 250);  // 100 * 2.5
        EXPECT_EQ(row->origin->y, 500);  // 200 * 2.5
        ASSERT_TRUE(row->step_x.has_value());
        EXPECT_EQ(*row->step_x, 1000); // 400 * 2.5

        // The Technology's own shared scale must never be overwritten by a
        // disagreeing DEF - every Shape/Row/etc already created under LEF's
        // 1000 units/micron would otherwise silently go out of alignment.
        EXPECT_EQ(root.get_technology(root.get_technology_ids().front())->database_units_microns, 1000.0);

        bool saw_precision_warning = false;
        for (const std::string &message : reader.messages())
            if (message.find("WARNING") != std::string::npos && message.find("coarser") != std::string::npos)
                saw_precision_warning = true;
        EXPECT_TRUE(saw_precision_warning) << "expected a precision warning since DEF units (500) < technology units (1000)";
    }

    // Regression: append_shapes_from_path (def_reader.cpp) used to leave
    // every routed Path's own width at 0 whenever the DEF text itself
    // never carried an explicit PATHWIDTH token - the common case for
    // ordinary routing (most real DEF writers rely entirely on the
    // LAYER's own default LEF WIDTH instead), which rendered as a
    // hairline stroke instead of the real trace width. route_default_width.def
    // has two nets on the same M1 layer - one with no PATHWIDTH at all,
    // one with an explicit override - proving both the new default AND
    // that an explicit width still wins.
    TEST(DEFReaderRouteWidth, DefaultsToLayerWidthWhenPathwidthOmittedButExplicitOverrideStillWins)
    {
        Root root;
        const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING", .width = 700});
        DEFReader reader;
        ASSERT_EQ(reader.read_def(std::string(IO_TEST_FIXTURES_DIR) + "/route_default_width.def", root, "test_lib"), 0);

        const DesignId design_id = root.get_design_by_name("route_width_test");
        ASSERT_TRUE(design_id.valid());
        const LayoutId layout_id = root.get_design_layout(design_id);
        ASSERT_TRUE(layout_id.valid());

        const std::vector<RouteId> &route_ids = root.get_layout_routes(layout_id);
        auto find_by_name = [&](const std::string &name) -> RouteId
        {
            for (const RouteId id : route_ids)
                if (const RouteData *route = root.get_route(id); route && route->name == name)
                    return id;
            return RouteId{};
        };
        auto first_path_width = [&](RouteId id) -> std::optional<int64_t>
        {
            for (const ShapeId shape_id : root.get_route_shapes(id))
                if (const Shape *shape = root.get_shape(shape_id); shape && !shape->paths.empty())
                    return shape->paths.front().width;
            return std::nullopt;
        };

        const RouteId no_width_id = find_by_name("NET_NO_WIDTH");
        ASSERT_TRUE(no_width_id.valid());
        const std::optional<int64_t> no_width = first_path_width(no_width_id);
        ASSERT_TRUE(no_width.has_value());
        EXPECT_EQ(*no_width, 700); // defaults to M1's own declared LEF width

        const RouteId with_width_id = find_by_name("NET_WITH_WIDTH");
        ASSERT_TRUE(with_width_id.valid());
        const std::optional<int64_t> with_width = first_path_width(with_width_id);
        ASSERT_TRUE(with_width.has_value());
        EXPECT_EQ(*with_width, 50); // explicit PATHWIDTH still overrides the default
    }
}
