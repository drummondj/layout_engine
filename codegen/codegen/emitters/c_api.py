"""The generated C API functions' bodies (api.cpp): create, update and delete by C arguments, and applying a snapshot for undo."""

from typing import Callable, List
from codegen.schema import SMALL_UINT_MAX


def _fail(native: bool, op: str, invalid: str) -> Callable[..., List[str]]:
    """
    The statements reporting a failure of `op` and leaving the body: in the
    C API, log `op: message` and return `invalid`; in a native body (one
    returning std::expected), return the message as the error. `args` are
    the message's format arguments.
    """

    def fail(message: str, args: str = "") -> List[str]:
        if native:
            text = f'std::format("{message}", {args})' if args else f'std::string("{message}")'
            return [f"return std::unexpected({text});"]
        return [f'spdlog::error("{op}: {message}"{", " + args if args else ""});', f"return {invalid};"]

    return fail


def create_api_body(klass, native: bool = False) -> str:
    """
    The statement-list body of le_create_<type>(LeHandle *handle,
    <create_api_params()>): the C-to-C++ half - required-null checks,
    enum parsing, micron-to-dbu conversion, flat coordinate lists, id
    conversion - building a <Klass>Data for le::edit::create_<type>
    (edit_ops.create_op_body()), which validates, creates and records undo.

    With `native`, the same conversion for the Tcl shim's
    create_<type>_native(le::WriteView &view, <create_native_params()>):
    ids are already native, nothing is locked or logged, and it returns
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
    fail = _fail(native, f"create_{snake}", "invalid")

    def add_fail(message: str, args: str = "", prefix: str = "    ") -> None:
        for line in fail(message, args):
            add(f"{prefix}{line}")

    id_expr = (lambda name: name) if native else (lambda name: f"from_c({name})")
    parent_fields = klass.non_owner_parent_fields()
    owner_fields = klass.get_owner_fields()
    create_fields = klass.get_create_fields()
    reference_fields = klass.get_reference_create_fields()
    enum_fields = [f for f in create_fields if f.is_enum_type()]
    small_uint_fields = [f for f in create_fields if f.type in SMALL_UINT_MAX]
    dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]

    if not native:
        add(f"const Le{klass.name}Id invalid{{.index = UINT32_MAX, .generation = 0}};")
        add("if (!handle)")
        add("    return invalid;")
    for f in create_fields:
        if (f.type == "str" or f.is_enum_type()) and f.create_required():
            add(f"if (!{f.create_c_param_name()})")
            add("{")
            add_fail(f"{f.name} is required")
            add("}")
    if not native:
        add("HandleWriteLock lock(handle);  // writer - mutates Root")

    if parent_fields:
        add()
        for pf in parent_fields:
            add(f"const le::{pf.type}Id {pf.name} = {id_expr(pf.name + '_id')};")

    if owner_fields:
        owner_type = f"le::{klass.owner_type_name()}"
        add()
        if native:
            # Only the kind is checked here; le::edit::create_<type> checks
            # that the owner exists.
            add(f"const {owner_type} owner_value = owner;")
            add("if (!owner_value.valid())")
            add("{")
            add_fail("an owner is required")
            add("}")
        else:
            add(f"{owner_type} owner_value{{}};")
            add("switch (owner.kind)")
            add("{")
            for f in owner_fields:
                add(f"case {klass.owner_c_kind(f)}:")
                add(f"    owner_value = {owner_type}{{le::{klass.owner_type_name()}Kind::{f.owner_kind()}, owner.index, owner.generation}};")
                add("    break;")
            add("default:")
            add_fail("an owner is required")
            add("}")

    if reference_fields:
        add()
        for rf in reference_fields:
            add(f"const le::{rf.type}Id {rf.name} = {id_expr(rf.name + '_id')};")

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
            scale = "units_dbu_per_um(view)" if native else "::database_units_microns(handle->root)"
            add(f"const std::optional<double> dbu_per_um = {scale};")
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
    if native:
        add(f"return view.create_{snake}(std::move(new_data));")
        return "\n".join(lines)
    add(f"const std::expected<le::{klass.name}Id, std::string> created = le::edit::create_{snake}(*handle, std::move(new_data));")
    add("if (!created)")
    add("{")
    add(f'    spdlog::error("create_{snake}: {{}}", created.error());')
    add("    return invalid;")
    add("}")
    add("return to_c(*created);")

    return "\n".join(lines)


def update_api_body(klass, native: bool = False) -> str:
    """
    The statement-list body of le_update_<type>(LeHandle *handle,
    <update_api_params()>): the C-to-C++ half, turning each has-flag/value
    slot into a <Klass>Changes member for le::edit::update_<type>
    (edit_ops.update_op_body()). Every enum field is parsed the way
    create's optional branch does. dbu_per_um is fetched whenever this
    class has a dbu-using create field; that's safe because each
    Field.update_root_arg_expr() is a ternary that only evaluates its
    value (and dereferences dbu_per_um) when the field's has-flag is set.

    With `native`, the same for the Tcl shim's update_<type>_native
    (le::WriteView &view, <update_native_params()>), returning
    std::expected<void, std::string> (see create_api_body()).
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    fail = _fail(native, f"update_{snake}", "1")

    def add_fail(message: str, args: str = "", prefix: str = "    ") -> None:
        for line in fail(message, args):
            add(f"{prefix}{line}")

    id_expr = (lambda name: name) if native else (lambda name: f"from_c({name})")
    root = "view.root()" if native else "handle->root"
    parent_fields = klass.get_parent_fields()
    create_fields = klass.get_create_fields()
    reference_fields = klass.get_reference_create_fields()
    enum_fields = [f for f in create_fields if f.is_enum_type()]
    dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]
    single_parent = parent_fields[0] if len(parent_fields) == 1 else None

    if not native:
        add("if (!handle)")
        add("    return 1;")
        add("HandleWriteLock lock(handle);  // writer - mutates Root")
        add()
    add(f"const le::{klass.name}Id typed_id = {id_expr('id')};")

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
            scale = "units_dbu_per_um(view)" if native else "::database_units_microns(handle->root)"
            add(f"const std::optional<double> dbu_per_um = {scale};")
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
        add(f"    changes.{single_parent.name} = {id_expr(single_parent.name + '_id')};")
    for rf in reference_fields:
        add(f"if (has_{rf.name})")
        add(f"    changes.{rf.name} = {id_expr(rf.name + '_id')};")
    for f in create_fields:
        add(f"changes.{f.name} = {f.update_root_arg_expr()};")
    if native:
        add(f"return view.update_{snake}(typed_id, std::move(changes));")
        return "\n".join(lines)
    add(f"const std::expected<void, std::string> updated = le::edit::update_{snake}(*handle, typed_id, std::move(changes));")
    add("if (!updated)")
    add("{")
    add(f'    spdlog::error("update_{snake}: {{}}", updated.error());')
    add("    return 1;")
    add("}")
    add("return 0;")

    return "\n".join(lines)


def apply_snapshot_body(klass) -> str:
    """
    The full statement-list body of `inline bool apply_<snake>_snapshot
    (Root &root, <Klass>Id id, const <Klass>Data &data)`
    (api_snapshot_appliers_hpp_j2.py) - restores a *whole* field
    snapshot through one real Root::update_<klass>() call, reusing the
    same field list/single-parent handling update_root_params() does,
    so every index stays consistent. Each create field is passed as
    `std::optional<T>(data.<field>)` (T = root_value_cpp_type()). For
    an is_optional field that is unset in the snapshot that's an empty
    optional, which update_<klass>() reads as "leave unchanged", so
    those fields are then copied directly. None is indexed, and the
    update already logged the object as changed.
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    parent_fields = klass.get_parent_fields()
    create_fields = klass.get_create_fields()
    single_parent = parent_fields[0] if len(parent_fields) == 1 else None

    call_args = ["id"]
    if single_parent is not None:
        call_args.append(f"data.{single_parent.name}")
    for rf in klass.get_reference_create_fields():
        call_args.append(f"std::optional<{rf.type}Id>(data.{rf.name})")
    for f in create_fields:
        call_args.append(f"std::optional<{f.root_value_cpp_type(qualified=False)}>(data.{f.name})")

    optional_fields = [f for f in create_fields if f.is_optional]
    if not optional_fields:
        add(f"return root.update_{snake}({', '.join(call_args)});")
        return "\n".join(lines)
    add(f"if (!root.update_{snake}({', '.join(call_args)}))")
    add("    return false;")
    add(f"{klass.name}Data *stored = root.get_{snake}(id);")
    for f in optional_fields:
        add(f"stored->{f.name} = data.{f.name};")
    add("return true;")

    return "\n".join(lines)


def delete_api_body(klass) -> str:
    """
    The full statement-list body of le_delete_<type>(LeHandle *handle,
    Le<Klass>Id id): locks and calls le::edit::delete_<type>
    (edit_ops.delete_op_body()). Returns 1, without logging, if the id is unknown.
    """
    snake = klass.to_snake_case()
    return "\n".join(
        [
            "        if (!handle)",
            "            return 1;",
            "        HandleWriteLock lock(handle);  // writer - mutates Root",
            f"        return le::edit::delete_{snake}(*handle, from_c(id)) ? 0 : 1;",
        ]
    )
