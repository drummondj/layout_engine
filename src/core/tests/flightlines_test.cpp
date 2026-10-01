#include "core/flightlines.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <tuple>

using namespace le;

namespace
{
    // One 1000x1000 cell INV (pin A at local (200,200), Y at (800,800)),
    // and a TOP design with a Schematic and a Layout:
    //   U1 (N at 0,0), U2 (FN at 2000,0), U3 (N at 0,2000), U4 (unlinked)
    //   n1: U1.Y, U2.A, U3.A      n2: U2.Y, top-level pin OUT at (5000,0)
    struct FlightlineFixture : ::testing::Test
    {
        Root root;
        LayoutId layout;
        PlacementId u1, u2, u3, u4;

        void SetUp() override
        {
            root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            const LibraryId library = root.create_library(LibraryData{.name = "LIB"});

            const DesignId inv = root.create_design(DesignData{.library = library, .name = "INV"});
            AbstractData inv_abstract{};
            inv_abstract.design = inv;
            inv_abstract.size = Point{1000, 1000};
            const AbstractId abstract = root.create_abstract(inv_abstract);
            const auto add_terminal = [&](const char *name, Rect rect)
            {
                TerminalData terminal{};
                terminal.abstract = abstract;
                terminal.name = name;
                const TerminalPortId port = root.create_terminal_port(TerminalPortData{.terminal = root.create_terminal(terminal)});
                ShapeData shape{};
                shape.terminal_port = port;
                shape.rects = {rect};
                root.create_shape(shape);
            };
            add_terminal("A", Rect{.ll = {100, 100}, .ur = {300, 300}});
            add_terminal("Y", Rect{.ll = {700, 700}, .ur = {900, 900}});

            const DesignId top = root.create_design(DesignData{.library = library, .name = "TOP"});
            const SchematicId schematic = root.create_schematic(SchematicData{.design = top});
            layout = root.create_layout(LayoutData{.design = top});

            const auto net = [&](const char *name)
            {
                NetData data{};
                data.schematic = schematic;
                data.name = name;
                return root.create_net(data);
            };
            const NetId n1 = net("n1");
            const NetId n2 = net("n2");

            const auto place = [&](const char *name, Point location, Orientation orientation, std::vector<std::pair<const char *, NetId>> pins)
            {
                InstanceId instance;
                if (!pins.empty())
                {
                    InstanceData inst{};
                    inst.schematic = schematic;
                    inst.name = name;
                    inst.reference_design = inv;
                    instance = root.create_instance(inst);
                    for (const auto &[pin_name, pin_net] : pins)
                    {
                        PinData pin{};
                        pin.instance = instance;
                        pin.name = pin_name;
                        pin.net = pin_net;
                        root.create_pin(pin);
                    }
                }
                return root.create_placement(PlacementData{.layout = layout, .name = name, .reference_design = inv, .instance = instance,
                                                           .placement_status = PlacementStatus::PLACED, .location = location, .orientation = orientation});
            };
            u1 = place("U1", Point{0, 0}, Orientation::N, {{"A", NetId{}}, {"Y", n1}});
            u2 = place("U2", Point{2000, 0}, Orientation::FN, {{"A", n1}, {"Y", n2}});
            u3 = place("U3", Point{0, 2000}, Orientation::N, {{"A", n1}});
            u4 = place("U4", Point{4000, 4000}, Orientation::N, {});

            PhysicalPortData out{};
            out.layout = layout;
            out.name = "OUT";
            out.net = n2;
            out.location = Point{5000, 0};
            root.create_physical_port(out);
        }

        std::vector<Flightline> lines_for(std::vector<PlacementId> selected)
        {
            const NetEndpointIndex index(root, layout);
            return placement_flightlines(root, index, selected);
        }
    };

    // Order-insensitive comparison of undirected segments.
    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> normalized(const std::vector<Flightline> &lines)
    {
        std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> out;
        for (const Flightline &l : lines)
        {
            auto a = std::make_pair(l.from.x, l.from.y);
            auto b = std::make_pair(l.to.x, l.to.y);
            if (b < a)
                std::swap(a, b);
            out.emplace_back(a.first, a.second, b.first, b.second);
        }
        std::ranges::sort(out);
        return out;
    }
}

TEST_F(FlightlineFixture, SelectedPinFansOutToEveryOtherPinOnItsNet)
{
    // U1.Y (800,800) to U2.A - FN mirrors local x: 2000 + (1000-200) - and U3.A.
    EXPECT_EQ(normalized(lines_for({u1})), (normalized({
                                               Flightline{.from = {800, 800}, .to = {2800, 200}},
                                               Flightline{.from = {800, 800}, .to = {200, 2200}},
                                           })));
}

TEST_F(FlightlineFixture, AConnectionBetweenTwoSelectedPinsIsDrawnOnceAndPortsAreEndpoints)
{
    // U1.Y-U2.A once (not once per side), U1.Y-U3.A, U2.A-U3.A, and
    // U2.Y (FN: 2000 + (1000-800), 800) to the top-level pin OUT.
    EXPECT_EQ(normalized(lines_for({u1, u2})), (normalized({
                                                   Flightline{.from = {800, 800}, .to = {2800, 200}},
                                                   Flightline{.from = {800, 800}, .to = {200, 2200}},
                                                   Flightline{.from = {2800, 200}, .to = {200, 2200}},
                                                   Flightline{.from = {2200, 800}, .to = {5000, 0}},
                                               })));
}

TEST_F(FlightlineFixture, UnlinkedOrUnconnectedPlacementsDrawNothing)
{
    EXPECT_TRUE(lines_for({u4}).empty());
    EXPECT_TRUE(lines_for({}).empty());
}

TEST_F(FlightlineFixture, NetsAboveTheFanoutLimitDrawNothing)
{
    // U1.Y's net n1 has 2 other endpoints (U2.A, U3.A).
    const NetEndpointIndex index(root, layout);
    const std::vector<PlacementId> selected{u1};
    EXPECT_EQ(placement_flightlines(root, index, selected, 0).size(), 2u); // 0: no limit
    EXPECT_EQ(placement_flightlines(root, index, selected, 2).size(), 2u); // exactly at the limit - drawn
    EXPECT_TRUE(placement_flightlines(root, index, selected, 1).empty());  // above it - skipped

    // U2: n1 (fanout 2) is skipped at limit 1, n2 (fanout 1, the top-level pin) is kept.
    const std::vector<PlacementId> u2_selected{u2};
    EXPECT_EQ(placement_flightlines(root, index, u2_selected, 1).size(), 1u);
}
