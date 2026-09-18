#pragma once

// Generates "stub" Verilog module declarations (a port list plus an
// empty body, no functional content) for every Design in a Library that
// has an Abstract (a LEF-read physical view) but no Schematic (no real
// Verilog/SystemVerilog was ever read for it) - the common
// standard-cell/macro situation for a gate-level netlist read against a
// LEF-only cell library (BUF_X1, XNOR2_X1, an SRAM macro, ...).
//
// Why this exists: SVReader::read_netlist already tolerates an
// unresolved module reference without failing the read (see
// sv_reader.cpp's own "populate_pins_for_uninstantiated" - it recovers a
// real Instance/Pin/Net set from raw syntax, and SVReader::
// link_unresolved_instances later links Instance.reference_design to
// the matching LEF-only Design purely by name, no stub involved) - but
// slang still logs a real "unknown module" diagnostic for every such
// instantiation, and the raw-syntax fallback can't resolve a bus
// connection (`.addr_in(some_bus)`) as precisely as real elaboration
// against a real port declaration can. Feeding a generated stub file
// into the *same* SVReader::read_netlist call as the real netlist
// file(s) (they share one slang::ast::Compilation only within one call -
// see that function's own body) lets slang elaborate these
// instantiations for real, with no diagnostic noise and accurate
// bit-width-aware connections.
//
// SRAM-macro caveat this module exists to handle correctly: LEF PIN
// names for a multi-bit macro port are individually bracket-suffixed
// ("addr_in[0]", "addr_in[1]", ...) - LEFReader never transforms
// Terminal.name, so that's exactly what's stored (see LEFReader::
// lefrPinCbkFn). A real netlist instantiating that macro almost always
// connects the *bus* as one port (`.addr_in(some_bus)`), so a stub that
// declared 8 separate scalar ports named "addr_in[0]".."addr_in[7]"
// would fail to elaborate that connection (Verilog port names can't
// contain literal brackets unless escaped, and an escaped-identifier
// port still wouldn't be what a bus-connecting instantiation names).
// group_terminals_into_ports below detects a contiguous, direction-
// consistent run of bracket-suffixed Terminals sharing one base name and
// combines them into a single `[msb:lsb]` vector port instead.

#include "../database/database.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace le
{
    namespace verilog_stub_detail
    {
        /// @brief Peels a trailing "[<digits>]" suffix off `name` (e.g.
        /// "addr_in[3]" -> {"addr_in", 3}). Returns std::nullopt if
        /// `name` has no such suffix (a plain scalar Terminal name, or
        /// one that merely ends in ']' without a purely-numeric index -
        /// e.g. a LEF pin legitimately named with a literal bracket
        /// isn't touched). Mirrors SVReader's own
        /// peel_trailing_bracket_indices (sv_reader.cpp) - deliberately
        /// not shared with it: that one peels *every* trailing bracket
        /// group for a multi-dimensional net/port reference on the
        /// Verilog side, this one only ever needs the single outermost
        /// group a LEF PIN name can have.
        inline std::optional<std::pair<std::string, int>> peel_trailing_bracket_index(const std::string &name)
        {
            if (name.size() < 3 || name.back() != ']')
                return std::nullopt;
            const size_t open = name.rfind('[');
            if (open == std::string::npos || open + 2 >= name.size())
                return std::nullopt;
            const std::string digits = name.substr(open + 1, name.size() - open - 2);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                                [](unsigned char c) { return std::isdigit(c); }))
                return std::nullopt;
            return std::make_pair(name.substr(0, open), std::stoi(digits));
        }

        /// @brief Maps a LEF Terminal's SignalDirection onto the nearest
        /// Verilog port direction keyword. Verilog only has
        /// input/output/inout; OUTPUT_TRISTATE and FEEDTHRU (LEF-only
        /// concepts with no Verilog equivalent) map to the direction a
        /// real instantiation would still connect them the same way as -
        /// output for OUTPUT_TRISTATE (a real netlist drives it like any
        /// other output), inout for FEEDTHRU and the unspecified NONE
        /// case (the maximally permissive choice - a stub only needs to
        /// let a real connection elaborate, not enforce LEF's own
        /// directionality).
        inline const char *verilog_direction_keyword(SignalDirection direction)
        {
            switch (direction)
            {
            case SignalDirection::INPUT:
                return "input";
            case SignalDirection::OUTPUT:
            case SignalDirection::OUTPUT_TRISTATE:
                return "output";
            case SignalDirection::INOUT:
            case SignalDirection::NONE:
            case SignalDirection::FEEDTHRU:
            default:
                return "inout";
            }
        }

        /// @brief True iff `name` is a simple Verilog identifier (starts
        /// with a letter or underscore, every character alnum/underscore/
        /// $) - the common case for every plain LEF PIN name and every
        /// bus base name this module synthesizes. A name that doesn't
        /// qualify (e.g. one LEF legitimately allows but Verilog
        /// wouldn't accept unescaped) is instead declared as a Verilog
        /// escaped identifier (backslash-prefixed, whitespace-
        /// terminated) by the caller - a real, if unusual, Verilog
        /// construct that can spell any name verbatim, including one
        /// containing brackets.
        inline bool is_simple_verilog_identifier(const std::string &name)
        {
            if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_'))
                return false;
            return std::all_of(name.begin(), name.end(), [](unsigned char c)
                                { return std::isalnum(c) || c == '_' || c == '$'; });
        }

        /// @brief `name` as a Verilog identifier token, escaping it
        /// (`\name<space>`) when it isn't already a simple one - see
        /// is_simple_verilog_identifier's own comment.
        inline std::string verilog_identifier(const std::string &name)
        {
            if (is_simple_verilog_identifier(name))
                return name;
            return "\\" + name + " ";
        }

        /// @brief One stub module port - a plain scalar Terminal, or a
        /// bus formed by combining a contiguous, direction-consistent
        /// run of bracket-suffixed Terminals sharing one base name
        /// (bit_range = {msb, lsb}, msb >= lsb).
        struct StubPort
        {
            std::string name;
            SignalDirection direction;
            std::optional<std::pair<int, int>> bit_range;
        };

        /// @brief Groups `terminal_ids` (in declaration order, as
        /// Root::get_abstract_terminals already returns them) into a
        /// stub port list. A base name's bracket-suffixed Terminals
        /// combine into one bus port only when their indices form one
        /// contiguous run (any msb/lsb, not just 0-based) and every bit
        /// shares the same direction - the shape every real LEF-authored
        /// bus pin set actually has. A base name whose bits are
        /// non-contiguous or direction-inconsistent (unusual, but not
        /// impossible) falls back to one scalar port per bit instead of
        /// silently guessing a range - see verilog_identifier for how
        /// that bracketed name still gets declared as an exact-match
        /// escaped identifier.
        inline std::vector<StubPort> group_terminals_into_ports(const Root &root,
                                                                  const std::vector<TerminalId> &terminal_ids)
        {
            struct BusGroup
            {
                std::vector<std::pair<int, SignalDirection>> bits;
            };

            // First pass: collect every bus base's full bit set (a base
            // name's Terminals aren't necessarily contiguous in
            // `terminal_ids` in a pathological input, though they always
            // are for a real LEF read), while also recording the
            // declaration-order position of each entry - a scalar
            // Terminal directly, or (only at its first sighting) a bus
            // base name - so the second pass below can emit ports in the
            // same order the Abstract's own Terminals were declared,
            // whichever kind each position turns out to be.
            struct Entry
            {
                bool is_bus;
                TerminalId terminal_id; // valid when !is_bus
                std::string bus_base;   // valid when is_bus
            };

            std::vector<Entry> entries;
            std::unordered_map<std::string, BusGroup> buses;
            std::unordered_map<std::string, bool> bus_seen;

            for (TerminalId id : terminal_ids)
            {
                const TerminalData *terminal = root.get_terminal(id);
                if (!terminal)
                    continue;

                if (auto peeled = peel_trailing_bracket_index(terminal->name))
                {
                    const std::string &base = peeled->first;
                    if (!bus_seen[base])
                    {
                        bus_seen[base] = true;
                        entries.push_back(Entry{.is_bus = true, .terminal_id = {}, .bus_base = base});
                    }
                    buses[base].bits.emplace_back(peeled->second, terminal->direction);
                    continue;
                }

                entries.push_back(Entry{.is_bus = false, .terminal_id = id, .bus_base = {}});
            }

            // Second pass: emit ports in that same order, resolving each
            // bus entry against its now-complete bit set.
            std::vector<StubPort> ports;
            ports.reserve(entries.size());
            for (const Entry &entry : entries)
            {
                if (!entry.is_bus)
                {
                    const TerminalData *terminal = root.get_terminal(entry.terminal_id);
                    ports.push_back(StubPort{.name = terminal->name, .direction = terminal->direction, .bit_range = std::nullopt});
                    continue;
                }

                const BusGroup &group = buses.at(entry.bus_base);
                const auto [lo_it, hi_it] = std::minmax_element(
                    group.bits.begin(), group.bits.end(),
                    [](const auto &a, const auto &b) { return a.first < b.first; });
                const int lo = lo_it->first;
                const int hi = hi_it->first;
                const bool contiguous = (hi - lo + 1) == static_cast<int>(group.bits.size());
                const SignalDirection first_direction = group.bits.front().second;
                const bool direction_consistent = std::all_of(
                    group.bits.begin(), group.bits.end(),
                    [&](const auto &bit) { return bit.second == first_direction; });

                if (contiguous && direction_consistent)
                {
                    ports.push_back(StubPort{.name = entry.bus_base, .direction = first_direction, .bit_range = std::make_pair(hi, lo)});
                    continue;
                }

                std::vector<std::pair<int, SignalDirection>> sorted_bits = group.bits;
                std::sort(sorted_bits.begin(), sorted_bits.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });
                for (const auto &[index, direction] : sorted_bits)
                    ports.push_back(StubPort{.name = fmt::format("{}[{}]", entry.bus_base, index), .direction = direction, .bit_range = std::nullopt});
            }

            return ports;
        }
    }

    /// @brief Generates stub Verilog source (one empty-bodied module per
    /// Abstract-only Design in `library_id`) - see this header's own
    /// top-of-file comment for why. A Design with a real Schematic
    /// (already read from real Verilog) is skipped - it needs no stub.
    /// Returns an empty string if `library_id` doesn't resolve or has no
    /// Abstract-only Designs.
    inline std::string generate_verilog_stubs(const Root &root, LibraryId library_id)
    {
        std::string out;
        for (DesignId design_id : root.get_library_designs(library_id))
        {
            const AbstractId abstract_id = root.get_design_abstract(design_id);
            if (!abstract_id.valid() || root.get_design_schematic(design_id).valid())
                continue;

            const DesignData *design = root.get_design(design_id);
            if (!design)
                continue;

            const std::vector<verilog_stub_detail::StubPort> ports =
                verilog_stub_detail::group_terminals_into_ports(root, root.get_abstract_terminals(abstract_id));

            out += fmt::format("module {} (", verilog_stub_detail::verilog_identifier(design->name));
            for (size_t i = 0; i < ports.size(); ++i)
                out += fmt::format("{}{}", i == 0 ? "" : ", ", verilog_stub_detail::verilog_identifier(ports[i].name));
            out += ");\n";

            for (const auto &port : ports)
            {
                const char *keyword = verilog_stub_detail::verilog_direction_keyword(port.direction);
                const std::string ident = verilog_stub_detail::verilog_identifier(port.name);
                if (port.bit_range)
                    out += fmt::format("    {} [{}:{}] {};\n", keyword, port.bit_range->first, port.bit_range->second, ident);
                else
                    out += fmt::format("    {} {};\n", keyword, ident);
            }

            out += "endmodule\n\n";
        }
        return out;
    }
}
