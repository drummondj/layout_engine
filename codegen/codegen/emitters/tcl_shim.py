"""The Tcl shim's create_<type>_native/update_<type>_native bodies
(le_tcl_shim_generated.inc): Tcl's arguments (C strings, has-flags,
microns, flat coordinate lists) turned into a <Klass>Data or
<Klass>Changes for the WriteView edit."""

from typing import List
from codegen.schema import SMALL_UINT_MAX


def _fail(message: str, args: str = "") -> List[str]:
    """The statement returning `message` (a std::format string over
    `args`) as the body's error."""
    text = f'std::format("{message}", {args})' if args else f'std::string("{message}")'
    return [f"return std::unexpected({text});"]


def create_native_body(klass) -> str:
    """
    The statement-list body of the Tcl shim's create_<type>_native
    (le::WriteView &view, <create_native_params()>): required-null
    checks, enum parsing, micron-to-dbu conversion and flat coordinate
    lists, building a <Klass>Data for WriteView::create_<type>, which
    validates, creates and records undo. Returns
    std::expected<le::<Klass>Id, std::string>, an error carrying its
    reason. Built as one Python string rather than nested Jinja
    conditionals, since the per-field-type/per-optionality branching is
    easier to keep readable as real Python control flow.
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    fail = _fail

    def add_fail(message: str, args: str = "", prefix: str = "    ") -> None:
        for line in fail(message, args):
            add(f"{prefix}{line}")

    parent_fields = klass.non_owner_parent_fields()
    owner_fields = klass.get_owner_fields()
    create_fields = klass.get_create_fields()
    reference_fields = klass.get_reference_create_fields()
    enum_fields = [f for f in create_fields if f.is_enum_type()]
    small_uint_fields = [f for f in create_fields if f.type in SMALL_UINT_MAX]
    dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]

    for f in create_fields:
        if (f.type == "str" or f.is_enum_type()) and f.create_required():
            add(f"if (!{f.create_c_param_name()})")
            add("{")
            add_fail(f"{f.name} is required")
            add("}")

    if parent_fields:
        add()
        for pf in parent_fields:
            add(f"const le::{pf.type}Id {pf.name} = {pf.name}_id;")

    if owner_fields:
        owner_type = f"le::{klass.owner_type_name()}"
        add()
        # Only the kind is checked here; WriteView::create_<type> checks
        # that the owner exists.
        add(f"const {owner_type} owner_value = owner;")
        add("if (!owner_value.valid())")
        add("{")
        add_fail("an owner is required")
        add("}")

    if reference_fields:
        add()
        for rf in reference_fields:
            add(f"const le::{rf.type}Id {rf.name} = {rf.name}_id;")

    if enum_fields:
        add()
        for f in enum_fields:
            enum_snake = f._type_klass.to_snake_case()
            if f.create_required():
                add(f"const std::optional<le::{f.type}> parsed_{f.name} = le::{enum_snake}_from_string({f.name});")
                add(f"if (!parsed_{f.name})")
            else:
                add(f"const std::optional<le::{f.type}> parsed_{f.name} = ({f.name} && {f.name}[0]) ? le::{enum_snake}_from_string({f.name}) : std::nullopt;")
                add(f"if ({f.name} && {f.name}[0] && !parsed_{f.name})")
            add("{")
            add_fail(f"unrecognized {f.name} \'{{}}\'", f.name)
            add("}")

    for f in small_uint_fields:
        add()
        out_of_range = f"{f.name} < 0 || {f.name} > {SMALL_UINT_MAX[f.type]}"
        add(f"if ({out_of_range})" if f.create_required() else f"if (has_{f.name} && ({out_of_range}))")
        add("{")
        add_fail(f"{f.name} {{}} is out of range (0 to {SMALL_UINT_MAX[f.type]})", f.name)
        add("}")

    if dbu_fields:
        add()
        if klass._owns_dbu_scale():
            # Technology's own dbu fields convert through the scale
            # being created alongside them, not an existing one - and
            # the parameter of that name would shadow the helper.
            add("const std::optional<double> dbu_per_um = database_units_microns > 0.0 ? std::optional<double>{database_units_microns} : std::nullopt;")
            add("if (!dbu_per_um)")
            add("{")
            add_fail("database_units_microns must be positive (needed for micron-to-dbu conversion)")
        else:
            add("const std::optional<double> dbu_per_um = units_dbu_per_um(view);")
            add("if (!dbu_per_um)")
            add("{")
            add_fail("no Technology has been read yet (needed for micron-to-dbu conversion)")
        add("}")

    list_compound_fields = [f for f in create_fields if f.list_compound_kind() is not None]
    for f in list_compound_fields:
        add()
        lines.extend(f.list_compound_parse_lines("create", fail))

    add()
    add(f"le::{klass.name}Data new_data{{")
    # Designated initializers must appear in declaration order
    # (klass.fields, not the role groupings above): a plain reference
    # field can be declared anywhere among its class's create fields.
    if owner_fields:
        add("    .owner = owner_value,")  # `owner` is the struct's first member
    for f in klass.fields:
        if f.owner:
            continue
        if f in parent_fields or f in reference_fields:
            add(f"    .{f.name} = {f.name},")
        elif f in create_fields:
            add(f"    .{f.name} = {f.create_struct_init_expr()},")
    add("};")
    add(f"return view.create_{snake}(std::move(new_data));")
    return "\n".join(lines)


def update_native_body(klass) -> str:
    """
    The statement-list body of the Tcl shim's update_<type>_native
    (le::WriteView &view, <update_native_params()>): turns each
    has-flag/value slot into a <Klass>Changes member for
    WriteView::update_<type>, returning std::expected<void, std::string>
    (see create_native_body()). Every enum field is parsed the way
    create's optional branch does. dbu_per_um is fetched whenever this
    class has a dbu-using create field; that's safe because each
    Field.update_root_arg_expr() is a ternary that only evaluates its
    value (and dereferences dbu_per_um) when the field's has-flag is set.
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    fail = _fail

    def add_fail(message: str, args: str = "", prefix: str = "    ") -> None:
        for line in fail(message, args):
            add(f"{prefix}{line}")

    root = "view.root()"
    parent_fields = klass.get_parent_fields()
    create_fields = klass.get_create_fields()
    reference_fields = klass.get_reference_create_fields()
    enum_fields = [f for f in create_fields if f.is_enum_type()]
    dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]
    single_parent = parent_fields[0] if len(parent_fields) == 1 else None

    add("const le::{klass.name}Id typed_id = id;".replace("{klass.name}", klass.name))

    if enum_fields:
        add()
        for f in enum_fields:
            enum_snake = f._type_klass.to_snake_case()
            add(
                f"const std::optional<le::{f.type}> parsed_{f.name} = ({f.name} && {f.name}[0]) "
                f"? le::{enum_snake}_from_string({f.name}) : std::nullopt;"
            )
            add(f"if ({f.name} && {f.name}[0] && !parsed_{f.name})")
            add("{")
            add_fail(f"unrecognized {f.name} \'{{}}\'", f.name)
            add("}")

    for f in create_fields:
        if f.type not in SMALL_UINT_MAX:
            continue
        add()
        add(f"if (has_{f.name} && ({f.name} < 0 || {f.name} > {SMALL_UINT_MAX[f.type]}))")
        add("{")
        add_fail(f"{f.name} {{}} is out of range (0 to {SMALL_UINT_MAX[f.type]})", f.name)
        add("}")

    if dbu_fields:
        add()
        if klass._owns_dbu_scale():
            # The scale is this object's own, possibly being updated too.
            add(f"const le::{klass.name}Data *existing_{snake} = {root}.get_{snake}(typed_id);")
            add(f"if (!existing_{snake})")
            add("{")
            add_fail("unknown id")
            add("}")
            add(f"const double own_dbu_per_um = has_database_units_microns ? database_units_microns : existing_{snake}->database_units_microns;")
            add("const std::optional<double> dbu_per_um = own_dbu_per_um > 0.0 ? std::optional<double>{own_dbu_per_um} : std::nullopt;")
            add("if (!dbu_per_um)")
            add("{")
            add_fail("database_units_microns must be positive (needed for micron-to-dbu conversion)")
        else:
            add("const std::optional<double> dbu_per_um = units_dbu_per_um(view);")
            add("if (!dbu_per_um)")
            add("{")
            add_fail("no Technology has been read yet (needed for micron-to-dbu conversion)")
        add("}")

    list_compound_fields = [f for f in create_fields if f.list_compound_kind() is not None]
    for f in list_compound_fields:
        add()
        lines.extend(f.list_compound_parse_lines("update", fail))

    add()
    add(f"le::{klass.name}Changes changes;")
    if single_parent is not None:
        add(f"if (has_{single_parent.name})")
        add(f"    changes.{single_parent.name} = {single_parent.name}_id;")
    for rf in reference_fields:
        add(f"if (has_{rf.name})")
        add(f"    changes.{rf.name} = {rf.name}_id;")
    for f in create_fields:
        add(f"changes.{f.name} = {f.update_root_arg_expr()};")
    add(f"return view.update_{snake}(typed_id, std::move(changes));")
    return "\n".join(lines)
