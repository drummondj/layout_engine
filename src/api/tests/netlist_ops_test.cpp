#include "views.hpp"

#include <gtest/gtest.h>

#include <string>

// Netlist edits that keep the Layout linked, property paths and the
// hierarchical path searches, through the C++ API.

namespace
{
    // TOP's Schematic holds Instance u1 (of LEAF) with Pin A on Net n1; its
    // Layout holds Placement u1 and Route n1, linked to them.
    struct NetlistFixture : public ::testing::Test
    {
        void SetUp() override
        {
            le::WriteView view = session.write();
            le::Root &root = view.unrecorded_root();
            const le::LibraryId library = root.create_library({.name = "lib"});
            const le::DesignId leaf = root.create_design({.library = library, .name = "LEAF"});
            const le::DesignId top = root.create_design({.library = library, .name = "TOP"});
            schematic = root.create_schematic({.design = top});
            instance = root.create_instance({.schematic = schematic, .name = "u1", .reference_design = leaf});
            net = root.create_net({.schematic = schematic, .name = "n1"});
            pin = root.create_pin({.instance = instance, .name = "A", .net = net});
            const le::LayoutId layout = root.create_layout({.design = top});
            placement = root.create_placement({.layout = layout, .name = "u1", .instance = instance});
            route = root.create_route({.layout = layout, .name = "n1", .net = net});
        }

        size_t library_count() { return session.read().root().get_library_ids().size(); }

        le::Session session;
        le::SchematicId schematic;
        le::InstanceId instance;
        le::NetId net;
        le::PinId pin;
        le::PlacementId placement;
        le::RouteId route;
    };
}

TEST_F(NetlistFixture, DeleteNetCascadeJoinsAnOpenUndoStep)
{
    {
        le::Transaction transaction = session.transaction("delete and add");
        session.write().create_library({.name = "BEFORE"}).value();
        ASSERT_TRUE(session.write().delete_net_cascade(net));
        session.write().create_library({.name = "AFTER"}).value();
    }
    EXPECT_EQ(session.read().root().get_net(net), nullptr);
    EXPECT_EQ(session.read().root().get_route(route), nullptr);
    EXPECT_FALSE(session.read().root().get_pin(pin)->net.valid());
    EXPECT_EQ(library_count(), 3u);

    EXPECT_EQ(session.write().undo(), true);
    EXPECT_EQ(library_count(), 1u);
    EXPECT_NE(session.read().root().get_net(net), nullptr);
    EXPECT_EQ(session.read().root().get_pin(pin)->net, net);
    EXPECT_FALSE(session.read().can_undo());
}

TEST_F(NetlistFixture, RenamesJoinAnOpenUndoStep)
{
    {
        le::Transaction transaction = session.transaction("renames");
        ASSERT_TRUE(session.write().rename_net(net, "n2"));
        ASSERT_TRUE(session.write().rename_instance(instance, "u2"));
        session.write().create_library({.name = "AFTER"}).value();
    }
    EXPECT_EQ(session.read().root().get_route(route)->name, "n2");
    EXPECT_EQ(session.read().root().get_placement(placement)->name, "u2");

    EXPECT_EQ(session.write().undo(), true);
    const le::ReadView view = session.read();
    EXPECT_EQ(view.root().get_net(net)->name, "n1");
    EXPECT_EQ(view.root().get_route(route)->name, "n1");
    EXPECT_EQ(view.root().get_instance(instance)->name, "u1");
    EXPECT_EQ(view.root().get_placement(placement)->name, "u1");
    EXPECT_EQ(view.root().get_library_ids().size(), 1u);
    EXPECT_FALSE(view.can_undo());
}

TEST_F(NetlistFixture, NetlistEditsGiveTheirReasons)
{
    le::WriteView view = session.write();
    EXPECT_EQ(view.delete_net_cascade(le::NetId{}).error(), "no such Net exists");
    EXPECT_EQ(view.rename_net(net, "").error(), "the new name may not be empty");
    view.unrecorded_root().create_route({.layout = view.root().get_placement(placement)->layout, .name = "taken"});
    const std::expected<void, std::string> clash = view.rename_net(net, "taken");
    ASSERT_FALSE(clash);
    EXPECT_NE(clash.error().find("would rename a linked object onto a taken name"), std::string::npos) << clash.error();
    EXPECT_EQ(view.root().get_net(net)->name, "n1");
}

TEST_F(NetlistFixture, BeginAndEndCommandSpanSeveralViews)
{
    session.write().begin_command("two views");
    session.write().create_library({.name = "A"}).value();
    session.write().create_library({.name = "B"}).value();
    session.write().end_command(true);

    EXPECT_EQ(library_count(), 3u);
    EXPECT_EQ(session.write().undo(), true);
    EXPECT_EQ(library_count(), 1u);
    const le::ReadView view = session.read();
    ASSERT_EQ(view.command_history_size(), 1u);
    EXPECT_EQ(view.command_history_at(0), "two views");
}

TEST_F(NetlistFixture, PropertyPathsReadThroughReferences)
{
    const le::ReadView view = session.read();
    const auto name = view.property(pin, ".name");
    ASSERT_TRUE(name && *name);
    EXPECT_EQ((*name)->string_value, "A");

    const auto net_name = view.property(pin, ".net.name");
    ASSERT_TRUE(net_name && *net_name) << (net_name ? "" : net_name.error());
    EXPECT_EQ((*net_name)->string_value, "n1");

    EXPECT_EQ(view.property(le::PinId{}, ".name"), std::nullopt); // no such Pin
    ASSERT_FALSE(view.property(pin, ".colour"));
    EXPECT_EQ(view.property(pin, ".colour").error(), "unknown field 'colour' on Pin");
    EXPECT_FALSE(view.property(pin, "name")); // a path starts with '.'
}

TEST_F(NetlistFixture, PathSearchesFindAndFilter)
{
    const le::ReadView view = session.read();
    EXPECT_EQ(view.get_instances_by_path(schematic, "u*").value(), std::vector<le::InstanceId>{instance});
    EXPECT_EQ(view.get_nets_by_path(schematic, "n1").value(), std::vector<le::NetId>{net});
    EXPECT_TRUE(view.get_nets_by_path(schematic, "n1", ".name == other").value().empty());
    EXPECT_FALSE(view.get_ports_by_path(schematic, "*", ".name ==")); // no value
}
