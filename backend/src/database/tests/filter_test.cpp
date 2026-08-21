#include "../database.hpp"
#include "../filter.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    template <typename RootT, typename IdT, typename DataT>
    bool matches(std::string_view text, const RootT &root, IdT id, const DataT &data)
    {
        auto expr = parse_filter_expression(text);
        if (!expr)
            ADD_FAILURE() << "parse_filter_expression(\"" << text << "\") failed: " << expr.error();
        return evaluate_filter(*expr, root, id, data);
    }
}

TEST(FilterExpression, ParsesAndEvaluatesEqualityOnAStringLeaf)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_TRUE(matches(".name == M1", root, layer_id, *data));
    EXPECT_FALSE(matches(".name == M2", root, layer_id, *data));
    EXPECT_TRUE(matches(".name != M2", root, layer_id, *data));
}

TEST(FilterExpression, GlobOperatorMatchesTclStyleWildcards)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_TRUE(matches(".name =~ M*", root, layer_id, *data));
    EXPECT_TRUE(matches(".name =~ M?", root, layer_id, *data));
    EXPECT_FALSE(matches(".name =~ V*", root, layer_id, *data));
}

TEST(FilterExpression, NumericComparisonOperatorsWorkOnADoubleLeaf)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    const TechnologyData *data = root.get_technology(tech_id);

    EXPECT_TRUE(matches(".database_units_microns == 2000", root, tech_id, *data));
    EXPECT_TRUE(matches(".database_units_microns >= 2000", root, tech_id, *data));
    EXPECT_TRUE(matches(".database_units_microns > 1000", root, tech_id, *data));
    EXPECT_FALSE(matches(".database_units_microns > 2000", root, tech_id, *data));
    EXPECT_TRUE(matches(".database_units_microns <= 2000", root, tech_id, *data));
    // A non-numeric literal against a numeric field never matches, rather than erroring.
    EXPECT_FALSE(matches(".database_units_microns == abc", root, tech_id, *data));
}

TEST(FilterExpression, AndRequiresBothSidesOrRequiresEitherSide)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_TRUE(matches(".name == M1 && .type == ROUTING", root, layer_id, *data));
    EXPECT_FALSE(matches(".name == M1 && .type == CUT", root, layer_id, *data));
    EXPECT_TRUE(matches(".name == M9 || .type == ROUTING", root, layer_id, *data));
    EXPECT_FALSE(matches(".name == M9 || .type == CUT", root, layer_id, *data));
}

TEST(FilterExpression, ParenthesesOverrideDefaultAndBeforeOrPrecedence)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    // Without parens, && binds tighter than ||: M9==false OR (M1==true AND
    // CUT==false) -> OR(false, false) -> false.
    EXPECT_FALSE(matches(".name == M9 || .name == M1 && .type == CUT", root, layer_id, *data));
    // Parens force the OR to be evaluated first instead: (M9 || M1)==true
    // AND ROUTING==true -> true.
    EXPECT_TRUE(matches("(.name == M9 || .name == M1) && .type == ROUTING", root, layer_id, *data));
}

TEST(FilterExpression, SingleHopResolvesParentField)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_TRUE(matches(".technology.database_units_microns == 2000", root, layer_id, *data));
    EXPECT_FALSE(matches(".technology.database_units_microns == 1000", root, layer_id, *data));
}

TEST(FilterExpression, SingleHopResolvesExistentialListField)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});

    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer_name = "M4"});
    const ObstructionData *data = root.get_obstruction(obstruction_id);

    EXPECT_TRUE(matches(".shapes.layer_name == M4", root, obstruction_id, *data));
    EXPECT_FALSE(matches(".shapes.layer_name == M9", root, obstruction_id, *data));
}

TEST(FilterExpression, TwoHopChainResolvesAcrossTwoParentLinks)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});
    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
    const ObstructionData *data = root.get_obstruction(obstruction_id);

    EXPECT_TRUE(matches(".abstract.design.name == CELL", root, obstruction_id, *data));
    EXPECT_FALSE(matches(".abstract.design.name == NOT_CELL", root, obstruction_id, *data));
}

TEST(FilterExpression, MatchesUpdatesMdItem15SExampleShape)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "IN0"});
    TerminalPortId port_id = root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
    root.create_shape(ShapeData{.terminal_port = port_id, .layer_name = "M4"});
    const TerminalPortData *data = root.get_terminal_port(port_id);

    EXPECT_TRUE(matches(".terminal.name =~ IN* && .shapes.layer_name == M4", root, port_id, *data));
    EXPECT_FALSE(matches(".terminal.name =~ OUT* && .shapes.layer_name == M4", root, port_id, *data));
}

TEST(FilterExpression, SearchIntegratesWithRootSearchX)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId m1 = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    LayerId m2 = root.create_layer(LayerData{.technology = tech_id, .name = "M2", .type = "ROUTING"});
    root.create_layer(LayerData{.technology = tech_id, .name = "V1", .type = "CUT"});

    auto expr = parse_filter_expression(".type == ROUTING");
    ASSERT_TRUE(expr.has_value());

    std::vector<LayerId> results = root.search_layer([&](const Root &r, LayerId id, const LayerData &d)
                                                       { return evaluate_filter(*expr, r, id, d); });

    EXPECT_EQ(results.size(), 2u);
    EXPECT_NE(std::find(results.begin(), results.end(), m1), results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), m2), results.end());
}

TEST(FilterExpression, ParseErrorsAreReportedNotThrown)
{
    EXPECT_FALSE(parse_filter_expression("").has_value());
    EXPECT_FALSE(parse_filter_expression(".name ==").has_value());
    EXPECT_FALSE(parse_filter_expression("name == M1").has_value());
    EXPECT_FALSE(parse_filter_expression(".name @@ M1").has_value());
    EXPECT_FALSE(parse_filter_expression(".name == M1 &&").has_value());
    EXPECT_FALSE(parse_filter_expression("(.name == M1").has_value());
    EXPECT_FALSE(parse_filter_expression(".name == \"unterminated").has_value());
}

TEST(FilterExpression, QuotedLiteralsAllowValuesContainingSpaces)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M 1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_TRUE(matches(".name == \"M 1\"", root, layer_id, *data));
    EXPECT_TRUE(matches(".name == 'M 1'", root, layer_id, *data));
}

TEST(FilterExpression, UnknownFieldOrHopNeverMatchesRatherThanErroring)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);

    EXPECT_FALSE(matches(".does_not_exist == M1", root, layer_id, *data));
    EXPECT_FALSE(matches(".bogus_hop.name == M1", root, layer_id, *data));
}
