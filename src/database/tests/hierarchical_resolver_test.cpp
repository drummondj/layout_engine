#include "../database.hpp"
#include "../hierarchical_resolver.hpp"
#include <gtest/gtest.h>

using namespace le;
using namespace le::hierarchy;

namespace
{
    // Builds:
    //   TOP.schematic: Instance "a" (-> A), Net "topnet"
    //     A.schematic:  Instance "b" (-> B), Instance "b2" (-> B), Net "topnet_a"
    //       B.schematic:  Instance "u_leaf" (-> LEAF), Net "n1"
    //         LEAF: no Schematic at all (a hard macro / physical leaf cell)
    //
    // "b" and "b2" deliberately reference the *same* Design B, so any test
    // exercising glob/"**" fan-out into both exercises the "one Schematic
    // per Design, not per instantiation" dedup the resolver relies on.
    struct Fixture
    {
        Root root;
        LibraryId library;
        DesignId leaf_design, b_design, a_design, top_design;
        SchematicId top_schematic, a_schematic, b_schematic;
        InstanceId inst_a, inst_b, inst_b2, inst_u_leaf;
        NetId net_topnet, net_topnet_a, net_n1;

        Fixture()
        {
            library = root.create_library(LibraryData{.name = "lib"});

            leaf_design = root.create_design(DesignData{.library = library, .name = "LEAF"});

            b_design = root.create_design(DesignData{.library = library, .name = "B"});
            b_schematic = root.create_schematic(SchematicData{.design = b_design});
            inst_u_leaf = root.create_instance(
                InstanceData{.schematic = b_schematic, .name = "u_leaf", .reference_design = leaf_design});
            net_n1 = root.create_net(NetData{.schematic = b_schematic, .name = "n1"});

            a_design = root.create_design(DesignData{.library = library, .name = "A"});
            a_schematic = root.create_schematic(SchematicData{.design = a_design});
            inst_b = root.create_instance(InstanceData{.schematic = a_schematic, .name = "b", .reference_design = b_design});
            inst_b2 = root.create_instance(InstanceData{.schematic = a_schematic, .name = "b2", .reference_design = b_design});
            net_topnet_a = root.create_net(NetData{.schematic = a_schematic, .name = "topnet_a"});

            top_design = root.create_design(DesignData{.library = library, .name = "TOP"});
            top_schematic = root.create_schematic(SchematicData{.design = top_design});
            inst_a = root.create_instance(InstanceData{.schematic = top_schematic, .name = "a", .reference_design = a_design});
            net_topnet = root.create_net(NetData{.schematic = top_schematic, .name = "topnet"});
        }
    };
}

TEST(HierarchicalResolver, SplitPathSplitsOnSlash)
{
    EXPECT_EQ(split_path("a/b/c"), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(split_path("a"), (std::vector<std::string>{"a"}));
}

TEST(HierarchicalResolver, SplitPathRejectsEmptyOrMalformedPaths)
{
    EXPECT_TRUE(split_path("").empty());
    EXPECT_TRUE(split_path("/a").empty());
    EXPECT_TRUE(split_path("a/").empty());
    EXPECT_TRUE(split_path("a//b").empty());
}

TEST(HierarchicalResolver, SplitPathUnescapesBackslashedCharactersInASegment)
{
    // DEF preserves a Verilog escaped identifier's own backslash-escaping
    // literally (e.g. a synthesizer-flattened array element name comes
    // back as "block_reg\[0\]") - the Schematic's own Net/Instance names
    // never carry this escaping (slang already strips it), so matching
    // needs the unescaped form.
    EXPECT_EQ(split_path("a/block_reg\\[0\\]"), (std::vector<std::string>{"a", "block_reg[0]"}));
    EXPECT_EQ(split_path("core_dec_block_rkey\\[1\\]"), (std::vector<std::string>{"core_dec_block_rkey[1]"}));
}

TEST(HierarchicalResolver, SplitPathTreatsAnEscapedSlashAsLiteralNotADelimiter)
{
    // A "/" that's itself escaped ("\/") is part of one segment's own
    // name, not a hierarchy boundary - matches DEF's own per-character
    // escaping convention (any backslash-prefixed character is literal).
    EXPECT_EQ(split_path("a\\/b/c"), (std::vector<std::string>{"a/b", "c"}));
}

TEST(HierarchicalResolver, SplitPathHandlesAnEscapedBackslashAsOneLiteralBackslash)
{
    EXPECT_EQ(split_path("a\\\\b"), (std::vector<std::string>{"a\\b"}));
}

TEST(HierarchicalResolver, SplitPathRejectsATrailingLoneBackslash)
{
    EXPECT_TRUE(split_path("a\\").empty());
}

TEST(HierarchicalResolver, SingleSegmentExactMatchResolvesADirectChildInstance)
{
    Fixture f;
    EXPECT_EQ(resolve_instances(f.root, f.top_schematic, {"a"}), (std::vector<InstanceId>{f.inst_a}));
}

TEST(HierarchicalResolver, MultiSegmentExactMatchDescendsThroughNestedInstances)
{
    Fixture f;
    EXPECT_EQ(resolve_instances(f.root, f.top_schematic, {"a", "b"}), (std::vector<InstanceId>{f.inst_b}));
    EXPECT_EQ(resolve_instances(f.root, f.top_schematic, {"a", "b", "u_leaf"}), (std::vector<InstanceId>{f.inst_u_leaf}));
}

TEST(HierarchicalResolver, MultiSegmentExactMatchResolvesANestedNet)
{
    Fixture f;
    EXPECT_EQ(resolve_nets(f.root, f.top_schematic, {"a", "b", "n1"}), (std::vector<NetId>{f.net_n1}));
    EXPECT_EQ(resolve_nets(f.root, f.top_schematic, {"a", "topnet_a"}), (std::vector<NetId>{f.net_topnet_a}));
}

TEST(HierarchicalResolver, UnknownSegmentResolvesToNoResultsRatherThanErroring)
{
    Fixture f;
    EXPECT_TRUE(resolve_instances(f.root, f.top_schematic, {"nonexistent"}).empty());
    EXPECT_TRUE(resolve_instances(f.root, f.top_schematic, {"a", "nonexistent"}).empty());
}

TEST(HierarchicalResolver, LeafRecursionStopsCleanlyAtAHardMacroWithNoSchematic)
{
    Fixture f;
    // "u_leaf" resolves to LEAF, which has no Schematic at all - a further
    // segment past it must yield no results, not crash or error.
    EXPECT_TRUE(resolve_instances(f.root, f.top_schematic, {"a", "b", "u_leaf", "anything"}).empty());
}

TEST(HierarchicalResolver, GlobLeafMatchesEverySiblingInstanceByPattern)
{
    Fixture f;
    auto results = resolve_instances(f.root, f.a_schematic, {"b*"});
    std::sort(results.begin(), results.end());
    std::vector<InstanceId> expected{f.inst_b, f.inst_b2};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(results, expected);
}

TEST(HierarchicalResolver, GlobMidPathFansOutButDedupesTheSharedDesignSchematic)
{
    Fixture f;
    // "b" and "b2" both reference Design B, which has exactly one
    // Schematic (not one per instantiation) - "b*" as a MIDDLE segment
    // must not visit that shared Schematic twice, so "u_leaf" (and n1)
    // must each come back exactly once, not duplicated.
    EXPECT_EQ(resolve_instances(f.root, f.top_schematic, {"a", "b*", "u_leaf"}), (std::vector<InstanceId>{f.inst_u_leaf}));
    EXPECT_EQ(resolve_nets(f.root, f.top_schematic, {"a", "b*", "n1"}), (std::vector<NetId>{f.net_n1}));
}

TEST(HierarchicalResolver, RecursiveDescentAtTheLeafFindsEveryDescendantAtAnyDepth)
{
    Fixture f;
    // top/a/** as leaf_kind=Instance: every Instance nested anywhere under
    // "a" - "b", "b2" (depth 1) and "u_leaf" (depth 2, once - not once per
    // "b"/"b2" parent, since B's Schematic is shared).
    auto results = resolve_instances(f.root, f.top_schematic, {"a", "**"});
    std::sort(results.begin(), results.end());
    std::vector<InstanceId> expected{f.inst_b, f.inst_b2, f.inst_u_leaf};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(results, expected);
}

TEST(HierarchicalResolver, RecursiveDescentIncludesTheAnchorItselfAtDepthZero)
{
    Fixture f;
    // top/a/** for leaf_kind=Net must include "a"'s own direct Net
    // (topnet_a, depth 0) as well as the deeper "n1" (depth 1, via B).
    auto results = resolve_nets(f.root, f.top_schematic, {"a", "**"});
    std::sort(results.begin(), results.end());
    std::vector<NetId> expected{f.net_topnet_a, f.net_n1};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(results, expected);
}

TEST(HierarchicalResolver, RecursiveDescentAsAMiddleSegmentContinuesMatchingAfterIt)
{
    Fixture f;
    // top/**/n1: n1 lives two levels down (top -> a -> b -> B.schematic),
    // reachable only by recursing through "a" then "b"/"b2".
    EXPECT_EQ(resolve_nets(f.root, f.top_schematic, {"**", "n1"}), (std::vector<NetId>{f.net_n1}));
}

TEST(HierarchicalResolver, EmptySegmentsOrInvalidRootResolveToNoResults)
{
    Fixture f;
    EXPECT_TRUE(resolve_instances(f.root, f.top_schematic, {}).empty());
    EXPECT_TRUE(resolve_instances(f.root, SchematicId{}, {"a"}).empty());
}
