"""The generated le::edit create/update/delete ops' bodies (api/edit_ops.cpp)."""

from typing import List


def create_op_body(klass) -> str:
    """
    The statement-list body of le::edit::create_<type>(LeHandle &handle,
    <Klass>Data data) (edit_ops_defs.inc): checks that every parent,
    owner and reference in `data` exists, creates the object, bumps the
    mutation version and records the create into the open transaction.
    The caller holds the handle's exclusive lock. Errors are returned
    without the "create_<type>: " prefix the C API adds when it logs.
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    parent_fields = klass.non_owner_parent_fields()
    owner_fields = klass.get_owner_fields()
    reference_fields = klass.get_reference_create_fields()
    unique_fields = klass.get_unique_per_parent_fields()

    add("le::Root &root = handle.root;")
    if len(parent_fields) == 1:
        pf = parent_fields[0]
        add(f"if (!root.get_{pf._parent_klass.to_snake_case()}(data.{pf.name}))")
        add(f'    return std::unexpected("unknown {pf.name} - no such {pf.type} exists");')
    elif parent_fields:
        add(f"// Exactly one parent must resolve - a {klass.name} belongs to exactly one of these.")
        add("int32_t provided_parent_count = 0;")
        for pf in parent_fields:
            add(f"if (root.get_{pf._parent_klass.to_snake_case()}(data.{pf.name}))")
            add("    ++provided_parent_count;")
        add("if (provided_parent_count != 1)")
        add(
            f'    return std::unexpected(fmt::format("exactly one of '
            f'{"/".join(p.name for p in parent_fields)} must resolve to a valid parent, got {{}}", '
            f"provided_parent_count));"
        )

    if owner_fields:
        add("switch (data.owner.kind)")
        add("{")
        for f in owner_fields:
            add(f"case le::{klass.owner_type_name()}Kind::{f.owner_kind()}:")
            add(f"    if (!root.get_{f._parent_klass.to_snake_case()}(le::{f.type}Id{{data.owner.index, data.owner.generation}}))")
            add(f'        return std::unexpected("unknown {f.name} owner - no such {f.type} exists");')
            add("    break;")
        add("default:")
        add('    return std::unexpected("an owner is required");')
        add("}")

    for rf in reference_fields:
        getter = f"root.get_{rf._type_klass.to_snake_case()}(data.{rf.name})"
        # An optional reference may be left unset; a set one must exist.
        condition = f"!{getter}" if rf.create_required() else f"data.{rf.name}.valid() && !{getter}"
        add(f"if ({condition})")
        add(f'    return std::unexpected("unknown {rf.name} - no such {rf.type} exists");')

    for sf in klass.get_singular_parent_fields():
        add(f"if (data.{sf.accessor}.valid() && root.get_{sf._parent_klass.to_snake_case()}_{sf.parent}(data.{sf.accessor}).valid())")
        add(f'    return std::unexpected("the {sf.name} already has a {sf.parent} - delete or update that {klass.name} instead");')

    for gf in klass.get_global_unique_fields():
        add(f"if (root.get_{snake}_by_{gf.name}(data.{gf.name}).valid())")
        add(f'    return std::unexpected(fmt::format("a {klass.name} with this {gf.name} (\'{{}}\') already exists", data.{gf.name}));')
    if klass.create_check:
        add(f"if (const std::optional<std::string> refused = {klass.create_check}(root, data))")
        add("    return std::unexpected(*refused);")

    add()
    if unique_fields:
        add(f"const auto {unique_fields[0].name}_for_error = data.{unique_fields[0].name};")
    add(f"const le::{klass.name}Id created = root.create_{snake}(std::move(data));")
    if unique_fields:
        field = unique_fields[0]
        add("if (!created.valid())")
        add(
            f'    return std::unexpected(fmt::format("a sibling {klass.name} with this '
            f"{field.name} ('{{}}') already exists\", {field.name}_for_error));"
        )
    add("root.bump_mutation_version();")
    add("if (le::editing::Transaction *txn = handle.command_history.current())")
    add("    txn->record_create(created);")
    add("return created;")
    return "\n".join(lines)


def update_op_body(klass) -> str:
    """
    The statement-list body of le::edit::update_<type>(LeHandle &handle,
    <Klass>Id id, <Klass>Changes changes) (edit_ops_defs.inc): checks
    the object and every new parent/reference exist, applies the
    changes with one Root::update_<type>() call, bumps the mutation
    version and records the before/after snapshots into the open
    transaction. The caller holds the handle's exclusive lock.
    """
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    parent_fields = klass.get_parent_fields()
    create_fields = klass.get_create_fields()
    reference_fields = klass.get_reference_create_fields()
    unique_fields = klass.get_unique_per_parent_fields()
    single_parent = parent_fields[0] if len(parent_fields) == 1 else None

    add("le::Root &root = handle.root;")
    add(f"const le::{klass.name}Data *existing = root.get_{snake}(id);")
    add("if (!existing)")
    add('    return std::unexpected("unknown id");')
    add(f"const le::{klass.name}Data before = *existing;")
    if single_parent is not None:
        add(f"if (changes.{single_parent.name} && !root.get_{single_parent._parent_klass.to_snake_case()}(*changes.{single_parent.name}))")
        add(f'    return std::unexpected("unknown {single_parent.name} - no such {single_parent.type} exists");')
        if not single_parent._parent_field.is_list:
            getter = f"root.get_{single_parent._parent_klass.to_snake_case()}_{single_parent.parent}"
            add(f"if (changes.{single_parent.name} && *changes.{single_parent.name} != before.{single_parent.name} && {getter}(*changes.{single_parent.name}).valid())")
            add(f'    return std::unexpected("the new {single_parent.name} already has a {single_parent.parent}");')
    for rf in reference_fields:
        add(f"if (changes.{rf.name} && !root.get_{rf._type_klass.to_snake_case()}(*changes.{rf.name}))")
        add(f'    return std::unexpected("unknown {rf.name} - no such {rf.type} exists");')
    for gf in klass.get_global_unique_fields():
        add(f"if (changes.{gf.name} && *changes.{gf.name} != before.{gf.name} && root.get_{snake}_by_{gf.name}(*changes.{gf.name}).valid())")
        add(f'    return std::unexpected(fmt::format("a {klass.name} with this {gf.name} (\'{{}}\') already exists", *changes.{gf.name}));')
    if unique_fields and single_parent is not None:
        uf = unique_fields[0]
        pf = single_parent
        add("{")
        add(f"    const le::{pf.type}Id target_parent = changes.{pf.name} ? *changes.{pf.name} : before.{pf.name};")
        add(f"    const auto &target_{uf.name} = changes.{uf.name} ? *changes.{uf.name} : before.{uf.name};")
        add(f"    if ((target_parent != before.{pf.name} || target_{uf.name} != before.{uf.name}) && target_parent.valid() &&")
        add(f"        root.get_{snake}_by_{uf.name}(target_parent, target_{uf.name}).valid())")
        add(f'        return std::unexpected(fmt::format("a sibling {klass.name} with this {uf.name} (\'{{}}\') already exists", target_{uf.name}));')
        add("}")
    if klass.update_check:
        add(f"if (const std::optional<std::string> refused = {klass.update_check}(root, before, changes))")
        add("    return std::unexpected(*refused);")

    call_args = ["id"]
    if single_parent is not None:
        call_args.append(f"changes.{single_parent.name}.value_or(le::{single_parent.type}Id{{}})")
    for rf in reference_fields:
        call_args.append(f"changes.{rf.name}")
    for f in create_fields:
        call_args.append(f"std::move(changes.{f.name})")
    add(f"if (!root.update_{snake}({', '.join(call_args)}))")
    add('    return std::unexpected("internal error: the database rejected an update that passed its checks");')
    add("root.bump_mutation_version();")
    add("if (handle.command_history.is_recording())")
    add(f"    handle.command_history.current()->record_update<le::{klass.name}Id, le::{klass.name}Data>(")
    add(f"        id, before, *root.get_{snake}(id), &le::apply_{snake}_snapshot);")
    add("return {};")
    return "\n".join(lines)


def delete_op_body(klass) -> str:
    """
    The statement-list body of le::edit::delete_<type>(LeHandle &handle,
    <Klass>Id id) (edit_ops_defs.inc); the caller holds the handle's
    exclusive lock. Deleting a <Klass> also deletes every descendant
    reachable through tcl_child_list_fields() (an owned child such as
    a TerminalPort is only reachable through its parent, so leaving it
    behind would orphan it), however deep the schema subtree goes.

    The recursion is unrolled at codegen time: each level of the
    subtree becomes one flat loop collecting that level's children
    across every parent at the level above. Every id is collected
    before anything is deleted, since deleting a child edits its
    parent's list. Deletion runs deepest level first and each level
    newest first, so every child comes off the end of its parent's
    list; this object goes last. Each goes through
    le::editing::delete_object, which records it
    when a transaction is open; undo replays in reverse, so each
    parent is restored before its children, and each child goes back
    at its own id and list position.
    """
    return delete_body(klass, batch=False)


def delete_batch_op_body(klass) -> str:
    """
    The statement-list body of le::edit::delete_<type>_batch(LeHandle
    &handle, std::span<const <Klass>Id> ids): delete_op_body()'s
    cascade for many objects at once. Every id must exist (nothing is
    deleted otherwise); repeats are ignored. Each level, and the ids
    themselves, go through le::editing::delete_objects as one batch,
    so each child list they leave is compacted once rather than
    shifted per object - linear in the objects deleted plus the lists
    they were in.
    """
    return delete_body(klass, batch=True)


def delete_body(klass, batch: bool) -> str:
    indent = "        "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    root_fields = klass.tcl_child_list_fields()

    if batch:
        add(f"for (const le::{klass.name}Id {snake}_id : ids)")
        add(f"    if (!handle.root.get_{snake}({snake}_id))")
        add('        return std::unexpected("unknown id");')
    else:
        add(f"const le::{klass.name}Id {snake}_id = id;")
        add(f"if (!handle.root.get_{snake}({snake}_id))")
        add('    return std::unexpected("unknown id");')
    add("le::editing::Transaction *txn = handle.command_history.current();")

    # `levels` ends up deepest-first: plan_edge() recurses into a
    # child's own children before appending that child's level.
    levels: List[dict] = []
    counter = [0]

    def plan_edge(owner_klass: "Klass", fld: "Field", owner_ids_expr: str, owner_is_scalar: bool) -> None:
        child_klass = fld._child_klass
        if child_klass is None:
            raise ValueError(f"delete_op_body: {owner_klass.name}.{fld.name} (type={fld.type!r}) has no resolved child class")
        idx = counter[0]
        counter[0] += 1
        owner_snake = owner_klass.to_snake_case()
        ids_var = f"casc{idx}_{fld.name}_ids"

        add()
        if owner_is_scalar:
            add(f"const std::vector<le::{fld.type}Id> {ids_var} = handle.root.get_{owner_snake}_{fld.name}({owner_ids_expr});")
        else:
            add(f"std::vector<le::{fld.type}Id> {ids_var};")
            add(f"for (const le::{owner_klass.name}Id owner_id : {owner_ids_expr})")
            add(f"    for (const le::{fld.type}Id child_id : handle.root.get_{owner_snake}_{fld.name}(owner_id))")
            add(f"        {ids_var}.push_back(child_id);")

        for grandchild_field in child_klass.tcl_child_list_fields():
            plan_edge(child_klass, grandchild_field, ids_var, False)

        levels.append({"type": fld.type, "ids_var": ids_var})

    for fld in root_fields:
        plan_edge(klass, fld, "ids" if batch else f"{snake}_id", not batch)

    add()
    if batch:
        for level in levels:
            add(f"le::editing::delete_objects(handle.root, txn, std::span<const le::{level['type']}Id>({level['ids_var']}));")
        add("const bool deleted = le::editing::delete_objects(handle.root, txn, ids) > 0 || ids.empty();")
    else:
        # Newest first within each level: each child is then its
        # parent's last, so taking it off the parent's list is O(1).
        for level in levels:
            add(f"for (auto child = {level['ids_var']}.rbegin(); child != {level['ids_var']}.rend(); ++child)")
            add("    le::editing::delete_object(handle.root, txn, *child);")
        add(f"const bool deleted = le::editing::delete_object(handle.root, txn, {snake}_id);")
    add("handle.root.bump_mutation_version();")
    add("if (!deleted)")
    add('    return std::unexpected("delete failed");')
    add("return {};")
    return "\n".join(lines)
