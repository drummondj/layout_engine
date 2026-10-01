#pragma once

// The shared hierarchical-path resolver LINKING_STRATEGY_RESEARCH.md section 3
// designs: walks a "/"-delimited path down the logical Instance hierarchy
// starting from a Schematic, resolving every segment but the last through
// Instance.reference_design -> Design.schematic, then resolving the final
// segment against whichever leaf klass the caller wants (Instance, Net,
// Port, ...). One shared implementation, two callers: `link` (SVReader,
// exact-match segments only - a plain literal path never touches the
// glob/recursive branches below, so it pays none of their cost) and the
// TCL search commands (get_instances/get_nets/get_ports, which may pass
// glob or "**" segments - see le_tcl_shim.cpp).
//
// Segment kinds:
//   - a plain literal (no '*'/'?', not "**"): O(1) exact-match via the
//     generated unique_per_parent by-name index (get_instance_by_name/
//     get_net_by_name/get_port_by_name) - the fast path every `link` call
//     takes.
//   - a single-level glob (contains '*'/'?', not exactly "**"): matches
//     within one hierarchy level only, same as a shell glob against one
//     directory listing - never crosses into a child's own children.
//   - "**" (recursive descent, Bash globstar/`find -path` convention):
//     matches zero or more full hierarchy levels - "top/a/b/**" is every
//     descendant of `b` at any depth, "top/**/n1" is `n1` found at `top`
//     itself or nested arbitrarily deep below it.
//
// A path segment resolving to an Instance whose own Design has no
// Schematic (a hard macro/pure physical leaf cell) is a normal stopping
// point, not an error - it simply drops out of the frontier and
// contributes no results past that point (see LINKING_STRATEGY_RESEARCH.md
// section 6's "leaf recursion stopping condition").

#include "database.hpp"
#include "filter.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace le::hierarchy
{
    /// @brief Splits a "/"-delimited path into segments, e.g.
    /// "top/a/b/**" -> ["top", "a", "b", "**"]. An empty path, or one with
    /// an empty segment (leading/trailing/doubled "/"), is a malformed
    /// path - returns an empty vector rather than throwing, so a caller's
    /// own "no results" handling covers it uniformly.
    ///
    /// Backslash-escape aware, both for the "/" delimiter itself and for
    /// each segment's own content - DEF preserves a Verilog escaped
    /// identifier's backslash-escaping literally in COMPONENTS/NETS/PINS
    /// names (e.g. a synthesizer-flattened array element name comes back
    /// as `block_reg\[0\]`, and a real hierarchy separator inside such a
    /// name would come back `\/` the same way), whereas the Schematic's
    /// own Net/Instance names never carry escaping at all - slang's own
    /// NamedValueExpression::symbol.name already gives the clean,
    /// unescaped identifier text directly (Verilog's escaped-identifier
    /// syntax is stripped at the lexer level, never stored on the
    /// symbol), confirmed empirically rather than assumed while
    /// diagnosing exactly this mismatch on real synthesized-netlist DEF
    /// data. So a `\` immediately preceding any other character removes
    /// the backslash and keeps that character literally (matching DEF's
    /// own per-character escaping convention - not Verilog's own
    /// whole-token `\name ` syntax, which slang has already resolved by
    /// the time any of this code ever sees a name), and a `/` is only a
    /// real segment boundary when it isn't itself escaped this way. This
    /// unescaping only ever matters for DEF-derived exact-match paths
    /// (`link`'s own callers) - a TCL-typed glob pattern naturally never
    /// contains DEF's own escaping, and '['/']' were never glob-special
    /// in this dialect's own glob_match to begin with, so nothing about
    /// normal TCL usage changes here.
    inline std::vector<std::string> split_path(std::string_view path)
    {
        std::vector<std::string> segments;
        if (path.empty())
            return segments;

        std::string current;
        bool escaped = false;
        for (const char c : path)
        {
            if (escaped)
            {
                current.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '/')
            {
                if (current.empty())
                    return {};
                segments.push_back(std::move(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (escaped || current.empty())
            return {}; // trailing lone backslash, or trailing "/"
        segments.push_back(std::move(current));
        return segments;
    }

    inline bool is_recursive_segment(std::string_view segment) { return segment == "**"; }

    inline bool is_glob_segment(std::string_view segment)
    {
        return segment.find('*') != std::string_view::npos || segment.find('?') != std::string_view::npos;
    }

    /// @brief Every Schematic reachable from `start` via zero or more
    /// Instance.reference_design -> Design.schematic hops, including
    /// `start` itself (the zero-hop case) - the building block "**"
    /// needs, whether it's a middle segment (the remaining path segments
    /// continue matching from every reached Schematic) or the leaf
    /// segment (every leaf-kind object directly owned by every reached
    /// Schematic is a result). `visited` guards against a cycle in
    /// malformed data - valid Verilog can't actually instantiate a design
    /// recursively, but this doesn't assume the database is always
    /// well-formed.
    inline void collect_descendant_schematics(const Root &root, SchematicId start,
                                               std::unordered_set<uint64_t> &visited,
                                               std::vector<SchematicId> &out)
    {
        const uint64_t key = (static_cast<uint64_t>(start.generation) << 32) | start.index;
        if (!visited.insert(key).second)
            return;
        out.push_back(start);
        for (const InstanceId inst_id : root.get_schematic_instances(start))
        {
            const InstanceData *inst = root.get_instance(inst_id);
            if (!inst || !inst->reference_design.valid())
                continue;
            const SchematicId child = root.get_design_schematic(inst->reference_design);
            if (child.valid())
                collect_descendant_schematics(root, child, visited, out);
        }
    }

    /// @brief Descends one hierarchy level: for every Schematic in
    /// `frontier`, resolves `segment` (exact/glob/"**") against that
    /// Schematic's own Instances, collecting the Schematic reached via
    /// each match's own reference_design into `next_frontier` - deduped,
    /// since a Design has exactly one Schematic regardless of how many
    /// places instantiate it ("our schema has one Schematic per Design,
    /// not one per elaborated instantiation" - sv_reader.cpp's own
    /// visit_instance comment). A glob/"**" segment matching several
    /// sibling instances of the *same* Design would otherwise revisit
    /// that one shared Schematic once per matching sibling - redundant
    /// work at every subsequent level (real exponential-ish blowup for a
    /// wide glob into a commonly-instantiated leaf cell, not just a
    /// theoretical concern) and duplicate leaf results. Id<Tag>'s own
    /// std::hash specialization (generated/ids.hpp) makes this a plain
    /// unordered_set, not a sort+unique.
    inline void descend_one_segment(const Root &root, const std::vector<SchematicId> &frontier,
                                     const std::string &segment, std::vector<SchematicId> &next_frontier)
    {
        std::unordered_set<SchematicId> seen;
        auto add = [&](SchematicId child)
        {
            if (child.valid() && seen.insert(child).second)
                next_frontier.push_back(child);
        };

        if (is_recursive_segment(segment))
        {
            std::unordered_set<uint64_t> visited;
            for (const SchematicId schematic : frontier)
                collect_descendant_schematics(root, schematic, visited, next_frontier);
            return;
        }
        if (is_glob_segment(segment))
        {
            for (const SchematicId schematic : frontier)
                for (const InstanceId inst_id : root.get_schematic_instances(schematic))
                {
                    const InstanceData *inst = root.get_instance(inst_id);
                    if (!inst || !inst->reference_design.valid() || !filter_detail::glob_match(segment, inst->name))
                        continue;
                    add(root.get_design_schematic(inst->reference_design));
                }
            return;
        }
        for (const SchematicId schematic : frontier)
        {
            const InstanceId inst_id = root.get_instance_by_name(schematic, segment);
            if (!inst_id.valid())
                continue;
            const InstanceData *inst = root.get_instance(inst_id);
            if (!inst || !inst->reference_design.valid())
                continue;
            add(root.get_design_schematic(inst->reference_design));
        }
    }

    /// @brief The shared resolver itself. `leaf_kind` is entirely
    /// expressed through the three callables:
    ///   - by_name(root, schematic, name) -> LeafId - O(1) exact match.
    ///   - enumerate(root, schematic) -> const std::vector<LeafId>& - every
    ///     leaf-kind object directly owned by one Schematic (for a glob
    ///     or "**" leaf).
    ///   - name_of(root, id) -> const std::string& - for glob-matching a
    ///     candidate against the leaf pattern.
    /// A path with no glob/"**" segment anywhere degenerates exactly to a
    /// plain O(1)-per-segment walk (`frontier` never exceeds size 1) -
    /// `link`'s exact-match usage pays zero cost for the generality the
    /// TCL layer needs.
    template <typename LeafId, typename ByNameFn, typename EnumerateFn, typename NameOfFn>
    std::vector<LeafId> resolve_hierarchical_path(const Root &root, SchematicId root_schematic,
                                                   const std::vector<std::string> &segments, ByNameFn &&by_name,
                                                   EnumerateFn &&enumerate, NameOfFn &&name_of)
    {
        if (segments.empty() || !root_schematic.valid())
            return {};

        std::vector<SchematicId> frontier{root_schematic};
        for (size_t i = 0; i + 1 < segments.size(); ++i)
        {
            std::vector<SchematicId> next_frontier;
            descend_one_segment(root, frontier, segments[i], next_frontier);
            frontier = std::move(next_frontier);
            if (frontier.empty())
                return {};
        }

        const std::string &leaf = segments.back();
        std::vector<LeafId> results;
        if (is_recursive_segment(leaf))
        {
            std::unordered_set<uint64_t> visited;
            std::vector<SchematicId> all_schematics;
            for (const SchematicId schematic : frontier)
                collect_descendant_schematics(root, schematic, visited, all_schematics);
            for (const SchematicId schematic : all_schematics)
                for (const LeafId id : enumerate(root, schematic))
                    results.push_back(id);
        }
        else if (is_glob_segment(leaf))
        {
            for (const SchematicId schematic : frontier)
                for (const LeafId id : enumerate(root, schematic))
                    if (filter_detail::glob_match(leaf, name_of(root, id)))
                        results.push_back(id);
        }
        else
        {
            for (const SchematicId schematic : frontier)
            {
                const LeafId id = by_name(root, schematic, leaf);
                if (id.valid())
                    results.push_back(id);
            }
        }
        return results;
    }

    /// @brief resolve_hierarchical_path<InstanceId>, ready to call - the
    /// form `link`'s Placement-matching pass and TCL's `get_instances`
    /// both use.
    inline std::vector<InstanceId> resolve_instances(const Root &root, SchematicId root_schematic,
                                                       const std::vector<std::string> &segments)
    {
        return resolve_hierarchical_path<InstanceId>(
            root, root_schematic, segments,
            [](const Root &r, SchematicId s, const std::string &n) { return r.get_instance_by_name(s, n); },
            [](const Root &r, SchematicId s) -> const std::vector<InstanceId> & { return r.get_schematic_instances(s); },
            [](const Root &r, InstanceId id) -> const std::string & { return r.get_instance(id)->name; });
    }

    /// @brief resolve_hierarchical_path<NetId>, ready to call - the form
    /// `link`'s Route/PhysicalPort-matching pass and TCL's `get_nets` both
    /// use.
    inline std::vector<NetId> resolve_nets(const Root &root, SchematicId root_schematic,
                                            const std::vector<std::string> &segments)
    {
        return resolve_hierarchical_path<NetId>(
            root, root_schematic, segments,
            [](const Root &r, SchematicId s, const std::string &n) { return r.get_net_by_name(s, n); },
            [](const Root &r, SchematicId s) -> const std::vector<NetId> & { return r.get_schematic_nets(s); },
            [](const Root &r, NetId id) -> const std::string & { return r.get_net(id)->name; });
    }

    /// @brief resolve_hierarchical_path<PortId>, ready to call - the form
    /// TCL's `get_ports` uses.
    inline std::vector<PortId> resolve_ports(const Root &root, SchematicId root_schematic,
                                              const std::vector<std::string> &segments)
    {
        return resolve_hierarchical_path<PortId>(
            root, root_schematic, segments,
            [](const Root &r, SchematicId s, const std::string &n) { return r.get_port_by_name(s, n); },
            [](const Root &r, SchematicId s) -> const std::vector<PortId> & { return r.get_schematic_ports(s); },
            [](const Root &r, PortId id) -> const std::string & { return r.get_port(id)->name; });
    }
}
