"""The generated Tcl procs' text (le_tcl_procs_generated.tcl): each create_/update_/
delete_<type> proc's usage, help options, flag defaults, current-object
defaults, reference token checks and the argument parsing before its *_cmd
call."""

from codegen.schema import SMALL_UINT_MAX, _leaf_param_name, tcl_brace_escape, tcl_dquote_escape, tcl_help_description
from typing import List


def create_tcl_flag_defaults(klass) -> str:
    """
    The `array set opts {...}` initializer body (space-joined
    `-flag {}` pairs) for this class's generated `create_<type>
    {args}` proc - one entry per parent field, then per create field,
    in create_shim_params() order.
    """
    parts = [f"-{pf.name} {{}}" for pf in klass.get_parent_fields()]
    parts += [f"-{rf.name} {{}}" for rf in klass.get_reference_create_fields()]
    parts += [f"-{f.name} {{}}" for f in klass.get_create_fields()]
    return " ".join(parts)


def create_tcl_required_flags(klass) -> str:
    """
    Space-joined `-flag` names the generated `create_<type> {args}`
    proc rejects as missing before calling create_<type>_cmd - each
    parent flag when there's only one (get_parent_fields() length > 1
    means "exactly one of these", a distinct check - see
    c_api.create_api_body()'s own parent-count validation, mirrored here),
    plus every create_required() create field.
    """
    parts = []
    if len(klass.get_parent_fields()) == 1:
        parts.append(f"-{klass.get_parent_fields()[0].name}")
    parts += [f"-{rf.name}" for rf in klass.get_reference_create_fields() if rf.create_required()]
    parts += [f"-{f.name}" for f in klass.get_create_fields() if f.create_required()]
    return " ".join(parts)


def create_tcl_current_defaults(klass) -> str:
    """
    Generated Tcl statements for the create_<type> {args} proc,
    emitted after flag parsing and before the required-flags/
    exactly-one-parent checks - default an omitted parent flag to
    current_<parent_klass> (an already-generated, no-arg Tcl command,
    e.g. `current_abstract`) whenever that parent Klass has
    has_current_access=True, so e.g. create_terminal's own -abstract
    flag needs no explicit value once set_current_abstract has been
    called, the same way get_terminals' own default (-of omitted)
    scope already derives from current_abstract (see tcl_scope.py's
    own module docstring). Still just a *default* - an explicitly
    passed flag always wins (only substituted when the flag is
    empty), and create_<type>'s own required-flag check still fires
    afterward if current_<parent_klass> is itself unset, so a
    genuinely missing parent is still a clear error, not a silent
    invalid-parent create.

    For a multi-parent class (Shape/ViaLayer/Foreign/
    LayerDensityEntry), the default only applies when every *other*
    parent flag was also left empty - an explicitly-provided sibling
    parent flag (e.g. -via) must never be silently overridden by a
    current-access default for a *different* parent (e.g. -abstract
    on Foreign), which would otherwise turn a call that only ever
    meant to provide one parent into a spurious "exactly one parent
    must resolve, got 2" failure.

    When several parent fields point at the same parent Klass, a
    field whose sibling is a scalar child (e.g. Shape.layout ->
    Layout.diearea) never gets the default if one whose sibling is a
    list does (Shape.in_layout -> Layout.free_shapes): defaulting to
    the scalar would silently replace the existing child (the die
    area) instead of adding a new one.
    """
    parent_fields = klass.get_parent_fields()
    list_owner_klasses = {pf._parent_klass.name for pf in parent_fields if pf._parent_field is not None and pf._parent_field.is_list}
    lines = []
    for pf in parent_fields:
        if not pf._parent_klass.has_current_access:
            continue
        if pf._parent_klass.name in list_owner_klasses and not (pf._parent_field is not None and pf._parent_field.is_list):
            continue
        others = [p for p in parent_fields if p is not pf]
        condition = " && ".join([f"$opts(-{pf.name}) eq {{}}"] + [f"$opts(-{o.name}) eq {{}}" for o in others])
        lines.append(f"    if {{{condition}}} {{")
        lines.append(f"        set opts(-{pf.name}) [current_{pf._parent_klass.to_snake_case()}]")
        lines.append("    }")
    return "\n".join(lines)


def create_tcl_aliases(klass) -> str:
    """
    Generated Tcl statements for the create_<type> {args} proc,
    emitted right after flag parsing - rewrite each create field's own
    Field.tcl_create_aliases value (case-insensitive) into the flags
    it stands for, clearing the aliased flag itself.
    """
    lines = []
    for f in klass.fields:
        for alias, replacement in (f.tcl_create_aliases or {}).items():
            lines.append(f"    if {{[string tolower $opts(-{f.name})] eq \"{alias.lower()}\"}} {{")
            lines.append(f"        set opts(-{f.name}) {{}}")
            for other, value in replacement.items():
                lines.append(f"        set opts(-{other}) {value}")
            lines.append("    }")
    return "\n".join(lines)


def tcl_reference_token_checks(klass, command: str) -> str:
    """
    Generated Tcl statements for create_<type>/update_<type>: a
    reference flag (get_reference_create_fields(), e.g. Shape.layer)
    given a token that doesn't resolve is an error - le_create_<type>
    only ever sees the resolved id, so without this an unresolved
    token (e.g. a bare `Metal1` instead of `layer:Metal1`) silently
    created the object with the reference unset.
    """
    lines = []
    for rf in klass.get_reference_create_fields():
        snake = rf._type_klass.to_snake_case()
        example = "<name>" if rf._type_klass.tcl_indexed_id_field() else "<N>"
        lines.append(f"    if {{$opts(-{rf.name}) ne {{}} && ![{snake}_token_resolves $opts(-{rf.name})]}} {{")
        lines.append(f"        error \"{command}: unknown {snake} \\\"$opts(-{rf.name})\\\" - expected a {snake}:{example} token\"")
        lines.append("    }")
    return "\n".join(lines)


def create_tcl_call_args(klass) -> str:
    """
    Space-joined Tcl argument list for calling create_<type>_cmd from
    this class's own create_<type> {args} proc, in create_shim_params()
    order - `$opts(-<parent_field>)` per parent token, then each
    create field's own cmd_tcl_call_args("create").
    """
    parts = [f"$opts(-{pf.name})" for pf in klass.non_owner_parent_fields()]
    if klass.get_owner_fields():
        parts += ["$owner_kind", "$owner_id"]  # set from whichever owner flag was given
    parts += [f"$opts(-{rf.name})" for rf in klass.get_reference_create_fields()]
    for f in klass.get_create_fields():
        parts.extend(cmd_tcl_call_args(f, "create"))
    return " ".join(parts)


def cmd_tcl_preamble(klass, mode: str) -> str:
    """
    Generated Tcl statements, emitted after flag parsing and before
    the _cmd call, that turn every compound or list_compound_kind()-
    eligible create field's own Tcl list flag value into its has-flag
    (+ leaf-parameter locals, or a single flattened list - see
    field_cmd_tcl_preamble()) - shared verbatim between create_<type>
    and update_<type> generation. Empty string if this class has no
    such create fields.
    """
    cmd_name = f"{mode}_{klass.to_snake_case()}"
    lines = [
        field_cmd_tcl_preamble(f, mode, cmd_name)
        for f in klass.get_create_fields()
        if f.compound_klass() is not None or f.list_compound_kind() is not None
    ]
    return "\n".join(line for line in lines if line)


def update_tcl_flag_defaults(klass) -> str:
    """
    The `array set opts {...}` initializer body for this class's
    generated `update_<type> {args}` proc - one entry per parent
    field (only for a single-parent class), then per create field,
    in update_shim_params() order.
    """
    parent_fields = klass.get_parent_fields()
    parts = [f"-{parent_fields[0].name} {{}}"] if len(parent_fields) == 1 else []
    parts += [f"-{rf.name} {{}}" for rf in klass.get_reference_create_fields()]
    parts += [f"-{f.name} {{}}" for f in klass.get_create_fields()]
    return " ".join(parts)


def update_tcl_call_args(klass) -> str:
    """
    Space-joined Tcl argument list for calling update_<type>_cmd
    from this class's own update_<type> {args} proc, in
    update_shim_params() order - no required-flag list (nothing is
    ever required to update - update_<type> has no counterpart of
    create_tcl_required_flags()). The optional parent flag (single-
    parent classes only) passes a has-flag/value pair the same way
    an optional numeric create field does; every create field
    follows via its own cmd_tcl_call_args("update").
    """
    parts = []
    parent_fields = klass.get_parent_fields()
    if len(parent_fields) == 1:
        opt = f"$opts(-{parent_fields[0].name})"
        parts.append(f"[expr {{{opt} ne {{}} ? 1 : 0}}]")
        parts.append(opt)
    for rf in klass.get_reference_create_fields():
        opt = f"$opts(-{rf.name})"
        parts.append(f"[expr {{{opt} ne {{}} ? 1 : 0}}]")
        parts.append(opt)
    for f in klass.get_create_fields():
        parts.extend(cmd_tcl_call_args(f, "update"))
    return " ".join(parts)


def create_tcl_usage(klass) -> str:
    """
    The one-line `create_<type> -flag <type> [-flag2 <type>] ... -
    <description>` usage string registered for this class's generated
    create_<type> command (and returned by its own new `-help` flag) -
    required flags shown bare, optional ones bracketed, in
    create_tcl_flag_defaults()'s own field order. A multi-parent class
    (Shape/ViaLayer/Foreign/LayerDensityEntry) shows every parent flag
    bracketed - "exactly one of these is required" doesn't fit cleanly
    into per-flag bracket notation, so the fuller rule stays in this
    class's own description/options text instead.

    The result is embedded inside a generated Tcl `"..."`-quoted
    string (both `-help`'s own return and the register_command_help
    call use the identical literal), so every `[`/`]` this emits is
    backslash-escaped up front (`\\[`/`\\]`) - unescaped, Tcl would
    read an optional-flag bracket as command substitution, exactly
    why get_<type>'s own hand-written usage line
    (le_tcl_procs_generated_tcl_j2.py) already does the same for its
    own `[-of ...]`/`[-filter ...]`/`[-help]` fragments.
    """
    parent_fields = klass.get_parent_fields()
    single_parent = len(parent_fields) == 1
    parts = [f"create_{klass.to_snake_case()}"]
    for pf in parent_fields:
        frag = f"-{pf.name} <token>"
        parts.append(frag if single_parent else f"\\[{frag}\\]")
    for rf in klass.get_reference_create_fields():
        frag = f"-{rf.name} <token>"
        parts.append(frag if rf.create_required() else f"\\[{frag}\\]")
    for f in klass.get_create_fields():
        frag = f"-{f.name} <{tcl_help_type_label(f)}>"
        parts.append(frag if f.create_required() else f"\\[{frag}\\]")
    parts.append("\\[-help\\]")
    return " ".join(parts) + " - " + klass.tcl_description_escaped()


def tcl_help_attributes(klass) -> str:
    """
    The optional attributes argument of this class's register_command_help
    calls (with a leading space), or "": its section of the command
    reference, and, for an extension's class, its extension, so the core
    reference can leave it out.
    """
    attributes = []
    if klass.extension is not None:
        attributes += ["extension", klass.extension]
    if klass.tcl_category is not None:
        attributes += ["category", klass.tcl_category]
    return " {" + " ".join(attributes) + "}" if attributes else ""


def create_tcl_help_options(klass) -> str:
    """
    Tcl list literal (inner content - wrap in {...} at the call site)
    of this class's generated create_<type> command's own options, one
    `{-flag {type T required R description {D}}}` entry per parent
    field then create field, in create_tcl_flag_defaults()'s own field
    order - the registration payload help/man/complete_command
    (le_tcl_procs.tcl) read.
    """
    parent_fields = klass.get_parent_fields()
    single_parent = len(parent_fields) == 1
    parts = []
    for pf in parent_fields:
        required = 1 if single_parent else 0
        if single_parent:
            desc = f"Parent {pf.type} token"
        else:
            desc = f"Parent {pf.type} token - give exactly one of the parent flags"
        # An owner option an extension's class implies is that extension's.
        extension = f" extension {pf.synthesized_by}" if pf.synthesized_by else ""
        parts.append(
            f"{{-{pf.name} {{type token required {required} description {{{tcl_brace_escape(desc)}}}{extension}}}}}"
        )
    for rf in klass.get_reference_create_fields():
        required = 1 if rf.create_required() else 0
        parts.append(
            f"{{-{rf.name} {{type token required {required} description {{{tcl_brace_escape(rf.description)}}}}}}}"
        )
    for f in klass.get_create_fields():
        required = 1 if f.create_required() else 0
        parts.append(
            f"{{-{f.name} {{type {tcl_help_type_label(f)} required {required} "
            f"description {{{tcl_brace_escape(tcl_help_description(f))}}}}}}}"
        )
    return " ".join(parts)


def update_tcl_usage(klass) -> str:
    """
    Same shape as create_tcl_usage(), but every flag is bracketed
    (nothing is ever required to update - see update_tcl_flag_defaults())
    and a leading `<id>` positional precedes the flags, matching
    update_<type>'s own `{id args}` proc signature. A single-parent
    class's own parent flag is included as an optional reassignment
    flag; a multi-parent class gets none (matching
    update_tcl_flag_defaults()). See create_tcl_usage()'s own comment
    for why every `[`/`]` here is backslash-escaped.
    """
    parent_fields = klass.get_parent_fields()
    parts = [f"update_{klass.to_snake_case()}", "<id>"]
    if len(parent_fields) == 1:
        parts.append(f"\\[-{parent_fields[0].name} <token>\\]")
    for rf in klass.get_reference_create_fields():
        parts.append(f"\\[-{rf.name} <token>\\]")
    for f in klass.get_create_fields():
        parts.append(f"\\[-{f.name} <{tcl_help_type_label(f)}>\\]")
    parts.append("\\[-help\\]")
    return " ".join(parts) + " - " + klass.tcl_description_escaped()


def update_tcl_help_options(klass) -> str:
    """
    update_tcl_usage()'s own options-list counterpart, mirroring
    create_tcl_help_options() but with every `required` entry forced
    to 0 (see update_tcl_call_args()'s own "nothing is ever required
    to update" rule) and, for a single-parent class, its own optional
    parent-reassignment flag instead of create's always-parent-token
    entry.
    """
    parent_fields = klass.get_parent_fields()
    parts = []
    if len(parent_fields) == 1:
        pf = parent_fields[0]
        desc = f"Move it to this {pf.type} (token)"
        parts.append(f"{{-{pf.name} {{type token required 0 description {{{tcl_brace_escape(desc)}}}}}}}")
    for rf in klass.get_reference_create_fields():
        parts.append(
            f"{{-{rf.name} {{type token required 0 description {{{tcl_brace_escape(rf.description)}}}}}}}"
        )
    for f in klass.get_create_fields():
        parts.append(
            f"{{-{f.name} {{type {tcl_help_type_label(f)} required 0 "
            f"description {{{tcl_brace_escape(tcl_help_description(f))}}}}}}}"
        )
    return " ".join(parts)


def delete_tcl_usage(klass) -> str:
    """
    The one-line `delete_<type> <id> [-help] - <description>` usage
    string registered for this class's generated delete_<type> command
    (and returned by its own `-help` flag) - much narrower than
    create_tcl_usage()/update_tcl_usage(): delete_<type> takes exactly
    one positional `<id>` and no flags of its own (nothing to specify -
    see c_api.delete_api_body() for the cascade a delete triggers). When this
    class owns any tcl_child_list_fields(), the description names every
    child type deleted along with it, so `-help`/`man` surfaces the
    cascade instead of a caller discovering it only after the fact. See
    create_tcl_usage()'s own comment for why every `[`/`]` here is
    backslash-escaped.
    """
    cascade = klass.tcl_child_list_fields()
    if cascade:
        child_types = ", ".join(sorted({f.type for f in cascade}))
        cascade_note = f" Also deletes every {tcl_dquote_escape(child_types)} it owns (recursively, through any of their own owned children too)."
    else:
        cascade_note = ""
    return (
        f"delete_{klass.to_snake_case()} <ids> \\[-help\\] - "
        f"{klass.tcl_description_escaped()}.{cascade_note}"
    )


def delete_tcl_help_options(klass) -> str:
    """
    delete_tcl_usage()'s own options-list counterpart - just the one
    required `<id>` positional (delete_<type> has no flags of its own,
    unlike create_<type>/update_<type> - the call site appends the
    usual `-help` entry itself, same as every other family's own
    registration call).
    """
    desc = f"Token of the {klass.name} to delete, or a list of them"
    return f"{{<ids> {{type token required 1 description {{{tcl_brace_escape(desc)}}}}}}}"


def tcl_help_type_label(field) -> str:
    """
    Human-readable `<type>` label for this field's own create_<type>/
    update_<type> usage-string flag (create_tcl_usage()/
    update_tcl_usage()) and help-options registration (Klass.
    create_tcl_help_options()/update_tcl_help_options()) - this
    field's own scalar/enum/compound type name as-is (str/int/double/
    dbu/bool, an enum's own name, or a compound field's embedded-
    struct Klass name, e.g. "Point"), with a trailing "..." for a
    list_compound_kind() field (e.g. Shape.rects -> "Rect...") since
    its flag actually takes a Tcl list of several such records.
    """
    # dbu/dbu2 values cross the TCL layer in microns / square microns.
    label = {"dbu": "um", "dbu2": "um2", "dbu32": "um"}.get(field.type, field.type)
    if field.type in SMALL_UINT_MAX:
        label = "int"
    return f"{label}..." if field.is_list else label


def cmd_tcl_call_args(field, mode: str = "create") -> List[str]:
    """
    The Tcl expression(s) passing this field's own value from a
    generated `create_<type>`/`update_<type> {args}` proc's own
    `opts` array to its _cmd form, in cmd_param_slots(mode) order.
    A compound field references the locals its own
    _cmd_tcl_compound_preamble() already established (e.g.
    size_x_um/size_y_um), not $opts(-field) directly - the has-flag
    (if needed) is that preamble's own `has_<field>` local too. A
    plain str/enum/required-numeric field passes `$opts(-field)`
    directly (Tcl doesn't distinguish omitted from empty any more
    precisely than the shim layer already does for str/enum). A
    has-flag numeric field passes two expressions (has-flag, value-
    or-0) so an omitted flag can't reach SWIG's int/double typemap as
    an unparsable empty string. A `bool` field's own value slot is
    always boolean-coerced via `[expr {...}]`, since a Tcl caller may
    spell it as a boolean word (true/false/yes/no) that SWIG's plain
    int32 typemap can't parse directly.
    """
    opt = f"$opts(-{field.name})"
    ck = field.compound_klass()
    args: List[str] = []
    if field.create_needs_has_flag(mode):
        if ck is not None or field.list_compound_kind() is not None:
            args.append(f"$has_{field.name}")
        else:
            args.append(f"[expr {{{opt} ne {{}} ? 1 : 0}}]")
    if field.list_compound_kind() is not None:
        # The flat list itself, not a separate count - the shared
        # POINTS_ARRAY_UM SWIG typemap derives the count from the
        # Tcl list's own length (le_api.i), the same as every
        # existing points_um-taking command already relies on.
        args.append(f"${field.name}_flat")
        return args
    if ck is not None:
        for flat, leaf in ck.embedded_scalar_leaves():
            args.append(f"${_leaf_param_name(field.name, flat, leaf)}")
        return args
    if field.type == "bool":
        args.append(f"[expr {{{opt} ne {{}} && {opt}}}]")
        return args
    if field.create_needs_has_flag(mode):
        args.append(f"[expr {{{opt} ne {{}} ? {opt} : 0}}]")
        return args
    args.append(opt)
    return args


def list_compound_tcl_preamble(field, cmd_name: str) -> str:
    """
    cmd_tcl_preamble()'s own list_compound_kind() branch, factored
    out for readability - turns this field's own nested-list flag
    value (`$opts(-rects)` etc.) into `has_<field>` and a single flat
    `<field>_flat` Tcl list, in the wire encoding
    Field.list_compound_parse_lines() parses back apart api.cpp-side:
    "flat" (Rect-like) needs no per-record length prefix, since every
    record has the same fixed arity (arity-checked here instead, a
    real Tcl error naming the flag, the same reason create_<type>'s
    single-struct compound fields already validate arity in Tcl
    rather than leaving a wrong-length flat array to fail deep in
    C++ with no flag context); "points"/"points_plus_scalars" prefix
    the whole list with a record count, then each record with its
    own point count (and, for "points_plus_scalars", its scalar
    field(s) first, in declaration order) - see list_compound_kind()'s
    own docstring for the full per-kind wire format.

    Each entry's own coordinates are
    nested one level per point (e.g. a Rect entry is
    {{llx lly} {urx ury}}, a Polygon/Path entry's own point list is
    {{x0 y0} {x1 y1} ...}) - matching Klass.is_composed_of_records()'s
    same per-point bracing convention used by the single-struct
    "numeric" branch above and by the property-table display side.
    The underlying flat `<field>_flat` wire list this builds (what
    Field.list_compound_parse_lines() actually reads apart api.cpp-
    side) is unchanged either way - only how much bracing a caller
    types is different, all of it stripped back out to flat numbers
    here in Tcl before the _cmd call.
    """
    kind = field.list_compound_kind()
    element_klass = field._type_klass
    vals_var = f"{field.name}_vals"
    has_var = f"has_{field.name}"
    flat_var = f"{field.name}_flat"
    lines = [
        f"    set {vals_var} $opts(-{field.name})",
        f"    set {has_var} [expr {{[llength ${vals_var}] > 0 ? 1 : 0}}]",
        f"    set {flat_var} {{}}",
    ]
    if kind == "flat":
        record_fields = element_klass.is_composed_of_records()
        if record_fields is not None:
            # e.g. Shape.rects/Region.rects: each entry is a Rect,
            # {{llx lly} {urx ury}} - one bracketed point per direct
            # field, mirroring the single-struct "numeric" branch
            # above (Klass.is_composed_of_records()) one level deeper.
            n = len(record_fields)
            sub_leaf_lists = [f._type_klass.embedded_scalar_leaves() for f in record_fields]
            point_vars = [f"pt{i}" for i in range(n)]
            example = " ".join("{" + " ".join(flat for flat, _ in sub) + "}" for sub in sub_leaf_lists)
            lines.append(f"    foreach entry ${vals_var} {{")
            lines.append(f"        if {{[llength $entry] != {n}}} {{")
            lines.append(f'            error "{cmd_name}: -{field.name}: each entry must be {{{example}}}"')
            lines.append("        }")
            lines.append(f"        lassign $entry {' '.join(point_vars)}")
            for sub, pv in zip(sub_leaf_lists, point_vars):
                n_sub = len(sub)
                lines.append(f"        if {{[llength ${pv}] != {n_sub}}} {{")
                lines.append(
                    f'            error "{cmd_name}: -{field.name}: each point needs {n_sub} coordinates, '
                    f'got [llength ${pv}]"'
                )
                lines.append("        }")
                lines.append(f"        foreach c ${pv} {{ lappend {flat_var} $c }}")
            lines.append("    }")
        else:
            # e.g. a flat scalar-record element with nothing further to
            # nest (Point/DensityCheckWindow-shaped) - not exercised by
            # any current list_compound_kind() field, kept for
            # completeness/symmetry with the single-struct branch.
            n = len(element_klass.embedded_scalar_leaves())
            lines.append(f"    foreach entry ${vals_var} {{")
            lines.append(f"        if {{[llength $entry] != {n}}} {{")
            lines.append(f'            error "{cmd_name}: -{field.name}: each entry needs {n} coordinates, got [llength $entry]"')
            lines.append("        }")
            lines.append(f"        foreach c $entry {{ lappend {flat_var} $c }}")
            lines.append("    }")
    elif kind == "points":
        # e.g. Shape.polygons: each entry (a Polygon) is its own point
        # list, {{x0 y0} {x1 y1} ...} - each point individually
        # brace-checked (2 coordinates), rather than the old flat
        # "even number of numbers" check, since a caller now nests.
        lines.append(f"    lappend {flat_var} [llength ${vals_var}]")
        lines.append(f"    foreach entry ${vals_var} {{")
        lines.append(f"        lappend {flat_var} [llength $entry]")
        lines.append("        foreach point $entry {")
        lines.append("            if {[llength $point] != 2} {")
        lines.append(
            f'                error "{cmd_name}: -{field.name}: each point needs 2 coordinates '
            '{x y}, got [llength $point]"'
        )
        lines.append("            }")
        lines.append(f"            foreach c $point {{ lappend {flat_var} $c }}")
        lines.append("        }")
        lines.append("    }")
    else:  # "points_plus_scalars"
        # e.g. Shape.paths: each entry is {width {{x0 y0} {x1 y1} ...}} -
        # scalar field(s) first (unchanged), then a nested point list
        # instead of a flat one, same per-point bracing as "points"
        # above.
        scalar_fields = field.list_compound_scalar_fields()
        scalar_names = " ".join(sf.name for sf in scalar_fields)
        arity = len(scalar_fields) + 1
        example = " ".join(f"<{sf.name}>" for sf in scalar_fields) + " { {x y} {x y} ...}"
        lines.append(f"    lappend {flat_var} [llength ${vals_var}]")
        lines.append(f"    foreach entry ${vals_var} {{")
        lines.append(f"        if {{[llength $entry] != {arity}}} {{")
        lines.append(f'            error "{cmd_name}: -{field.name}: each entry must be {{{example}}}"')
        lines.append("        }")
        lines.append(f"        lassign $entry {scalar_names} points")
        for sf in scalar_fields:
            lines.append(f"        lappend {flat_var} ${sf.name}")
        lines.append(f"        lappend {flat_var} [llength $points]")
        lines.append("        foreach point $points {")
        lines.append("            if {[llength $point] != 2} {")
        lines.append(
            f'                error "{cmd_name}: -{field.name}: each point needs 2 coordinates '
            '{x y}, got [llength $point]"'
        )
        lines.append("            }")
        lines.append(f"            foreach c $point {{ lappend {flat_var} $c }}")
        lines.append("        }")
        lines.append("    }")
    return "\n".join(lines)


def field_cmd_tcl_preamble(field, mode: str, cmd_name: str) -> str:
    """
    Generated Tcl statements for this compound field, emitted (by
    cmd_tcl_preamble(), which loops over every compound create
    field) after flag parsing and before the _cmd call - turns the
    flag's own Tcl list value into its has-flag + leaf-parameter
    locals cmd_tcl_call_args() references. Empty string if this field
    isn't compound or list_compound_kind()-eligible (see
    _list_compound_tcl_preamble() for that branch). `cmd_name` (e.g.
    "create_abstract") is only used to make an arity/keyword error
    message name the actual command.

    The "numeric" branch (Point/Rect/DensityCheckWindow) itself
    splits in two, per Klass.is_composed_of_records(): a plain
    record of scalar leaves (Point, DensityCheckWindow) still parses
    one flat lassign (e.g. -size {x y}); a record composed of further
    records (Rect -> ll/ur, each a Point) parses one bracketed
    sub-list per direct field instead (-bbox {{llx lly} {urx ury}}),
    each recursed into its own flat lassign - matching the same
    nesting depth the property-table display side already produces
    for the same field.
    """
    if field.list_compound_kind() is not None:
        return list_compound_tcl_preamble(field, cmd_name)
    ck = field.compound_klass()
    if ck is None:
        return ""
    leaves = ck.embedded_scalar_leaves()
    vals_var = f"{field.name}_vals"
    has_var = f"has_{field.name}"
    lines = [
        f"    set {vals_var} $opts(-{field.name})",
        f"    set {has_var} [expr {{[llength ${vals_var}] > 0 ? 1 : 0}}]",
    ]
    if ck.compound_leaf_kind() == "numeric":
        record_fields = ck.is_composed_of_records()
        if record_fields is not None:
            # E.g. Rect: one bracketed
            # sub-list per direct field (-bbox {{llx lly} {urx ury}}),
            # not one flat lassign - see Klass.is_composed_of_records()'s
            # own doc comment.
            n = len(record_fields)
            names = " ".join(f.name for f in record_fields)
            lines.append(f"    if {{${has_var} && [llength ${vals_var}] != {n}}} {{")
            lines.append(
                f'        error "{cmd_name}: -{field.name} expects {n} values '
                + "{" + names + "}"
                + ', got [llength $' + vals_var + ']"'
            )
            lines.append("    }")
            sub_leaf_lists = [f._type_klass.embedded_scalar_leaves() for f in record_fields]
            zeros = " ".join("{" + " ".join("0" for _ in sub) + "}" for sub in sub_leaf_lists)
            lines.append(f"    if {{!${has_var}}} {{ set {vals_var} {{{zeros}}} }}")
            sub_vars = [f"{field.name}_{f.name}_vals" for f in record_fields]
            lines.append(f"    lassign ${vals_var} {' '.join(sub_vars)}")
            for f, sub, sub_var in zip(record_fields, sub_leaf_lists, sub_vars):
                n_sub = len(sub)
                lines.append(f"    if {{[llength ${sub_var}] != {n_sub}}} {{")
                lines.append(
                    f'        error "{cmd_name}: -{field.name} {f.name} expects {n_sub} values, '
                    f'got [llength ${sub_var}]"'
                )
                lines.append("    }")
                sub_params = " ".join(
                    _leaf_param_name(field.name, f"{f.name}_{flat}", leaf) for flat, leaf in sub
                )
                lines.append(f"    lassign ${sub_var} {sub_params}")
        else:
            n = len(leaves)
            names = " ".join(flat for flat, _ in leaves)
            lines.append(f"    if {{${has_var} && [llength ${vals_var}] != {n}}} {{")
            lines.append(
                f'        error "{cmd_name}: -{field.name} expects {n} values '
                + "{" + names + "}"
                + ', got [llength $' + vals_var + ']"'
            )
            lines.append("    }")
            zeros = " ".join("0" for _ in leaves)
            lines.append(f"    if {{!${has_var}}} {{ set {vals_var} {{{zeros}}} }}")
            params = " ".join(_leaf_param_name(field.name, flat, leaf) for flat, leaf in leaves)
            lines.append(f"    lassign ${vals_var} {params}")
    else:  # "flags" - Symmetry, keyword-set syntax mirroring LEF's own SYMMETRY X Y R90 ;
        for flat, leaf in leaves:
            lines.append(f"    set {_leaf_param_name(field.name, flat, leaf)} 0")
        lines.append(f"    foreach kw ${vals_var} {{")
        lines.append("        switch -- [string toupper $kw] {")
        for flat, leaf in leaves:
            lines.append(f"            {flat.upper()} {{ set {_leaf_param_name(field.name, flat, leaf)} 1 }}")
        expected = "/".join(flat.upper() for flat, _ in leaves)
        lines.append(
            f'            default {{ error "{cmd_name}: -{field.name}: unknown keyword '
            + '\\"$kw\\" - expected ' + expected + '" }'
        )
        lines.append("        }")
        lines.append("    }")
    return "\n".join(lines)
