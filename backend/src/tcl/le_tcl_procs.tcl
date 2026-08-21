# Thin -flag parsing layer over the SWIG-wrapped *_cmd shim functions
# (le_tcl_shim.hpp/.cpp) - Tcl's own `-flag value` calling convention has
# to be parsed here, since SWIG-wrapped C++ functions are always
# positional. See TCL_EXPLORATION.md's "Tcl ergonomics layer" section for
# why this split exists (C++ shim owns session state + command logic,
# Tcl owns flag parsing) rather than putting everything in one place.
# Sourced by anything that loads the le_tcl module and wants the
# ergonomic (item 15 -shaped) command surface rather than the raw *_cmd
# forms.
#
# Also owns every piece of Phase 5's CRUD/search surface that's better
# built in Tcl than in C++: property tables and search-result/shape-list
# aggregation (properties_for_token, shape_rects, ...) loop over the
# shim's plain count+by-index accessors and build a real Tcl dict/list
# with `dict set`/`lappend` - correct quoting by construction, unlike
# hand-rolled string-building in C++ (see le_tcl_shim.hpp's own
# "property tables and search results" comment for why that split was
# made). Coordinate lists themselves (`-points {x y x y ...}`) need no
# such treatment here - le_api.i's typemap already turns a plain Tcl list
# into the shim's (const double*, int32_t count) pair directly.

set kInvalidId 4294967295

# --- Editing / undo-redo (UPDATES.md item 21) ---
#
# Wraps one user-typed command with undo/redo transaction recording +
# command-recall logging - the single bracket point every REPL-style
# caller should use instead of raw Tcl_Eval, so a typed command is
# exactly as undoable (Ctrl-Z/Ctrl-Shift-Z) as a GUI edit like Move, and
# only successfully-run commands (`code == 0`) get added to
# command_history. `uplevel #0` runs $command in the *global* scope, not
# nested inside this proc's own local one - matching how a real
# interactive shell evaluates each line at toplevel (a bare `set x 5`
# lands in global scope, not thrown away when this proc returns).
#
# `flutter_plugin`'s LeTclBridge.mm is the only caller (every typed
# console command goes through it) - le_shell.cpp's own interactive REPL
# is deliberately *not* wired through this yet, since it doesn't route
# through a caller-controlled eval loop the way LeTclBridge.mm does; a
# command typed at that shell is undoable/recorded at the individual
# create/update/delete level (the generic per-mutation hook still fires),
# just not batched into one transaction/recall entry per line the way a
# Flutter-console command is.
proc le_repl_eval {command} {
    begin_command $command
    set code [catch {uplevel #0 $command} result]
    end_command [expr {$code == 0}]
    return $result
}

# --- Help system (UPDATES.md item 20) ---
#
# ::command_help maps a command name to a {usage <str> description <str>
# options <list>} dict - register_command_help below is the single write
# path, called once per command right after its own `proc` definition.
# Every generated command (get_<type>/create_<type>/update_<type>)
# registers itself from generated/le_tcl_procs_generated.tcl, sourced
# further down this file; every hand-written command below registers
# itself directly, right after its own definition. help/man/
# complete_command/generate_command_docs all read purely from this
# registry - none of them re-derive metadata by invoking a command with
# -help.
#
# An `options` entry is `{-flag {type T required R description {D}}}` (or
# `{<positional> {...}}` for a plain positional parameter, which
# documents the same way but is never treated as a `-`-flag by
# complete_command's own flag-completion branch, since it doesn't start
# with `-`). `required`/`type` are always present; `type` is a
# human-readable label only (e.g. "str"/"int"/"token"/"Point..."), not
# machine-validated here - real validation still happens inside each
# command's own body, same as before this system existed.
set ::command_help [dict create]

proc register_command_help {name usage description options} {
    dict set ::command_help $name [dict create usage $usage description $description options $options]
}

# `help ?pattern?` - one line per registered command whose name matches
# `pattern` (Tcl `string match` glob syntax, default `*` - every
# command), e.g. `help get_*` lists every search command. Just the bare
# command name (no argument/flag syntax - see `man <name>` for that) and
# its own description, names padded to the longest match so every
# description column lines up. Returns the joined text (not puts - see
# le_tcl_procs.tcl's own get_properties for why: usable both
# interactively and captured into a variable).
proc help {{pattern *}} {
    set names [lsort [dict keys $::command_help]]
    set matches [lsearch -all -inline -glob $names $pattern]
    if {[llength $matches] == 0} {
        return "help: no commands match \"$pattern\""
    }
    set max_len 0
    foreach name $matches {
        if {[string length $name] > $max_len} {
            set max_len [string length $name]
        }
    }
    set lines {}
    foreach name $matches {
        set description [dict get $::command_help $name description]
        lappend lines [format "%-*s  %s" $max_len $name $description]
    }
    return [join $lines "\n"]
}

# The `<command> <args...>` portion of a registered usage string, with
# its own trailing ` - <description>` dropped - every usage string ends
# with that suffix (see register_command_help's own callers), so man/
# generate_command_docs below can show the syntax and the (fuller, not
# truncated to one line) description as two distinct sections without
# printing the same description text twice. Splits on the *first* " - "
# - safe because no flag/type fragment a usage string's own syntax
# portion ever contains that exact substring, only the description
# suffix does.
proc _usage_syntax {usage} {
    set idx [string first " - " $usage]
    if {$idx < 0} {
        return $usage
    }
    return [string range $usage 0 [expr {$idx - 1}]]
}

# `man <name>` - the full registered page for one command: its own
# `<command> <args...>` syntax line, blank line, description, then (if
# any options are registered) an Options: table - one line per flag/
# positional with its type, required/optional-ness, and description.
proc man {name} {
    if {![dict exists $::command_help $name]} {
        error "man: no such command \"$name\" - see \[help\] for the full list"
    }
    set info [dict get $::command_help $name]
    set lines {}
    lappend lines [_usage_syntax [dict get $info usage]]
    lappend lines ""
    lappend lines [dict get $info description]
    set options [dict get $info options]
    if {[llength $options] > 0} {
        lappend lines ""
        lappend lines "Options:"
        # Each option's own "<flag> <type> (required/optional)" label
        # varies in length, so build them all first and pad every one to
        # the longest before appending its description - otherwise the
        # description column starts at a different place on every line.
        set labels {}
        foreach opt $options {
            lassign $opt flag meta
            set type [dict get $meta type]
            set required [expr {[dict get $meta required] ? "required" : "optional"}]
            lappend labels "$flag <$type> ($required)"
        }
        set max_len 0
        foreach label $labels {
            if {[string length $label] > $max_len} {
                set max_len [string length $label]
            }
        }
        foreach opt $options label $labels {
            set desc [dict get [lindex $opt 1] description]
            lappend lines [format "  %-*s  %s" $max_len $label $desc]
        }
    }
    return [join $lines "\n"]
}

# `complete_command <line>` - candidate replacements for the
# whitespace-delimited token currently being typed at the end of `line`
# (a command name, a -flag, or a .-prefixed property path), as a sorted
# Tcl list (empty if nothing matches or the token being completed isn't
# one of those three kinds - e.g. a plain positional value like a
# friendly-id token isn't attempted, since suggesting real object tokens
# would mean actually running a query while the user is still typing,
# not a safe/generic thing to do speculatively). Every candidate is a
# *full* replacement for that last token, not just a suffix, so the
# caller's own splice logic ("replace the last token with the chosen
# candidate") stays uniform across every completion kind. Pure
# static-metadata lookup (::command_help/::property_scalars/
# ::property_hops) - never runs the command/query being completed.
#
# Always returns via `join` (a plain space-separated string), never a
# raw Tcl list value directly - a property-path candidate can itself
# start with an unbalanced "{" (see the -filter branch below), and a
# real Tcl list's own canonical string form backslash-escapes an
# unbalanced brace inside an element to stay re-parseable (harmless
# to Tcl itself, but this crosses into the GUI console as plain text via
# Tcl_Eval's own string result - see flutter_plugin's LeTclConsole/
# LeTclBridge - where a naive caller splitting on whitespace would then
# see a literal, wrong leading backslash). `join`'s output has no such
# escaping (it's a flat concatenation, not a list's own string
# representation), so the plain-text contract stays exactly what every
# caller (this file's own tests, the GUI) actually relies on.
proc complete_command {line} {
    set tokens [regexp -all -inline {\S+} $line]
    set ends_with_space [expr {
        [string length $line] > 0 && [string is space [string index $line end]]
    }]

    if {[llength $tokens] == 0 || (!$ends_with_space && [llength $tokens] == 1)} {
        # Completing the command name itself - nothing typed yet, or
        # exactly one still-partial token with no trailing space.
        set partial [expr {[llength $tokens] == 0 ? "" : [lindex $tokens end]}]
        return [join [lsort [lsearch -all -inline -glob [dict keys $::command_help] "${partial}*"]]]
    }

    set partial [expr {$ends_with_space ? "" : [lindex $tokens end]}]
    # Every already-complete token on the line, i.e. every token except
    # the partial one currently being typed - all of $tokens when
    # $ends_with_space (nothing partial yet), otherwise all but the last.
    set complete_tokens [expr {$ends_with_space ? $tokens : [lrange $tokens 0 end-1]}]
    # $command_name is whichever command's own *arguments* the partial
    # token is currently completing - not always $tokens' own first
    # token: once a nested command's name has already been typed inside
    # "[...]" (e.g. `set t [get_terminals -f`), -flag/dot-path
    # completion below needs *that* command's own options, not the
    # outer one's. _innermost_command_start finds where the innermost
    # still-open bracket's own command name begins; with no bracket
    # open at all it's just index 0, the outer command, unchanged from
    # before this existed.
    set command_name [string trimleft \
        [lindex $complete_tokens [_innermost_command_start $complete_tokens]] "\["]

    # A command name can also start right after "[" - Tcl command
    # substitution nested inside another command's own argument, e.g.
    # `set t [get_` (the leading bracket glues onto whatever follows it
    # with no space, same as a -filter value's own leading brace
    # character below) or `set t [ get_` (whitespace after the bracket
    # makes it its own complete token instead). Either way this is the
    # start of a brand new
    # command, not a continuation of $command_name's own arguments, so
    # it gets its own top-level command-name completion, same as the
    # very first token of the whole line - checked before the -flag/
    # dot-path branches below, since those only make sense once we're
    # actually still inside $command_name's own argument list.
    set bracket_prefix ""
    set command_partial $partial
    while {[string index $command_partial 0] eq "\["} {
        append bracket_prefix "\["
        set command_partial [string range $command_partial 1 end]
    }
    set after_lone_bracket [expr {
        $bracket_prefix eq "" && [llength $complete_tokens] > 0
        && [lindex $complete_tokens end] eq "\["
    }]
    if {$bracket_prefix ne "" || $after_lone_bracket} {
        set matches [lsort [lsearch -all -inline -glob [dict keys $::command_help] "${command_partial}*"]]
        set candidates {}
        foreach match $matches {
            lappend candidates "${bracket_prefix}${match}"
        }
        return [join $candidates]
    }

    if {[string index $partial 0] eq "-"} {
        if {![dict exists $::command_help $command_name]} {
            return {}
        }
        set flags {}
        foreach opt [dict get $::command_help $command_name options] {
            lappend flags [lindex $opt 0]
        }
        return [join [lsort [lsearch -all -inline -glob $flags "${partial}*"]]]
    }

    # A dot-path can be completed in two different argument shapes:
    # get_properties/report_properties take one bare (each one always
    # its own whitespace-delimited token), while a get_<type> command's
    # own -filter expression embeds one or more inside a single braced
    # list argument, e.g. -filter {.direction == INPUT}. That opening
    # brace glues onto whatever follows it with no space (this
    # tokenizer only splits on whitespace), so the very first segment
    # arrives here as one token starting with a brace, not a dot -
    # strip any leading braces before checking for the dot both shapes
    # share, and remember them to re-prepend to every candidate, so a
    # candidate is still a full replacement for the actual token being
    # typed, braces included.
    set brace_prefix ""
    set dot_partial $partial
    while {[string index $dot_partial 0] eq "\{"} {
        append brace_prefix "\{"
        set dot_partial [string range $dot_partial 1 end]
    }

    if {[string index $dot_partial 0] eq "."} {
        set class_key {}
        if {$command_name in {get_properties report_properties}} {
            set class_key [_property_path_seed_class $complete_tokens]
        } elseif {[info exists ::get_command_class($command_name)]
                && [_partial_is_inside_filter_value $complete_tokens]} {
            set class_key $::get_command_class($command_name)
        }
        if {$class_key ne {}} {
            set candidates {}
            foreach candidate [_property_path_candidates $class_key $dot_partial] {
                lappend candidates "${brace_prefix}${candidate}"
            }
            return [join $candidates]
        }
    }

    return {}
}

# The index into complete_tokens where the innermost currently-open
# "[...]" command-substitution scope's own command name begins (the
# bracket character itself not included), for complete_command's own
# $command_name above - a nested command's own -flag/dot-path
# completion (e.g. `set t [get_terminals -f`) needs *its* options, not
# the outer command's. 0 (the very first token, i.e. the outer command
# itself) if no bracket is currently open.
#
# Scans char-by-char within each token, not just each token's first/
# last character, so a bracket that opens and/or closes mid-token (e.g.
# `[get_terminals]` fully self-contained in one token, or one appearing
# inside a -filter expression's own value) is still tracked correctly -
# every other part of complete_command already reasons at whole-token
# granularity, so this only needs to know *which token* a scope starts/
# ends in, not an exact character offset. A "[" glued to the front of
# the token it opens in (no space) makes that same token the new scope's
# own start; a "[" that's the very last character of its token (space
# before the next word) makes the *next* token the start instead -
# mirrors complete_command's own bracket_prefix/after_lone_bracket
# handling for a partial token that itself opens a new command.
proc _innermost_command_start {complete_tokens} {
    set stack {}
    set n [llength $complete_tokens]
    for {set i 0} {$i < $n} {incr i} {
        set token [lindex $complete_tokens $i]
        set chars [split $token {}]
        set clen [llength $chars]
        for {set j 0} {$j < $clen} {incr j} {
            set ch [lindex $chars $j]
            if {$ch eq "\["} {
                if {$j < $clen - 1} {
                    lappend stack $i
                } else {
                    lappend stack [expr {$i + 1}]
                }
            } elseif {$ch eq "\]" && [llength $stack] > 0} {
                set stack [lrange $stack 0 end-1]
            }
        }
    }
    if {[llength $stack] == 0} {
        return 0
    }
    return [lindex $stack end]
}

# Whether the partial token currently being completed is inside a
# `-filter <expr>` value, for complete_command's own get_<type> branch
# above - true iff the most recent already-typed `-`-prefixed token
# (scanning backward) is literally "-filter", not some other flag (whose
# own value we're still inside) or a filter-expression token that merely
# starts with "-" (e.g. a negative number literal - a rare, accepted
# miss: this degrades to "no completion offered", never a wrong one).
proc _partial_is_inside_filter_value {complete_tokens} {
    foreach token [lreverse $complete_tokens] {
        if {[string index $token 0] eq "-"} {
            return [expr {$token eq "-filter"}]
        }
    }
    return 0
}

# The dot-path completion seed for get_properties/report_properties -
# the most recent earlier token (scanning backward) matching a
# friendly-id token (kind:value), or "" if none does. get_<type>'s own
# -filter branch above needs no scan at all: ::get_command_class already
# names its class directly.
proc _property_path_seed_class {complete_tokens} {
    foreach token [lreverse $complete_tokens] {
        if {[regexp {^([a-z_]+):} $token whole_match prefix] && [info exists ::property_scalars($prefix)]} {
            return $prefix
        }
    }
    return {}
}

# Dot-hop property-path completion, given the class already known to
# start from (get_properties/report_properties's own friendly-id-token
# seed via _property_path_seed_class, or a get_<type> command's own
# class via ::get_command_class) and the .-prefixed path fragment
# currently being completed (e.g. ".terminal_port.na"). Follows each
# already-typed hop segment through ::property_hops one at a time - an
# unresolvable segment yields no candidates (an invalid path so far),
# matching resolve_property_path's own error behavior. Every returned
# candidate is the *full* path (resolved prefix + matched leaf/hop
# name), not just the trailing segment, so complete_command's "replace
# the last token" contract stays uniform.
proc _property_path_candidates {class_key partial} {
    # partial always starts with "." - drop it, then split on "." to get
    # every already-complete hop segment plus the final (possibly empty)
    # segment still being typed.
    set segments [split [string range $partial 1 end] "."]
    set final_segment [lindex $segments end]
    set hop_segments [lrange $segments 0 end-1]

    set resolved_prefix "."
    foreach hop $hop_segments {
        set next_key {}
        if {[info exists ::property_hops($class_key)]} {
            foreach pair $::property_hops($class_key) {
                lassign $pair hop_name target_key
                if {$hop_name eq $hop} {
                    set next_key $target_key
                    break
                }
            }
        }
        if {$next_key eq {}} {
            return {}
        }
        set class_key $next_key
        append resolved_prefix "${hop}."
    }

    set candidates {}
    if {[info exists ::property_scalars($class_key)]} {
        foreach name [lsearch -all -inline -glob $::property_scalars($class_key) "${final_segment}*"] {
            lappend candidates "${resolved_prefix}${name}"
        }
    }
    if {[info exists ::property_hops($class_key)]} {
        foreach pair $::property_hops($class_key) {
            set hop_name [lindex $pair 0]
            if {[string match "${final_segment}*" $hop_name]} {
                lappend candidates "${resolved_prefix}${hop_name}"
            }
        }
    }
    return [lsort $candidates]
}

# `generate_command_docs ?path?` - one Markdown string covering every
# registered command (usage/description/options table), in name order;
# if `path` is non-empty, also writes it there. Always returns the full
# text either way. See backend's generate-tcl-docs skill for the
# recipe that regenerates backend/TCL_COMMANDS.md from this.
proc generate_command_docs {{path {}}} {
    set lines {}
    lappend lines "# TCL Command Reference"
    lappend lines ""
    lappend lines "Generated by generate_command_docs (le_tcl_procs.tcl) - do not edit by hand."
    lappend lines ""
    foreach name [lsort [dict keys $::command_help]] {
        set info [dict get $::command_help $name]
        lappend lines "## $name"
        lappend lines ""
        lappend lines "`[_usage_syntax [dict get $info usage]]`"
        lappend lines ""
        lappend lines [dict get $info description]
        set options [dict get $info options]
        if {[llength $options] > 0} {
            lappend lines ""
            lappend lines "| Flag | Type | Required | Description |"
            lappend lines "| --- | --- | --- | --- |"
            foreach opt $options {
                lassign $opt flag meta
                set type [dict get $meta type]
                set required [expr {[dict get $meta required] ? "yes" : "no"}]
                set desc [dict get $meta description]
                lappend lines "| \`$flag\` | \`$type\` | $required | $desc |"
            }
        }
        lappend lines ""
    }
    set text [join $lines "\n"]
    if {$path ne {}} {
        set fh [open $path w]
        puts $fh $text
        close $fh
    }
    return $text
}

# --- undo/redo/command_history (UPDATES.md item 21) - the raw undo_command/
# redo_command/command_history_count/command_history_at shim functions
# (le_tcl_shim.hpp) are named with a suffix specifically so these procs
# can be the real `undo`/`redo`/`command_history` Tcl commands without
# shadowing (and thereby recursing into) the shim's own SWIG-bound ones. ---

proc undo {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "undo \[-help\] - Undoes the most recently recorded command or edit"
    }
    return [expr {[undo_command] ? "1" : "0"}]
}
register_command_help undo \
    "undo \[-help\] - Undoes the most recently recorded command or edit" \
    "Undoes the most recently recorded transaction (a typed command or a GUI edit like Move), if any. Returns 1 if something was undone, 0 otherwise." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc redo {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "redo \[-help\] - Redoes the most recently undone command or edit"
    }
    return [expr {[redo_command] ? "1" : "0"}]
}
register_command_help redo \
    "redo \[-help\] - Redoes the most recently undone command or edit" \
    "Redoes the most recently undone transaction, if any. Returns 1 if something was redone, 0 otherwise." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc command_history {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "command_history \[-help\] - Lists successfully executed commands, in order"
    }
    set count [command_history_count]
    set lines {}
    for {set i 0} {$i < $count} {incr i} {
        lappend lines "$i: [command_history_at $i]"
    }
    return [join $lines "\n"]
}
register_command_help command_history \
    "command_history \[-help\] - Lists successfully executed commands, in order" \
    "Lists every successfully executed typed command, in submission order (one per line, numbered) - backs the console's Up/Down recall. A command that errored is not recorded." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc set_viewport_size {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "set_viewport_size -width <int> -height <int> \[-help\] - Sets the render viewport's pixel size"
    }
    array set opts {-width {} -height {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "set_viewport_size: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-width) eq {} || $opts(-height) eq {}} {
        error "set_viewport_size: -width and -height are required"
    }
    return [set_viewport_size_cmd $opts(-width) $opts(-height)]
}
register_command_help set_viewport_size \
    "set_viewport_size -width <int> -height <int> \[-help\] - Sets the render viewport's pixel size" \
    "Sets the render viewport's pixel size (used by le_render_pixel_buffer)." \
    {
        {-width {type int required 1 description {Viewport width, in pixels}}}
        {-height {type int required 1 description {Viewport height, in pixels}}}
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

# --- Current view (UPDATES.md item 17) ---

# Selects `name`'s Design as this session's current view - every
# subsequent get_terminals/get_obstructions/get_terminal_ports/get_shapes
# call (whose default scope, absent an explicit -of, derives from
# current_abstract - see codegen/codegen/tcl_scope.py's own module
# docstring) is scoped to its Abstract, since a script's "give me the
# terminals" means "in the view I have open", not "across every open
# Library/Design". Also moves Scene::current_abstract() (GUI rendering)
# the same way, via the shared le_set_current_design_by_id both this and
# the GUI's own design-selection path (LeProvider.openDesign) call into -
# selecting a Design means the same thing regardless of which side asked
# (see that function's own comment in api.cpp). `-view` is accepted but
# currently only "abstract" is meaningful - every Design read via
# read_lef() has exactly one Abstract view and no DEF/placement-driven
# Design exists in this project yet (see le_tcl_shim.hpp's
# design_abstract_id comment for the same caveat).
proc open_design {name args} {
    # $name is checked too, not just $args: open_design takes a
    # *mandatory* leading positional (name), so calling it as bare
    # `open_design -help` (no real design name supplied at all) binds
    # "-help" to $name, leaving $args empty - the same class of bug
    # found (and fixed the same way) in every generated update_<type>.
    if {$name eq "-help" || [lsearch -exact $args "-help"] >= 0} {
        return "open_design <name> \[-view abstract\] \[-help\] - Selects <name>'s Design as this session's current view"
    }
    array set opts {-view abstract}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "open_design: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-view) ne "abstract"} {
        error "open_design: -view $opts(-view) is not supported - only \"abstract\" is currently meaningful"
    }
    set design_id [design_by_name $name]
    if {$design_id == $::kInvalidId} {
        error "open_design: no such design \"$name\""
    }
    if {[set_current_design_cmd $design_id] != 0} {
        error "open_design: failed to select design \"$name\""
    }
    # set_current_design_cmd (le_set_current_design_by_id) already moves
    # the generated current_abstract along with it - no separate
    # get_abstracts/set_current_abstract step needed here anymore (see
    # that function's own comment in api.cpp).
    #
    # design:<name>, not the raw design_id, for consistency with UPDATES.md
    # item 19.1's own friendly-id convention - the caller already has
    # `name` literally, so this costs nothing to derive.
    return "design:$name"
}
register_command_help open_design \
    "open_design <name> \[-view abstract\] \[-help\] - Selects <name>'s Design as this session's current view" \
    "Selects a Design by name as this session's current view - every subsequent get_<type> call's default (-of omitted) scope derives from it." \
    {
        {<name> {type str required 1 description {Name of the Design to open}}}
        {-view {type str required 0 description {Only "abstract" is currently meaningful}}}
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

# --- get_<type> (UPDATES.md item 19.1) ---
#
# `get_<type> [<name-expr>...] [-of <parent-token>...] [-filter <expr>]
# [-help]` - one shared shape across every object type. parse_get_args
# tokenizes a proc's own `args` into that shape; each has_name_expr=0
# type (Abstract/TerminalPort/Obstruction/Shape - none have a name field,
# see UPDATES.md item 19.1's own NOTE) rejects a bare positional token
# instead of silently ignoring it. `-of`'s own value is itself a Tcl list
# (same idiom as `-rect {...}`/`-points {...}` elsewhere in this file) -
# `-of design:A` and `-of {design:A design:B}` both work, the latter OR'd
# (UPDATES.md item 19.1: "-of <parent tokens>" is plural on purpose).
proc parse_get_args {cmd_name args_list has_name_expr} {
    set name_exprs {}
    set of_tokens {}
    set filter {}
    set help 0

    set i 0
    set n [llength $args_list]
    while {$i < $n} {
        set token [lindex $args_list $i]
        if {$token eq "-help"} {
            set help 1
            incr i
        } elseif {$token eq "-of"} {
            if {$i + 1 >= $n} {
                error "$cmd_name: -of requires a value"
            }
            foreach t [lindex $args_list [expr {$i + 1}]] {
                lappend of_tokens $t
            }
            incr i 2
        } elseif {$token eq "-filter"} {
            if {$i + 1 >= $n} {
                error "$cmd_name: -filter requires a value"
            }
            set filter [lindex $args_list [expr {$i + 1}]]
            incr i 2
        } elseif {[string index $token 0] eq "-"} {
            error "$cmd_name: unknown flag $token"
        } else {
            if {!$has_name_expr} {
                error "$cmd_name: this object type has no name - only -of/-filter/-help are valid"
            }
            lappend name_exprs $token
            incr i
        }
    }

    return [dict create name_exprs $name_exprs of_tokens $of_tokens filter $filter help $help]
}

# Every -of token must be validated against `cmd_name`'s own valid
# parent-type prefix set *before* any shim call (UPDATES.md item 19.1's
# error-checking requirement 1) - a wrong-type token is a script bug, not
# an empty-result-shaped "not found".
proc check_of_prefixes {cmd_name of_tokens prefixes} {
    foreach token $of_tokens {
        set matched 0
        foreach prefix $prefixes {
            if {[string match "${prefix}:*" $token]} {
                set matched 1
                break
            }
        }
        if {!$matched} {
            error "$cmd_name: -of only accepts [join $prefixes {: or }]: tokens (got \"$token\") - only [join $prefixes { or }] objects are valid parents for $cmd_name"
        }
    }
}

# {} (a single empty-string element) is the "axis not given" default for
# both name-expressions and -of tokens - each shim *_cmd already treats
# an empty name_expression/of_token as "skip this axis"/"use the default
# scope" (see le_tcl_shim.hpp's own "IDs" comment), so looping a
# one-element list holding that empty string through the same call path
# as a real value needs no special-casing here.
proc default_to_unset {values} {
    if {[llength $values] == 0} {
        return {{}}
    }
    return $values
}

# --- get_properties/report_properties (UPDATES.md item 19.2) ---
#
# property_accessors_for_token (dispatches a friendly-id token to its
# {count name value path} shim-function quadruplet by prefix, across
# every TCL-readable class - not just library:/design:/abstract:/
# terminal:/terminal_port:/obstruction:/shape:) and the ::property_scalars/
# ::property_hops dot-path completion tables (UPDATES.md item 20) are
# generated - see generated/le_tcl_procs_generated.tcl and backend/
# CLAUDE.md's TCL section. Never edit that file directly, regenerate via
# the regen-tcl skill instead.
# Tries the real source-tree layout first (generated/le_tcl_procs_generated.tcl,
# alongside this file, unchanged - ctest/le_shell/tclsh all source this file
# straight from backend/src/tcl/, where that subdirectory genuinely exists),
# then falls back to a flat layout (this file's own directory, no generated/
# subdirectory) - a packaged release bundle can't preserve that nesting (the
# whole flutter_plugin bundling mechanism installs individual files into one
# flat lib/ directory, no per-file destination subdirectory) - confirmed
# necessary by a real "couldn't read file .../generated/le_tcl_procs_generated.tcl:
# no such file or directory" error running a release build.
set _le_generated_procs_candidates [list \
    [file join [file dirname [info script]] generated le_tcl_procs_generated.tcl] \
    [file join [file dirname [info script]] le_tcl_procs_generated.tcl] \
]
foreach _le_candidate $_le_generated_procs_candidates {
    if {[file exists $_le_candidate]} {
        source $_le_candidate
        break
    }
}
unset _le_generated_procs_candidates _le_candidate

# All properties for one token, as a dict - the shared building block
# behind both get_properties and report_properties.
proc properties_for_token {token} {
    lassign [property_accessors_for_token $token] count_cmd name_cmd value_cmd
    set result {}
    set n [$count_cmd $token]
    for {set i 0} {$i < $n} {incr i} {
        dict set result [$name_cmd $token $i] [$value_cmd $token $i]
    }
    return $result
}

# `tokens`/`property_names` each independently collapse from "a list" to
# "one value" when they hold exactly one element - Tcl can't otherwise
# distinguish a single bare token/name from a one-element list of them
# (`terminal:IN0` literal and a one-match [get_terminals] result are
# structurally identical), so this is the only rule that can match every
# one of UPDATES.md item 19.2's own worked examples:
#   get_properties [get_terminals]              -> list of dicts (many tokens)
#   get_properties terminal:IN0 .name           -> scalar (one token, one name)
#   get_properties terminal:IN0 {.name .direction} -> flat list (one token, many names)
#   get_properties [get_terminals] {.name .direction} -> list of flat lists
#
# Each requested property name is a dotted path (`.name`, or chained
# through a hop like `.terminal.name` - backend/src/database/filter.hpp's
# parse_property_path/resolve_property_path grammar, the same one -filter
# expressions already use for their own field paths) resolved via the
# token's own *_property_path shim function - always through this path
# mechanism, even for a plain single-segment name, rather than a separate
# dict-lookup fast path, so chained and unchained lookups behave
# identically. A path that fails to parse or references an unrecognized
# field/hop pushes a message (see le_message_*) that this detects via a
# message_count before/after diff and re-raises as a Tcl error, naming
# the specific problem - a structurally valid path that simply has no
# data for this object (e.g. a list hop with zero elements) pushes no
# message and just resolves to "".
proc get_properties {tokens {property_names {}}} {
    set single_token [expr {[llength $tokens] == 1}]
    set token_list [expr {$single_token ? [list $tokens] : $tokens}]

    set results {}
    foreach token $token_list {
        if {[llength $property_names] == 0} {
            lappend results [properties_for_token $token]
        } else {
            lassign [property_accessors_for_token $token] count_cmd name_cmd value_cmd path_cmd
            set values {}
            foreach path $property_names {
                set messages_before [message_count]
                set value [$path_cmd $token $path]
                if {[message_count] > $messages_before} {
                    error "get_properties: [message_at [expr {[message_count] - 1}]]"
                }
                lappend values $value
            }
            if {[llength $property_names] == 1} {
                lappend results [lindex $values 0]
            } else {
                lappend results $values
            }
        }
    }

    if {$single_token} {
        return [lindex $results 0]
    }
    return $results
}
register_command_help get_properties \
    "get_properties <tokens> ?property_names? - Reads one or more dotted property paths from one or more friendly-id tokens" \
    "Reads every property, or a specific set of dotted property paths (.name, or a chained hop like .terminal.name), from one or more friendly-id tokens. No -help flag - see man get_properties for the full return-shape contract (single token/name collapse to a scalar, otherwise a list)." \
    {
        {<tokens> {type token... required 1 description {One friendly-id token, or a list of them (e.g. the result of [get_terminals])}}}
        {<property_names> {type str... required 0 description {One dotted property path, or a list of them - omitted returns every property}}}
    }

# Pretty-prints every property of every token to stdout, one block per
# token, names padded (within that token's own block) to align values.
proc report_properties {tokens} {
    foreach token $tokens {
        puts $token
        set props [properties_for_token $token]
        set max_len 0
        foreach name [dict keys $props] {
            if {[string length $name] > $max_len} {
                set max_len [string length $name]
            }
        }
        dict for {name value} $props {
            puts [format "  %-*s %s" [expr {$max_len + 1}] "${name}:" $value]
        }
        puts ""
    }
}
register_command_help report_properties \
    "report_properties <tokens> - Pretty-prints every property of every token to stdout" \
    "Pretty-prints every property of every given friendly-id token to stdout, one aligned block per token. No -help flag or return value - see get_properties for a script-friendly (non-printing) equivalent." \
    {
        {<tokens> {type token... required 1 description {One friendly-id token, or a list of them}}}
    }

# --- Terminal (create_terminal/update_terminal are generated -
# le_tcl_procs_generated.tcl) ---

# --- TerminalPort (create_terminal_port is generated) ---

# --- Obstruction (create_obstruction is generated) ---

# --- Shape (create_shape/update_shape are generated - unify the former
# create_terminal_port_shape/create_obstruction_shape split into one
# command taking -terminal_port|-obstruction, exactly one required, and
# take their own -rects/-polygons/-paths flags directly - geometry no
# longer needs a separate add_shape_rect/_polygon/_path call after
# create_shape; remove_shape_rect/_polygon/_path below still cover
# removing one entry by index, the one thing update_shape's own
# "replace-the-whole-list" flags don't do more conveniently) ---

proc shape_rects {id} {
    set result {}
    set n [shape_rect_count $id]
    for {set i 0} {$i < $n} {incr i} {
        lappend result [shape_rect_at $id $i]
    }
    return $result
}
register_command_help shape_rects \
    "shape_rects <id> - Every rect on Shape <id>, as a list of {ll_x ll_y ur_x ur_y} (microns)" \
    "Every rect on the given Shape, as a list of {ll_x ll_y ur_x ur_y} coordinate lists in microns." \
    {
        {<id> {type token required 1 description {A shape: friendly-id token}}}
    }

proc shape_polygons {id} {
    set result {}
    set polygon_count [shape_polygon_count $id]
    for {set p 0} {$p < $polygon_count} {incr p} {
        set points {}
        set point_count [shape_polygon_point_count $id $p]
        for {set c 0} {$c < $point_count} {incr c} {
            lappend points [shape_polygon_point_at $id $p $c]
        }
        lappend result $points
    }
    return $result
}
register_command_help shape_polygons \
    "shape_polygons <id> - Every polygon on Shape <id>, as a list of point lists (microns)" \
    "Every polygon on the given Shape, as a list of point lists (each a flat {x y x y ...} list, microns)." \
    {
        {<id> {type token required 1 description {A shape: friendly-id token}}}
    }

# --- GUI (Phase 6 - see TCL_EXPLORATION.md) ---

# Deliberate stub, not a silently missing feature: this project's only
# GUI is the Flutter app, a separate Dart/Flutter runtime consuming
# api/render/io via FFI - this Tcl shell can't "just start rendering" on
# the same in-process state without embedding a full FlutterEngine
# alongside its own Tcl event loop (OpenROAD's actual gui_start model),
# a substantial, separate integration effort TCL_EXPLORATION.md's Phase
# 6 section explicitly defers rather than fakes. Prints instead of
# silently no-op-ing so a caller typing `show_gui` learns why nothing
# happened, not just that nothing did.
proc show_gui {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "show_gui \[-help\] - Deliberate stub; this Tcl shell doesn't embed the Flutter GUI in-process"
    }
    puts "show_gui: not implemented - this Tcl shell doesn't embed the Flutter GUI in-process yet. See TCL_EXPLORATION.md's Phase 6 section for why and what real support would take."
}
register_command_help show_gui \
    "show_gui \[-help\] - Deliberate stub; this Tcl shell doesn't embed the Flutter GUI in-process" \
    "Deliberate stub, not a missing feature: this project's only GUI is the separate Flutter app, which this Tcl process can't start rendering in-process without embedding a full FlutterEngine - see TCL_EXPLORATION.md's Phase 6 section." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc shape_paths {id} {
    set result {}
    set path_count [shape_path_count $id]
    for {set p 0} {$p < $path_count} {incr p} {
        set points {}
        set point_count [shape_path_point_count $id $p]
        for {set c 0} {$c < $point_count} {incr c} {
            lappend points [shape_path_point_at $id $p $c]
        }
        lappend result [dict create width_um [shape_path_width_um $id $p] points $points]
    }
    return $result
}
register_command_help shape_paths \
    "shape_paths <id> - Every path on Shape <id>, as a list of {width_um <um> points <list>} dicts" \
    "Every path on the given Shape, as a list of {width_um <double> points <flat x/y list, microns>} dicts." \
    {
        {<id> {type token required 1 description {A shape: friendly-id token}}}
    }
