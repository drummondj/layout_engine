#pragma once
#include <string>
#include <vector>
#include "../database/database.hpp"
#include "slang/ast/SemanticFacts.h"
#include "spdlog/spdlog.h"

namespace le
{
    /// @brief Reads SystemVerilog/Verilog source into the logical
    /// connectivity data model (Schematic/Port/Net/Instance/Pin), using
    /// the slang frontend (see SYSTEMVERILOG.md and its linked plan).
    /// Two entry points, matching this project's read_lef/read_def
    /// convention of one function per distinct reading mode: read_netlist
    /// (full elaboration, for gate-level netlists where parameter/
    /// generate resolution must be accurate) and read_rtl (syntax-only,
    /// tolerant of invalid/unsupported content - see Instance.rtl_text).
    class SVReader
    {
    public:
        /// @brief Full-elaboration flavor. Builds a slang::ast::Compilation
        /// over `filenames` together, force-elaborating the design so
        /// parameter/generate-block resolution is accurate (instance
        /// names/counts reflect real elaboration, not source text). Per
        /// distinct module definition: populates Port/Net from its port
        /// list, and one Instance+Pin set per elaborated instance under
        /// it. A module instantiated but never defined in `filenames`
        /// (a leaf/standard cell, the common case for real gate-level
        /// netlists) gets an Instance with reference_name set and
        /// reference_design left unresolved - see link_unresolved_instances.
        /// Calls link_unresolved_instances itself at the end. Returns 0 on
        /// success (matching read_lef/read_def's convention), nonzero if
        /// no usable design was read at all.
        int read_netlist(std::vector<std::string> filenames, Root &root, std::string library_name);

        /// @brief Syntax-only flavor (slang::syntax::SyntaxTree, no
        /// elaboration/parameter resolution) - tolerates invalid or
        /// unsupported content. Two passes: first collects every ANSI-
        /// style module port list (name/direction only, no bit width -
        /// unlike read_netlist, there's no elaborated Type to ask, and a
        /// parameter-driven width isn't evaluated) across all `filenames`
        /// together, so a positional connection instantiating a module
        /// defined in a different file of this same read still resolves;
        /// second pass populates each module's Design/Schematic. A module
        /// whose own header isn't ANSI-style or has a diagnostic in its
        /// own range becomes a single logic-cloud Instance covering the
        /// whole body (Instance.rtl_text set, no Ports). Otherwise, each
        /// top-level member is either a diagnostic-clean module
        /// instantiation (extracted as a real Instance + Pins - named and
        /// positional connections both handled, matched by name/header
        /// order respectively) or becomes its own logic-cloud Instance
        /// (procedural blocks, assigns, generate constructs, or an
        /// instantiation with its own diagnostic). A connection's value is
        /// classified from its own verbatim source text (a bare
        /// identifier, or identifier[literal-digits] for a bit-select) -
        /// anything else (concatenation, a non-literal index, a constant
        /// tie) becomes Pin.raw_expression only, same fallback rule as
        /// read_netlist. Calls link_unresolved_instances itself at the
        /// end. Returns 0 on success, nonzero if no file could be read.
        int read_rtl(std::vector<std::string> filenames, Root &root, std::string library_name);

        /// @brief Re-resolves Instance.reference_design for every Instance
        /// in `root` whose reference_design is currently unset, matching
        /// reference_name against Design.name. Separate/re-runnable (not
        /// just inline at the end of a read) because it must also pick up
        /// Designs created by a LATER read_netlist/read_rtl/read_lef call
        /// on the same Root - the normal real flow is read the netlist,
        /// then read the leaf-cell LEF, then re-link. Returns the number
        /// of Instances newly resolved by this call.
        static size_t link_unresolved_instances(Root &root);

        /// @brief Messages produced by the most recent read_netlist()/
        /// read_rtl() call - slang diagnostics (already formatted) plus
        /// this class's own synthesized messages. Cleared and repopulated
        /// at the start of every read_netlist()/read_rtl() call, same
        /// convention as LEFReader::messages().
        const std::vector<std::string> &messages() const { return messages_; }

        // Pure conversion - public/static, no dependency on reader
        // instance state, for direct unit testing (same convention as
        // LEFReader::signal_direction_from_parser).
        static SignalDirection signal_direction_from_parser(slang::ast::ArgumentDirection direction);

    private:
        Root *root_ = nullptr;
        LibraryId library_id_;
        std::vector<std::string> messages_;
    };
}
