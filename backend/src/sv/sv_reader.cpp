#include "sv_reader.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceManager.h"

namespace le
{
    namespace
    {
        DesignId get_or_create_design(Root &root, LibraryId library_id, const std::string &name)
        {
            DesignId id = root.get_design_by_name(name);
            if (!id.valid())
                id = root.create_design(DesignData{.library = library_id, .name = name});
            return id;
        }

        SchematicId get_or_create_schematic(Root &root, DesignId design_id)
        {
            SchematicId id = root.get_design_schematic(design_id);
            if (!id.valid())
                id = root.create_schematic(SchematicData{.design = design_id});
            return id;
        }

        NetId get_or_create_net(Root &root, SchematicId schematic_id, const std::string &name)
        {
            NetId id = root.get_net_by_name(schematic_id, name);
            if (!id.valid())
                id = root.create_net(NetData{.schematic = schematic_id, .name = name});
            return id;
        }

        PortId get_or_create_port(Root &root, SchematicId schematic_id, const std::string &name,
                                   SignalDirection direction, std::optional<int> msb, std::optional<int> lsb,
                                   NetId net_id = NetId{})
        {
            PortId id = root.get_port_by_name(schematic_id, name);
            if (!id.valid())
                id = root.create_port(PortData{.schematic = schematic_id,
                                               .name = name,
                                               .direction = direction,
                                               .msb = msb,
                                               .lsb = lsb,
                                               .net = net_id});
            return id;
        }

        // Bus [msb:lsb] range for a port/net's own declared type - unset
        // (scalar) for anything that isn't a real packed vector, matching
        // Port.msb/lsb's own "unset = scalar 1-bit" schema convention.
        void bit_range_from_type(const slang::ast::Type &type, std::optional<int> &msb, std::optional<int> &lsb)
        {
            if (!type.isIntegral() || type.isScalar())
                return;
            const auto range = type.getFixedRange();
            msb = range.left;
            lsb = range.right;
        }

        std::string source_text(const slang::SourceManager &source_manager, slang::SourceRange range)
        {
            const auto text = source_manager.getSourceText(range.start().buffer());
            const size_t start = range.start().offset();
            const size_t end = range.end().offset();
            if (start > end || end > text.size())
                return {};
            return std::string(text.substr(start, end - start));
        }

        // Recursively collects every direct InstanceSymbol child of `scope`,
        // following through generate blocks/arrays (the only structural
        // containers a gate-level netlist's own instances can be nested
        // inside without behavioral constructs this reader doesn't model).
        //
        // Every instance generated inside a generate-for loop shares its
        // bare name ("u_inv") with every other iteration - each
        // GenerateBlockArraySymbol entry is qualified with the array's
        // own name and loop index (e.g. "gen_inv[0].u_inv") as the walk
        // descends through it, so sibling Instance records stay
        // distinguishable, matching the linked plan's own naming example.
        // An unarrayed (if/case) generate block's own name is not
        // currently added to the prefix - a real, documented v1 gap,
        // since only loop-generate is exercised by this reader's own
        // fixtures so far.
        void collect_instance_children(const slang::ast::Scope &scope, const std::string &prefix,
                                        std::vector<std::pair<const slang::ast::InstanceSymbol *, std::string>> &out)
        {
            for (const auto &member : scope.members())
            {
                if (member.kind == slang::ast::SymbolKind::Instance)
                    out.emplace_back(&member.as<slang::ast::InstanceSymbol>(), prefix + std::string(member.name));
                else if (member.kind == slang::ast::SymbolKind::GenerateBlock)
                    collect_instance_children(member.as<slang::ast::GenerateBlockSymbol>(), prefix, out);
                else if (member.kind == slang::ast::SymbolKind::GenerateBlockArray)
                {
                    const auto &array = member.as<slang::ast::GenerateBlockArraySymbol>();
                    for (const auto *entry : array.entries)
                    {
                        std::string entry_prefix = prefix + std::string(array.name);
                        if (const auto *index = entry->getArrayIndex())
                        {
                            const auto index_value = index->as<int64_t>();
                            entry_prefix += "[" + (index_value ? std::to_string(*index_value) : index->toString()) + "]";
                        }
                        entry_prefix += ".";
                        collect_instance_children(*entry, entry_prefix, out);
                    }
                }
            }
        }

        // Same walk as above, but for the undefined-leaf-cell case (see
        // read_netlist's own comment) - slang represents an instantiation
        // of a module it never found a definition for as an
        // UninstantiatedDefSymbol, a different Symbol kind entirely, not
        // an InstanceSymbol.
        void collect_uninstantiated_children(const slang::ast::Scope &scope,
                                              std::vector<const slang::ast::UninstantiatedDefSymbol *> &out)
        {
            for (const auto &member : scope.members())
            {
                if (member.kind == slang::ast::SymbolKind::UninstantiatedDef)
                    out.push_back(&member.as<slang::ast::UninstantiatedDefSymbol>());
                else if (member.kind == slang::ast::SymbolKind::GenerateBlock)
                    collect_uninstantiated_children(member.as<slang::ast::GenerateBlockSymbol>(), out);
                else if (member.kind == slang::ast::SymbolKind::GenerateBlockArray)
                    collect_uninstantiated_children(member.as<slang::ast::GenerateBlockArraySymbol>(), out);
            }
        }
    }

    SignalDirection SVReader::signal_direction_from_parser(slang::ast::ArgumentDirection direction)
    {
        switch (direction)
        {
        case slang::ast::ArgumentDirection::In:
            return SignalDirection::INPUT;
        case slang::ast::ArgumentDirection::Out:
            return SignalDirection::OUTPUT;
        case slang::ast::ArgumentDirection::InOut:
            return SignalDirection::INOUT;
        case slang::ast::ArgumentDirection::Ref:
            // Not a real signal direction (used for task/function ref
            // arguments, not module ports) - InOut is the closest
            // conservative stand-in this database's own vocabulary has.
            return SignalDirection::INOUT;
        }
        return SignalDirection::INOUT;
    }

    namespace
    {
        void populate_ports(Root &root, SchematicId schematic_id, const slang::ast::InstanceBodySymbol &body)
        {
            for (const auto *port_symbol : body.getPortList())
            {
                // MultiPortSymbol/InterfacePortSymbol (concatenated ports,
                // interface ports) are out of scope for this pass - a real,
                // deliberate v1 limit, not an oversight.
                if (port_symbol->kind != slang::ast::SymbolKind::Port)
                    continue;

                const auto &port = port_symbol->as<slang::ast::PortSymbol>();
                std::optional<int> msb, lsb;
                bit_range_from_type(port.getType(), msb, lsb);

                const std::string name(port.name);
                // Verilog gives every port an implicit net of the same name -
                // created first so the Port record below can reference it
                // directly (Port.net, not the other way around - Net has no
                // back-reference to any Pin/Port, matching Pin.net's own
                // shape uniformly for every connection endpoint).
                const NetId net_id = get_or_create_net(root, schematic_id, name);
                get_or_create_port(
                    root, schematic_id, name, SVReader::signal_direction_from_parser(port.direction), msb, lsb, net_id);
            }
        }

        // Extracts a Pin's net/net_bit_index/raw_expression from one
        // connection's expression - see sv_reader.hpp's own read_netlist
        // comment for the exact classification rules.
        void populate_pin_from_expression(Root &root, SchematicId parent_schematic_id, PinData &pin_data,
                                           const slang::ast::Expression *expression,
                                           const slang::SourceManager &source_manager)
        {
            if (!expression)
                return;

            if (expression->kind == slang::ast::ExpressionKind::NamedValue)
            {
                const auto &named = expression->as<slang::ast::NamedValueExpression>();
                pin_data.net = get_or_create_net(root, parent_schematic_id, std::string(named.symbol.name));
                return;
            }

            if (expression->kind == slang::ast::ExpressionKind::ElementSelect)
            {
                const auto &select = expression->as<slang::ast::ElementSelectExpression>();
                if (select.value().kind == slang::ast::ExpressionKind::NamedValue)
                {
                    const auto &named = select.value().as<slang::ast::NamedValueExpression>();
                    if (const auto *constant = select.selector().getConstant())
                    {
                        if (const auto index = constant->integer().as<int32_t>())
                        {
                            pin_data.net = get_or_create_net(root, parent_schematic_id, std::string(named.symbol.name));
                            pin_data.net_bit_index = *index;
                            return;
                        }
                    }
                }
            }

            // Anything else (concatenation, part-select, constant tie, a
            // non-constant bit-select index) - a deliberate v1 scope
            // limit, not a bug: raw_expression is always captured
            // regardless, so no connectivity information is lost, just
            // left unstructured.
            pin_data.raw_expression = source_text(source_manager, expression->sourceRange);
        }

        void populate_pins(Root &root, InstanceId instance_id, SchematicId parent_schematic_id,
                            const slang::ast::InstanceSymbol &instance, const slang::SourceManager &source_manager)
        {
            for (const auto *connection : instance.getPortConnections())
            {
                PinData pin_data{.instance = instance_id, .name = std::string(connection->port.name)};
                if (const auto *port = connection->port.as_if<slang::ast::PortSymbol>())
                    pin_data.direction = SVReader::signal_direction_from_parser(port->direction);
                populate_pin_from_expression(root, parent_schematic_id, pin_data, connection->getExpression(),
                                             source_manager);
                root.create_pin(pin_data);
            }
        }
    }

    int SVReader::read_netlist(std::vector<std::string> filenames, Root &root, std::string library_name)
    {
        messages_.clear();
        root_ = &root;

        library_id_ = root.get_library_by_name(library_name);
        if (!library_id_.valid())
            library_id_ = root.create_library(LibraryData{.name = library_name});

        slang::ast::Compilation compilation;
        bool any_tree_loaded = false;
        for (const auto &filename : filenames)
        {
            auto tree = slang::syntax::SyntaxTree::fromFile(filename);
            if (!tree)
            {
                messages_.push_back(
                    fmt::format("ERROR: read_netlist: failed to open '{}': {}", filename, tree.error().second));
                continue;
            }
            compilation.addSyntaxTree(tree.value());
            any_tree_loaded = true;
        }
        if (!any_tree_loaded)
            return 1;

        const auto &source_manager = slang::syntax::SyntaxTree::getDefaultSourceManager();
        const auto &design_root = compilation.getRoot();

        std::unordered_set<std::string> processed_definitions;

        // A recursive walk over the elaborated hierarchy: an Instance
        // record is created under `parent_schematic_id` for every
        // InstanceSymbol *except* the synthetic top-level call (an
        // invalid parent_schematic_id - nothing instantiates a real
        // top-level module in our own data model, it's just a Design
        // with its own populated Schematic). A module definition's own
        // ports/children are populated exactly once regardless of how
        // many times it's instantiated - our schema has one Schematic
        // per Design, not one per elaborated (possibly parameter-
        // varying) instantiation; the first body encountered for a given
        // definition name is treated as canonical. This is a deliberate
        // v1 simplification, not an oversight.
        std::function<void(const slang::ast::InstanceSymbol &, const std::string &, SchematicId)> visit_instance =
            [&](const slang::ast::InstanceSymbol &instance, const std::string &qualified_name,
                SchematicId parent_schematic_id)
        {
            const std::string definition_name(instance.getDefinition().name);
            const DesignId design_id = get_or_create_design(root, library_id_, definition_name);
            const SchematicId own_schematic_id = get_or_create_schematic(root, design_id);

            if (parent_schematic_id.valid())
            {
                const InstanceId instance_id = root.create_instance(InstanceData{
                    .schematic = parent_schematic_id,
                    .name = qualified_name,
                    .reference_name = definition_name,
                    .reference_design = design_id,
                });
                populate_pins(root, instance_id, parent_schematic_id, instance, source_manager);
            }

            if (processed_definitions.insert(definition_name).second)
            {
                populate_ports(root, own_schematic_id, instance.body);

                std::vector<std::pair<const slang::ast::InstanceSymbol *, std::string>> children;
                collect_instance_children(instance.body, "", children);
                for (const auto &[child, child_qualified_name] : children)
                    visit_instance(*child, child_qualified_name, own_schematic_id);

                // Undefined leaf cells (the common case for real gate-
                // level netlists referencing standard cells never
                // defined in `filenames`): create the Instance with
                // reference_name set, reference_design left unresolved -
                // link_unresolved_instances() is solely responsible for
                // resolving it later (e.g. once the corresponding LEF is
                // read). Pin population for this case is a deliberate,
                // documented v1 gap: UninstantiatedDefSymbol's own port
                // connections are AssertionExprs, not plain Expressions
                // (it can't fully type-check without a real definition),
                // and unwrapping them isn't worth the complexity before
                // real usage shows it's needed.
                std::vector<const slang::ast::UninstantiatedDefSymbol *> undefined_children;
                collect_uninstantiated_children(instance.body, undefined_children);
                for (const auto *child : undefined_children)
                {
                    root.create_instance(InstanceData{
                        .schematic = own_schematic_id,
                        .name = std::string(child->name),
                        .reference_name = std::string(child->definitionName),
                    });
                }
            }
        };

        for (const auto *top_instance : design_root.topInstances)
            visit_instance(*top_instance, std::string(top_instance->name), SchematicId{});

        const std::string report = slang::DiagnosticEngine::reportAll(source_manager, compilation.getAllDiagnostics());
        size_t line_start = 0;
        while (line_start <= report.size())
        {
            const size_t line_end = report.find('\n', line_start);
            const std::string line = report.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
            if (!line.empty())
                messages_.push_back(line);
            if (line_end == std::string::npos)
                break;
            line_start = line_end + 1;
        }

        link_unresolved_instances(root);
        return 0;
    }

    namespace
    {
        bool range_has_diagnostic(const slang::Diagnostics &diagnostics, slang::SourceRange range)
        {
            for (const auto &diag : diagnostics)
                if (range.contains(diag.location))
                    return true;
            return false;
        }

        std::optional<std::string> summarize_diagnostics_in_range(const slang::Diagnostics &diagnostics,
                                                                    slang::SourceRange range,
                                                                    const slang::SourceManager &source_manager)
        {
            slang::Diagnostics subset;
            for (const auto &diag : diagnostics)
                if (range.contains(diag.location))
                    subset.push_back(diag);
            if (subset.empty())
                return std::nullopt;
            return slang::DiagnosticEngine::reportAll(source_manager, subset);
        }

        SignalDirection direction_from_token_text(std::string_view text)
        {
            if (text == "output")
                return SignalDirection::OUTPUT;
            if (text == "inout")
                return SignalDirection::INOUT;
            return SignalDirection::INPUT; // "input", or an omitted/inherited direction.
        }

        struct RtlPortInfo
        {
            std::string name;
            SignalDirection direction;
        };

        // Extracts an ANSI port list's ports (name + direction only - see
        // sv_reader.hpp's own read_rtl comment for why bit width isn't
        // attempted here) if `header.ports` is a clean AnsiPortListSyntax.
        // Returns false (no output) for a non-ANSI port list or one with
        // its own diagnostic - both send the whole module to the
        // logic-cloud fallback instead, a deliberate v1 scope limit.
        bool extract_ansi_ports(const slang::syntax::ModuleHeaderSyntax &header,
                                 const slang::Diagnostics &diagnostics, std::vector<RtlPortInfo> &out)
        {
            if (!header.ports || header.ports->kind != slang::syntax::SyntaxKind::AnsiPortList)
                return false;
            if (range_has_diagnostic(diagnostics, header.ports->sourceRange()))
                return false;

            const auto &ansi_list = header.ports->as<slang::syntax::AnsiPortListSyntax>();
            SignalDirection last_direction = SignalDirection::INPUT;
            for (const auto *port_member : ansi_list.ports)
            {
                // ExplicitAnsiPort (.name(expr)) - out of scope for v1.
                if (port_member->kind != slang::syntax::SyntaxKind::ImplicitAnsiPort)
                    continue;
                const auto &implicit_port = port_member->as<slang::syntax::ImplicitAnsiPortSyntax>();

                slang::parsing::Token direction_token;
                if (implicit_port.header->kind == slang::syntax::SyntaxKind::VariablePortHeader)
                    direction_token = implicit_port.header->as<slang::syntax::VariablePortHeaderSyntax>().direction;
                else if (implicit_port.header->kind == slang::syntax::SyntaxKind::NetPortHeader)
                    direction_token = implicit_port.header->as<slang::syntax::NetPortHeaderSyntax>().direction;
                if (direction_token)
                    last_direction = direction_from_token_text(direction_token.valueText());

                out.push_back(
                    RtlPortInfo{std::string(implicit_port.declarator->name.valueText()), last_direction});
            }
            return true;
        }

        bool is_identifier_char(char c, bool first)
        {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
                   (!first && std::isdigit(static_cast<unsigned char>(c)));
        }

        bool is_identifier(const std::string &text)
        {
            if (text.empty() || !is_identifier_char(text[0], true))
                return false;
            for (size_t i = 1; i < text.size(); ++i)
                if (!is_identifier_char(text[i], false))
                    return false;
            return true;
        }

        // RTL flavor has no elaborated Expression to classify a
        // connection's value the way read_netlist's own
        // populate_pin_from_expression does - only raw syntax. Rather
        // than hand-unwrap slang's PropertyExpr/SequenceExpr assertion-
        // context wrapper chain down to a plain expression syntax node
        // purely to recognize two simple shapes, this classifies the
        // connection's own verbatim source text directly: a bare
        // identifier, or identifier[digits] (a literal bit-select - a
        // parameter-driven index is deliberately not evaluated, see this
        // file's own read_rtl comment). Anything else - concatenation,
        // constant ties, a non-literal index - falls back to
        // raw_expression, same as read_netlist's own fallback.
        void classify_simple_connection_text(Root &root, SchematicId schematic_id, PinData &pin_data,
                                              const std::string &text)
        {
            if (is_identifier(text))
            {
                pin_data.net = get_or_create_net(root, schematic_id, text);
                return;
            }

            if (!text.empty() && text.back() == ']')
            {
                const size_t bracket = text.find('[');
                if (bracket != std::string::npos)
                {
                    const std::string name_part = text.substr(0, bracket);
                    const std::string index_part = text.substr(bracket + 1, text.size() - bracket - 2);
                    const bool index_is_numeric =
                        !index_part.empty() &&
                        std::all_of(index_part.begin(), index_part.end(),
                                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
                    if (is_identifier(name_part) && index_is_numeric)
                    {
                        pin_data.net = get_or_create_net(root, schematic_id, name_part);
                        pin_data.net_bit_index = std::stoi(index_part);
                        return;
                    }
                }
            }

            pin_data.raw_expression = text;
        }

        void process_instantiation(Root &root, SchematicId schematic_id,
                                    const slang::syntax::HierarchyInstantiationSyntax &instantiation_syntax,
                                    const std::unordered_map<std::string, std::vector<RtlPortInfo>> &ports_by_module,
                                    const slang::SourceManager &source_manager)
        {
            const std::string reference_name(instantiation_syntax.type.valueText());
            const auto ports_it = ports_by_module.find(reference_name);

            for (const auto *hierarchical_instance : instantiation_syntax.instances)
            {
                if (!hierarchical_instance->decl)
                    continue;
                const InstanceId instance_id = root.create_instance(InstanceData{
                    .schematic = schematic_id,
                    .name = std::string(hierarchical_instance->decl->name.valueText()),
                    .reference_name = reference_name,
                });

                size_t positional_index = 0;
                for (const auto *connection : hierarchical_instance->connections)
                {
                    PinData pin_data{.instance = instance_id};
                    const slang::syntax::PropertyExprSyntax *value_node = nullptr;

                    if (connection->kind == slang::syntax::SyntaxKind::NamedPortConnection)
                    {
                        const auto &named = connection->as<slang::syntax::NamedPortConnectionSyntax>();
                        pin_data.name = std::string(named.name.valueText());
                        value_node = named.expr;
                    }
                    else if (connection->kind == slang::syntax::SyntaxKind::OrderedPortConnection)
                    {
                        const auto &ordered = connection->as<slang::syntax::OrderedPortConnectionSyntax>();
                        pin_data.name = (ports_it != ports_by_module.end() && positional_index < ports_it->second.size())
                                             ? ports_it->second[positional_index].name
                                             : fmt::format("$pos{}", positional_index);
                        value_node = ordered.expr;
                        ++positional_index;
                    }
                    else
                    {
                        continue; // Wildcard (.*) connection - out of scope for v1.
                    }

                    if (value_node)
                        classify_simple_connection_text(root, schematic_id, pin_data,
                                                        source_text(source_manager, value_node->sourceRange()));
                    root.create_pin(pin_data);
                }
            }
        }

        void process_module(Root &root, SchematicId schematic_id, const slang::syntax::ModuleDeclarationSyntax &module_syntax,
                             const slang::Diagnostics &diagnostics,
                             const std::unordered_map<std::string, std::vector<RtlPortInfo>> &ports_by_module,
                             const slang::SourceManager &source_manager)
        {
            // Module header wasn't ANSI-style/clean (see extract_ansi_ports)
            // - the whole module body becomes one logic-cloud Instance,
            // matching the "entire module unreadable" fallback described in
            // this file's own read_rtl comment. No Ports for this Design.
            if (!ports_by_module.count(std::string(module_syntax.header->name.valueText())))
            {
                root.create_instance(InstanceData{
                    .schematic = schematic_id,
                    .name = std::string(module_syntax.header->name.valueText()) + "_body",
                    .rtl_text = source_text(source_manager, module_syntax.sourceRange()),
                    .diagnostic_summary =
                        summarize_diagnostics_in_range(diagnostics, module_syntax.sourceRange(), source_manager),
                });
                return;
            }

            int logic_cloud_index = 0;
            for (const auto *member : module_syntax.members)
            {
                if (member->kind == slang::syntax::SyntaxKind::HierarchyInstantiation)
                {
                    const auto &instantiation_syntax = member->as<slang::syntax::HierarchyInstantiationSyntax>();
                    if (!range_has_diagnostic(diagnostics, instantiation_syntax.sourceRange()))
                    {
                        process_instantiation(root, schematic_id, instantiation_syntax, ports_by_module, source_manager);
                        continue;
                    }
                    // Falls through to the logic-cloud fallback below - the
                    // instantiation's own connection list has a diagnostic.
                }

                // Everything else at module scope (procedural blocks,
                // assigns, generate constructs genuinely needing
                // elaboration) - or an instantiation with its own
                // diagnostic - becomes one logic-cloud Instance per member.
                root.create_instance(InstanceData{
                    .schematic = schematic_id,
                    .name = fmt::format("logic_cloud_{}", ++logic_cloud_index),
                    .rtl_text = source_text(source_manager, member->sourceRange()),
                    .diagnostic_summary = summarize_diagnostics_in_range(diagnostics, member->sourceRange(), source_manager),
                });
            }
        }
    }

    int SVReader::read_rtl(std::vector<std::string> filenames, Root &root, std::string library_name)
    {
        messages_.clear();
        root_ = &root;

        library_id_ = root.get_library_by_name(library_name);
        if (!library_id_.valid())
            library_id_ = root.create_library(LibraryData{.name = library_name});

        std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> trees;
        for (const auto &filename : filenames)
        {
            auto tree = slang::syntax::SyntaxTree::fromFile(filename);
            if (!tree)
            {
                messages_.push_back(
                    fmt::format("ERROR: read_rtl: failed to open '{}': {}", filename, tree.error().second));
                continue;
            }
            trees.push_back(tree.value());
        }
        if (trees.empty())
            return 1;

        const auto &source_manager = slang::syntax::SyntaxTree::getDefaultSourceManager();

        // First pass: every module's own name -> ordered port list,
        // collected across all files - so a positional connection
        // instantiating a module defined in a *different* file of this
        // same read can still be resolved, and so every module (whether
        // or not its own header is clean) is known by the time
        // instantiations elsewhere are processed in the second pass.
        std::unordered_map<std::string, std::vector<RtlPortInfo>> ports_by_module;
        std::vector<std::pair<const slang::syntax::ModuleDeclarationSyntax *, slang::syntax::SyntaxTree *>>
            all_modules;
        for (const auto &tree : trees)
        {
            auto visitor = slang::syntax::makeSyntaxVisitor(
                [&](auto &self, const slang::syntax::ModuleDeclarationSyntax &node)
                {
                    all_modules.emplace_back(&node, tree.get());
                    std::vector<RtlPortInfo> ports;
                    if (extract_ansi_ports(*node.header, tree->diagnostics(), ports))
                        ports_by_module.emplace(std::string(node.header->name.valueText()), std::move(ports));
                    self.visitDefault(node);
                });
            tree->root().visit(visitor);
        }

        for (const auto &[module_syntax, tree] : all_modules)
        {
            const std::string module_name(module_syntax->header->name.valueText());
            const DesignId design_id = get_or_create_design(root, library_id_, module_name);
            const SchematicId schematic_id = get_or_create_schematic(root, design_id);

            const auto ports_it = ports_by_module.find(module_name);
            if (ports_it != ports_by_module.end())
            {
                for (const auto &port_info : ports_it->second)
                {
                    // Net created first so Port can reference it directly -
                    // see populate_ports' own comment (read_netlist's half
                    // of this reader) for why the reference lives on Port.
                    const NetId net_id = get_or_create_net(root, schematic_id, port_info.name);
                    get_or_create_port(root, schematic_id, port_info.name, port_info.direction, std::nullopt,
                                       std::nullopt, net_id);
                }
            }

            process_module(root, schematic_id, *module_syntax, tree->diagnostics(), ports_by_module, source_manager);
        }

        for (const auto &tree : trees)
        {
            const std::string report = slang::DiagnosticEngine::reportAll(source_manager, tree->diagnostics());
            size_t line_start = 0;
            while (line_start <= report.size())
            {
                const size_t line_end = report.find('\n', line_start);
                const std::string line =
                    report.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
                if (!line.empty())
                    messages_.push_back(line);
                if (line_end == std::string::npos)
                    break;
                line_start = line_end + 1;
            }
        }

        link_unresolved_instances(root);
        return 0;
    }

    size_t SVReader::link_unresolved_instances(Root &root)
    {
        size_t resolved = 0;
        std::vector<InstanceId> instance_ids;
        root.for_each_instance_id([&](InstanceId id)
                                   { instance_ids.push_back(id); });
        for (const InstanceId id : instance_ids)
        {
            const InstanceData *instance = root.get_instance(id);
            if (!instance || instance->reference_design.valid() || !instance->reference_name.has_value())
                continue;
            const DesignId design_id = root.get_design_by_name(*instance->reference_name);
            if (design_id.valid())
            {
                root.update_instance(id, SchematicId{}, std::optional<DesignId>(design_id), std::nullopt,
                                     std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                ++resolved;
            }
        }
        return resolved;
    }
}
