"""Root::update_<type>'s body (generated root.hpp)."""

from typing import List


def update_root_body(klass) -> str:
    """
    The statement-list body of Root::update_<klass>
    (update_root_params()), in two phases so a refused update changes
    nothing: first every check (the reparent's single-slot parent, the
    unique_per_parent name under the resulting parent with the
    resulting name, each global unique field), then the writes, which
    can't fail. The name check uses the resulting parent and name
    together, so a reparent plus rename is checked as one move.
    """
    indent = "            "
    lines: List[str] = []

    def add(text: str = "") -> None:
        lines.append(f"{indent}{text}" if text else "")

    snake = klass.to_snake_case()
    parent_fields = klass.get_parent_fields()
    create_fields = klass.get_create_fields()
    unique_fields = klass.get_unique_per_parent_fields()
    single_parent = parent_fields[0] if len(parent_fields) == 1 else None
    uf = unique_fields[0] if unique_fields and single_parent is not None else None
    global_fields = [f for f in create_fields if f.index and not f.unique_per_parent]

    add(f"auto* existing = {snake}_.get(id);")
    add("if (!existing) return false;")

    if single_parent is not None:
        pf = single_parent
        parent_index = f"index_.{pf._parent_klass.to_snake_case()}_{pf.parent}"
        add(f"const bool reparent = {pf.name}.valid() && {pf.name} != existing->{pf.name};")
        if not pf._parent_field.is_list:
            add(f"if (reparent && {parent_index}.contains({pf.name}))")
            add("    return false;")
    if uf is not None:
        names = f"index_.{snake}_by_{uf.name}"
        add(f"const bool rename = {uf.name} && *{uf.name} != existing->{uf.name};")
        add(f"const {pf.type}Id target_parent = reparent ? {pf.name} : existing->{pf.name};")
        add("if ((reparent || rename) && target_parent.valid())")
        add("{")
        add(f"    const auto bucket = {names}.find(target_parent);")
        add(f"    if (bucket != {names}.end() && bucket->second.find(rename ? *{uf.name} : existing->{uf.name}) != bucket->second.end())")
        add("        return false;")
        add("}")
    for gf in global_fields:
        add(f"if ({gf.name} && *{gf.name} != existing->{gf.name} && index_.{snake}_by_{gf.name}.contains(*{gf.name}))")
        add("    return false;")

    add()
    add("// Nothing below can fail.")
    if uf is not None:
        add("if (reparent || rename)")
        add("{")
        add(f"    const auto bucket = {names}.find(existing->{pf.name});")
        add(f"    if (bucket != {names}.end())")
        add(f"        bucket->second.erase(existing->{uf.name});")
        add("}")
    if single_parent is not None:
        add("if (reparent)")
        add("{")
        if pf._parent_field.is_list:
            add(f"    auto& old_siblings = {parent_index}[existing->{pf.name}];")
            add("    old_siblings.erase(std::remove(old_siblings.begin(), old_siblings.end(), id), old_siblings.end());")
            add(f"    existing->{pf.name} = {pf.name};")
            add(f"    {parent_index}[{pf.name}].push_back(id);")
        else:
            add(f"    const auto old_it = {parent_index}.find(existing->{pf.name});")
            add(f"    if (old_it != {parent_index}.end() && old_it->second == id)")
            add(f"        {parent_index}.erase(old_it);")
            add(f"    existing->{pf.name} = {pf.name};")
            add(f"    {parent_index}[{pf.name}] = id;")
        add("}")

    for rf in klass.get_reference_create_fields():
        add(f"if ({rf.name}) existing->{rf.name} = *{rf.name};")

    for f in create_fields:
        if f in global_fields:
            add(f"if ({f.name} && *{f.name} != existing->{f.name})")
            add("{")
            add(f"    index_.{snake}_by_{f.name}.erase(existing->{f.name});")
            add(f"    existing->{f.name} = std::move(*{f.name});")
            add(f"    index_.{snake}_by_{f.name}[existing->{f.name}] = id;")
            add("}")
        else:
            add(f"if ({f.name}) existing->{f.name} = std::move(*{f.name});")

    if uf is not None:
        add(f"if ((reparent || rename) && existing->{pf.name}.valid())")
        add(f"    {names}[existing->{pf.name}][existing->{uf.name}] = id;")

    add()
    add("return true;")
    return "\n".join(lines)
