TEMPLATE = """# GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
# (codegen --target tcl). Sourced once from le_tcl_procs.tcl.
#
# property_accessors_for_token (dispatches a friendly-id token to its
# {count name value path} shim-function quadruplet by prefix, across
# every TCL-readable class) and get_<type>/set_current_<type> - all
# generated - see backend/CLAUDE.md's TCL codegen section. Never edit
# this file directly, regenerate via the regen-tcl skill instead.
# parse_get_args/check_of_prefixes/default_to_unset (le_tcl_procs.tcl)
# are shared, class-agnostic helpers this file calls into, not generated
# here. register_command_help/::command_help (UPDATES.md item 20's help
# system) are likewise defined in le_tcl_procs.tcl, sourced before this
# file - every proc below registers itself right after its own
# definition, so help/man/complete_command see the full generated
# surface with no extra wiring.

# Maps a friendly-id token to its {count name value path} shim-function
# quadruplet - see le_tcl_procs.tcl's own property_accessors_for_token
# comment (properties_for_token/get_properties/report_properties) for
# the full contract this backs.
proc property_accessors_for_token {token} {
{%- for klass in readable_classes %}
    if {[string match "{{klass.to_snake_case()}}:*" $token]} {
        return {{'{'}}{{klass.to_snake_case()}}_property_count {{klass.to_snake_case()}}_property_name {{klass.to_snake_case()}}_property_value {{klass.to_snake_case()}}_property_path{{'}'}}
    }
{%- endfor %}
    error "get_properties: unrecognized token \\"$token\\" - expected a friendly id ({% for klass in readable_classes %}{{klass.to_snake_case()}}:{% if not loop.last %}/{% endif %}{% endfor %})"
}

# Dot-path completion tables (UPDATES.md item 20 point 3,
# complete_command in le_tcl_procs.tcl) - one entry per TCL-readable
# class, keyed the same way property_accessors_for_token dispatches
# (klass.to_snake_case()). ::property_scalars holds this class's own
# leaf property names; ::property_hops holds {hop_name target_key} pairs
# so a chained path (.terminal_port.terminal.name) can be walked one hop
# at a time, looking up the next class's own ::property_scalars/
# ::property_hops entry after each hop. Both are mechanically derived
# from Klass.get_filterable_scalar_fields()/get_filterable_hop_fields() -
# the exact same source api_filter_tables_inc_j2.py's C++ table already
# uses for -filter/get_properties path validation, so a suggested
# completion can never name a path those commands would actually reject.
array set ::property_scalars {
{%- for klass in readable_classes %}
    {{klass.to_snake_case()}} {{'{'}}{{klass.tcl_property_scalars()}}{{'}'}}
{%- endfor %}
}
array set ::property_hops {
{%- for klass in readable_classes %}
    {{klass.to_snake_case()}} {{'{'}}{{klass.tcl_property_hops()}}{{'}'}}
{%- endfor %}
}

# Maps a get_<type> command's own name to its class's ::property_scalars/
# ::property_hops key - complete_command's (le_tcl_procs.tcl) own seed
# for a dot-path typed inside that command's -filter value, e.g.
# `get_terminals -filter {.` completes against Terminal's own properties
# with no friendly-id token needed anywhere on the line (unlike
# get_properties/report_properties, whose dot-paths take a token
# argument directly and seed from *that* instead).
array set ::get_command_class {
{%- for klass in classes %}
    get_{{klass.tcl_plural_snake_case()}} {{klass.to_snake_case()}}
{%- endfor %}
}

# --- Current-instance access - `current_<type> [<id>] [-help]`: with no
# arguments, returns the current <type> (empty string if none is set
# yet); with a friendly-id token given, selects it first (folding in
# the old, now-removed set_current_<type>'s own job), then returns it -
# every other readable class's get_<type> default (-of omitted) scope
# derives from whichever instance this selects, see codegen/codegen/
# tcl_scope.py's own module docstring. Reads and writes go through this
# one command so a caller never has to remember which of two separate
# commands does which. ---
{% for klass in current_access_classes %}
{%- set snake = klass.to_snake_case() %}
proc current_{{snake}} {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "current_{{snake}} \\[<id>\\] \\[-help\\] - Returns the current {{klass.name}}, selecting it first if <id> is given"
    }
    if {[llength $args] > 1} {
        error "current_{{snake}}: at most one argument (a {{klass.name}} id) is allowed"
    }
    if {[llength $args] == 1} {
        if {[set_current_{{snake}}_cmd [lindex $args 0]] != 0} {
            error "current_{{snake}}: failed to select \\"[lindex $args 0]\\""
        }
    }
    return [current_{{snake}}_cmd]
}
set current_{{snake}}_options {}
lappend current_{{snake}}_options {<id> {type token required 0 description {A friendly-id token to select as current - omit to just read the current value}}}
lappend current_{{snake}}_options {-help {type flag required 0 description {Show this usage message and return immediately}}}
register_command_help current_{{snake}} "current_{{snake}} \\[<id>\\] \\[-help\\] - Returns the current {{klass.name}}, selecting it first if <id> is given" "Returns the current {{klass.name}} (empty string if none is set yet); with <id> given, selects it first (this is also every other readable class's own get_<type> default -of-omitted scope anchor), then returns it." $current_{{snake}}_options
{% endfor %}

# --- get_<type> search ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
{%- set plural = klass.tcl_plural_snake_case() %}
{%- set usage_line -%}
get_{{plural}} {% if id_field %}\\[<name-expr>...\\] {% endif %}{% if scope.of_params %}\\[-of <token>...\\] {% endif %}\\[-filter <expr>\\] \\[-help\\] - {{klass.tcl_description_escaped()}}
{%- endset %}
proc get_{{plural}} {args} {
    set parsed [parse_get_args get_{{plural}} $args {{ 1 if id_field else 0 }}]
    if {[dict get $parsed help]} {
        return "{{usage_line}}"
    }
    {%- if scope.of_params %}
    check_of_prefixes get_{{plural}} [dict get $parsed of_tokens] {{'{'}}{% for op in scope.of_params %}{{op.parent_klass.to_snake_case()}}{% if not loop.last %} {% endif %}{% endfor %}{{'}'}}
    {%- else %}
    if {[llength [dict get $parsed of_tokens]] > 0} {
        error "get_{{plural}}: -of is not valid here - {{klass.name}} has no parent object type"
    }
    {%- endif %}
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        {%- for op in scope.of_params %}
        set of_{{op.parent_field.name}} {}
        {%- endfor %}
        {%- for op in scope.of_params %}
        if {[string match "{{op.parent_klass.to_snake_case()}}:*" $of_token]} {
            set of_{{op.parent_field.name}} $of_token
        }
        {%- endfor %}
        {%- if id_field %}
        foreach name_expr [default_to_unset [dict get $parsed name_exprs]] {
            set count [get_{{plural}}_cmd {% for op in scope.of_params %}$of_{{op.parent_field.name}} {% endfor %}$name_expr $filter]
            for {set i 0} {$i < $count} {incr i} {
                lappend result [get_{{plural}}_at $i]
            }
        }
        {%- else %}
        set count [get_{{plural}}_cmd {% for op in scope.of_params %}$of_{{op.parent_field.name}} {% endfor %}$filter]
        for {set i 0} {$i < $count} {incr i} {
            lappend result [get_{{plural}}_at $i]
        }
        {%- endif %}
    }
    return [lsort -unique $result]
}
set get_{{plural}}_options {}
{%- if id_field %}
lappend get_{{plural}}_options {<name-expr> {type str required 0 description {Glob-matched against {{id_field.name}} (Tcl string match syntax) - may be given more than once, OR'd}}}
{%- endif %}
{%- if scope.of_params %}
lappend get_{{plural}}_options {-of {type token... required 0 description {Parent token(s) to scope the search to (OR'd across each -of value's own list) - defaults to the current view when omitted, see codegen/codegen/tcl_scope.py}}}
{%- endif %}
lappend get_{{plural}}_options {-filter {type expr required 0 description {A -filter expression (backend/src/database/filter.hpp) - field/hop names validated against this class's own allowlist}}}
lappend get_{{plural}}_options {-help {type flag required 0 description {Show this usage message and return immediately}}}
register_command_help get_{{plural}} "{{usage_line}}" "{{klass.tcl_description_escaped()}}" $get_{{plural}}_options
{% endfor %}

# --- create_<type> - `create_<type> [-flag value...]`, one flag per
# scalar field plus one per parent (a friendly-id token, e.g. `-abstract
# abstract:0` - see api_declarations_inc_j2's own comment for the field-
# scope/optionality conventions this follows uniformly). A parent flag
# whose own Klass has has_current_access=True (e.g. -abstract, since
# Abstract does) defaults to current_<parent_klass> when omitted (see
# Klass.create_tcl_current_defaults() - only when every other parent
# flag was also left empty, for a multi-parent class), the same way
# get_<type>'s own default (-of omitted) scope already derives from
# current_abstract - so create_terminal needs no explicit -abstract once
# current_abstract <id>/open_design has been called. A multi-parent class
# (e.g. Shape's -terminal_port/-obstruction) requires exactly one of its
# parent flags to resolve (after that default is applied), checked here
# before calling down - same "exactly one" rule create_<type>_cmd's own
# C++ body re-checks (defense in depth, not redundant: a caller could
# reach the *_cmd form directly). `-help` (UPDATES.md item 20) is checked
# before the unknown-flag loop below, since it never receives a paired
# value the way every other flag does. ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
{%- set parent_fields = klass.get_parent_fields() %}
proc create_{{snake}} {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "{{klass.create_tcl_usage()}}"
    }
    array set opts {{'{'}}{{klass.create_tcl_flag_defaults()}}{{'}'}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_{{snake}}: unknown flag $flag"
        }
        set opts($flag) $value
    }
{{klass.create_tcl_current_defaults()}}
    {%- if parent_fields|length > 1 %}
    set provided_parents 0
    {%- for pf in parent_fields %}
    if {$opts(-{{pf.name}}) ne {}} { incr provided_parents }
    {%- endfor %}
    if {$provided_parents != 1} {
        error "create_{{snake}}: exactly one of {% for pf in parent_fields %}-{{pf.name}}{% if not loop.last %}/{% endif %}{% endfor %} is required"
    }
    {%- endif %}
    foreach required {{'{'}}{{klass.create_tcl_required_flags()}}{{'}'}} {
        if {$opts($required) eq {}} {
            error "create_{{snake}}: $required is required"
        }
    }
{{klass.cmd_tcl_preamble('create')}}
    return [create_{{snake}}_cmd {{klass.create_tcl_call_args()}}]
}
register_command_help create_{{snake}} "{{klass.create_tcl_usage()}}" "{{klass.tcl_description_escaped()}}" {{'{'}}{{klass.create_tcl_help_options()}} {-help {type flag required 0 description {Show this usage message and return immediately}}}{{'}'}}
{% endfor %}

# --- update_<type> - `update_<type> <id> -flag value...` - mutates an
# *existing* object in place; the *only* way any field is ever updated
# (no per-field setter, generated or hand-written, exists anymore - see
# this round's own plan/commit message). Same flag set as create_<type>
# (same compound/enum/dbu conventions - see api_declarations_inc_j2's
# own comment), but every flag here means "leave unchanged" when
# omitted, the opposite of create_<type>'s "omitted means unset" - so,
# unlike create_<type>, nothing is ever required *except* that at least
# one flag must be given (an update with zero flags is a no-op call,
# rejected here rather than silently doing nothing). A single-parent
# class also accepts its own parent flag (e.g. -abstract) to reassign
# it; a multi-parent class (e.g. Shape) accepts none at all - passing
# -terminal_port/-obstruction to update_shape is an "unknown flag"
# error, same as any other flag this class doesn't have. Returns the
# (possibly-changed, e.g. after a rename) friendly id token. `-help`
# (UPDATES.md item 20) is checked before the "at least one flag"
# requirement below, so `update_<type> <id> -help` works even with no
# other flags given - and also checked against `id` itself, not just
# `args`: `update_<type>` takes a *mandatory* leading positional (`id`),
# so calling it as bare `update_<type> -help` (no real id supplied at
# all) binds "-help" to `id`, leaving `args` empty - a real bug found
# this way (only checking `args` silently fell through to the
# "at least one -flag is required" error instead). ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
proc update_{{snake}} {id args} {
    if {$id eq "-help" || [lsearch -exact $args "-help"] >= 0} {
        return "{{klass.update_tcl_usage()}}"
    }
    if {[llength $args] == 0} {
        error "update_{{snake}}: at least one -flag is required"
    }
    array set opts {{'{'}}{{klass.update_tcl_flag_defaults()}}{{'}'}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "update_{{snake}}: unknown flag $flag"
        }
        set opts($flag) $value
    }
{{klass.cmd_tcl_preamble('update')}}
    set new_id [update_{{snake}}_cmd $id {{klass.update_tcl_call_args()}}]
    if {$new_id eq {}} {
        error "update_{{snake}}: failed to update \\"$id\\""
    }
    return $new_id
}
register_command_help update_{{snake}} "{{klass.update_tcl_usage()}}" "{{klass.tcl_description_escaped()}}" {{'{'}}{{klass.update_tcl_help_options()}} {-help {type flag required 0 description {Show this usage message and return immediately}}}{{'}'}}
{% endfor %}

# --- delete_<type> - `delete_<type> <id> [-help]` - deletes an existing
# object, cascading to every owned pool-backed child reachable through
# Klass.tcl_child_list_fields() (however many schema-graph levels deep
# that goes for this class - see Klass.delete_api_body()'s own docstring
# for the full mechanism). Unlike create_<type>/update_<type>, there are
# no flags at all here - delete_<type> takes exactly one positional `<id>`
# argument, nothing else to specify - so, unlike those two families'
# `array set opts {...}`/`foreach {flag value} $args` flag parsing, this
# is just an argument-count check. `-help` is checked the same way
# create_<type>'s own is (before the argument-count check, so
# `delete_<type> -help` works with no real id supplied at all). Returns 0
# on success (raises a Tcl error otherwise) - matches the plain SWIG-bound
# `int` return value the 4 formerly hand-written delete_X commands this
# replaces (le_tcl_shim.cpp/le_api.i) already returned directly with no
# Tcl-level wrapper proc at all, so existing callers checking `delete_X
# $id == 0` keep working unchanged even though every delete_<type> now
# goes through a real Tcl proc (needed for -help/register_command_help,
# neither of which a bare SWIG binding can provide). ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
proc delete_{{snake}} {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "{{klass.delete_tcl_usage()}}"
    }
    if {[llength $args] != 1} {
        error "delete_{{snake}}: exactly one argument (a {{klass.name}} id) is required"
    }
    if {[delete_{{snake}}_cmd [lindex $args 0]] != 0} {
        error "delete_{{snake}}: failed to delete \\"[lindex $args 0]\\""
    }
    return 0
}
register_command_help delete_{{snake}} "{{klass.delete_tcl_usage()}}" "{{klass.tcl_description_escaped()}}" {{'{'}}{{klass.delete_tcl_help_options()}} {-help {type flag required 0 description {Show this usage message and return immediately}}}{{'}'}}
{% endfor %}
"""
