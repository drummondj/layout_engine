"""Computes each TCL-readable class's default get_<type> search scope
(the candidate list used when the caller omits every `-of` parameter) -
purely from schema structure (Klass.has_current_access, parent/is_child
fields), no hand-maintained per-class table.

## The algorithm

A class `Y`'s default scope falls into exactly one of four cases, checked
in order:

1. **`Y.has_current_access`** (Abstract/Schematic/Technology themselves):
   default = `[current_Y]` if set and valid, else `[]`.

2. **`Y` is reachable by walking *down* `is_child` list fields from some
   `has_current_access` class `A`** (e.g. Terminal under Abstract, Layer
   under Technology, Shape under Abstract via two distinct paths -
   TerminalPort and Obstruction, both unioned): default = descend from
   `current_A` through every such path, unioning results. Degrades to `[]`
   if `current_A` isn't set (Root's own generated child accessors return
   empty for an invalid id).

3. **`Y` is reachable by walking *down* any `is_child` field (list or
   scalar) from `Y` to some `has_current_access` class `A`** (e.g. Design
   contains Abstract via the scalar `Design.abstract` field) - i.e. `Y` is
   an *ancestor* of `A`: default = every sibling of "the `Y` instance
   containing `current_A`" - walk *up* from `current_A` via parent fields
   until reaching an instance of `Y`, then take that instance's own
   parent's full list of `Y`-children. Degrades to *every* `Y` (flat scan)
   if `current_A` isn't set - this asymmetry vs. case 2 matches the
   pre-existing hand-written `le_get_designs` exactly.

4. **Neither** (no parent at all, e.g. Library; or, defensively, any
   future class this graph walk can't relate to an anchor): flat scan of
   every instance.

Every class's `-of <parent>` handling is independent of this and always
generated the same way regardless of case: one `of_<parent>` parameter per
`Field.has_parent()` field (almost always one; `Shape` has two -
`terminal_port` and `obstruction` - unioned exactly like `TerminalPort`/
`Obstruction`'s own `-of` would be). The case above only supplies the
fallback used when *none* of those `-of` parameters were given/valid.
"""

from dataclasses import dataclass, field
from typing import List, Optional

from codegen.schema import Field, Klass, Schema


@dataclass
class OfParam:
    """One `-of <parent>` axis of a get_<type> command - one per parent field."""

    parent_field: Field  # the field on `klass` naming this parent (e.g. Shape.terminal_port)
    sibling_field: Field  # the is_child list field on the parent klass enumerating `klass` (e.g. TerminalPort.shapes)

    @property
    def parent_klass(self) -> Klass:
        return self.parent_field._type_klass


@dataclass
class SelfScope:
    """Case 1: klass is itself has_current_access."""

    klass: Klass


@dataclass
class DescendantScope:
    """Case 2: klass is reached by descending is_child list-field paths from anchor."""

    anchor: Klass
    paths: List[List[Field]]  # each path: is_child list Fields from anchor down to klass


@dataclass
class AncestorScope:
    """Case 3: klass is an ancestor of anchor's current instance."""

    anchor: Klass
    up_chain: List[Field]  # parent fields walked from anchor's current instance up to an instance of klass
    sibling_param: OfParam  # klass's own parent field + the sibling-enumeration field (same shape as an OfParam)


@dataclass
class RootScope:
    """Case 4: flat scan of every instance."""


ScopeCase = SelfScope | DescendantScope | AncestorScope | RootScope


@dataclass
class SearchScope:
    klass: Klass
    of_params: List[OfParam]
    default_case: ScopeCase


def _find_down_paths(
    from_klass: Klass, target_name: str, list_only: bool, visited: Optional[frozenset] = None
) -> List[List[Field]]:
    """Every path of is_child fields from from_klass down to target_name (DFS, all paths)."""
    visited = visited or frozenset()
    if from_klass.name in visited:
        return []
    visited = visited | {from_klass.name}
    if from_klass.name == target_name:
        return [[]]

    child_fields = from_klass.tcl_child_list_fields() if list_only else from_klass.tcl_child_fields()
    paths = []
    for child_field in child_fields:
        for sub_path in _find_down_paths(child_field._type_klass, target_name, list_only, visited):
            paths.append([child_field] + sub_path)
    return paths


def _find_up_chain(from_klass: Klass, target_name: str) -> Optional[List[Field]]:
    """Parent fields walked from from_klass up to an instance of target_name, or
    None if unreachable via a single, unambiguous parent chain (e.g. hits a
    class with zero or multiple parent fields before reaching the target)."""
    chain: List[Field] = []
    current = from_klass
    visited = set()
    while current.name != target_name:
        if current.name in visited:
            return None
        visited.add(current.name)
        parent_fields = current.get_parent_fields()
        if len(parent_fields) != 1:
            return None
        chain.append(parent_fields[0])
        current = parent_fields[0]._type_klass
    return chain


def _sibling_field_for(parent_klass: Klass, target_name: str) -> Optional[Field]:
    """The is_child field (list or scalar - e.g. Design.abstract is a
    single field, since a Design has at most one Abstract) on
    parent_klass enumerating/holding target_name instances."""
    matches = [f for f in parent_klass.tcl_child_fields() if f.type == target_name]
    return matches[0] if len(matches) == 1 else None


def _of_params(klass: Klass) -> List[OfParam]:
    params = []
    for pf in klass.get_parent_fields():
        sibling = _sibling_field_for(pf._type_klass, klass.name)
        if sibling is not None:
            params.append(OfParam(parent_field=pf, sibling_field=sibling))
    return params


def compute_default_case(klass: Klass, current_access_classes: List[Klass]) -> ScopeCase:
    if klass.has_current_access:
        return SelfScope(klass=klass)

    for anchor in current_access_classes:
        paths = _find_down_paths(anchor, klass.name, list_only=True)
        if paths:
            return DescendantScope(anchor=anchor, paths=paths)

    for anchor in current_access_classes:
        if not _find_down_paths(klass, anchor.name, list_only=False):
            continue
        up_chain = _find_up_chain(anchor, klass.name)
        if up_chain is None:
            continue
        klass_parent_fields = klass.get_parent_fields()
        if len(klass_parent_fields) != 1:
            continue
        sibling = _sibling_field_for(klass_parent_fields[0]._type_klass, klass.name)
        if sibling is None:
            continue
        return AncestorScope(
            anchor=anchor,
            up_chain=up_chain,
            sibling_param=OfParam(parent_field=klass_parent_fields[0], sibling_field=sibling),
        )

    return RootScope()


def compute_search_scope(klass: Klass, current_access_classes: List[Klass]) -> SearchScope:
    return SearchScope(
        klass=klass,
        of_params=_of_params(klass),
        default_case=compute_default_case(klass, current_access_classes),
    )


# --- C++ rendering ---
#
# The scope-classification functions above only compute *data* (which
# case applies, which fields/paths are involved) - purely mechanical
# graph structure, no per-class knowledge baked in. These functions turn
# that data into the actual C++ `candidates` computation, still entirely
# generically (a single recursive nested-loop builder handles every
# descent path of any length/branching, not a per-class template) - the
# le_get_X template just drops the resulting string in.


def _render_descent_path(path: List[Field], source_expr: str) -> str:
    """
    One is_child-list-field path from an anchor's current instance down
    to the target class, as a self-contained C++ block that appends into
    `candidates` (already declared by the caller). `source_expr` is the
    C++ expression for the anchor's own current id
    (`handle->current_X_id`). Nested for-loops for a multi-hop path (e.g.
    TerminalPort under Abstract: 2 hops) - each hop's owning klass/field
    name comes straight from the Field objects themselves
    (`field._klass`/`field.name`/`field.type`), so this is one generic
    algorithm over the path list, not per-class code.
    """
    lines = ["{"]
    indent = "    "
    cursor = source_expr
    for i, hop in enumerate(path):
        owner_snake = hop._klass.to_snake_case()
        accessor = f"handle->root.get_{owner_snake}_{hop.name}({cursor})"
        is_last = i == len(path) - 1
        if is_last:
            lines.append(f"{indent}const auto &items = {accessor};")
            lines.append(f"{indent}candidates.insert(candidates.end(), items.begin(), items.end());")
        else:
            loop_var = f"hop{i}"
            lines.append(f"{indent}for (const le::{hop.type}Id {loop_var} : {accessor})")
            lines.append(f"{indent}{{")
            indent += "    "
            cursor = loop_var
    for i in range(len(path) - 1):
        indent = indent[:-4]
        lines.append(f"{indent}}}")
    lines.append("}")
    return "\n".join(lines)


def render_default_scope_cpp(scope: SearchScope) -> str:
    """The C++ block computing `candidates` (already-declared
    `std::vector<le::{Klass}Id>`) when every `-of` parameter was omitted
    or invalid - dispatches on the 4 cases from this module's own
    docstring."""
    y = scope.klass
    y_snake = y.to_snake_case()
    case = scope.default_case

    if isinstance(case, SelfScope):
        return (
            f"if (handle->root.get_{y_snake}(handle->current_{y_snake}_id))\n"
            f"    candidates.push_back(handle->current_{y_snake}_id);"
        )

    if isinstance(case, DescendantScope):
        anchor_snake = case.anchor.to_snake_case()
        source = f"handle->current_{anchor_snake}_id"
        blocks = [_render_descent_path(path, source) for path in case.paths]
        return "\n".join(blocks)

    if isinstance(case, AncestorScope):
        anchor = case.anchor
        anchor_snake = anchor.to_snake_case()
        sibling = case.sibling_param
        parent_snake = sibling.parent_klass.to_snake_case()

        lines = [f"const le::{anchor.name}Data *anchor_data = handle->root.get_{anchor_snake}(handle->current_{anchor_snake}_id);"]
        lines.append("if (anchor_data)")
        lines.append("{")
        cursor = "anchor_data"
        # Walk up_chain: each hop dereferences the previous data struct's
        # own parent field, re-fetching that instance's data to keep
        # walking (and to null-check at every step).
        indent = "    "
        for idx, hop in enumerate(case.up_chain):
            id_var = f"up{idx}_id"
            data_var = f"up{idx}_data"
            lines.append(f"{indent}const le::{hop.type}Id {id_var} = {cursor}->{hop.name};")
            lines.append(f"{indent}const le::{hop.type}Data *{data_var} = handle->root.get_{hop._type_klass.to_snake_case()}({id_var});")
            lines.append(f"{indent}if ({data_var})")
            lines.append(f"{indent}{{")
            indent += "    "
            cursor = data_var
        lines.append(f"{indent}const auto &items = handle->root.get_{parent_snake}_{sibling.sibling_field.name}({cursor}->{sibling.parent_field.name});")
        lines.append(f"{indent}candidates.insert(candidates.end(), items.begin(), items.end());")
        for _ in case.up_chain:
            indent = indent[:-4]
            lines.append(f"{indent}}}")
            lines.append(f"{indent}else")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    candidates = handle->root.get_{y_snake}_ids();")
            lines.append(f"{indent}}}")
        lines.append("}")
        lines.append("else")
        lines.append("{")
        lines.append(f"    candidates = handle->root.get_{y_snake}_ids();")
        lines.append("}")
        return "\n".join(lines)

    # RootScope
    return f"candidates = handle->root.get_{y_snake}_ids();"


def render_of_check_cpp(scope: SearchScope) -> str:
    """The C++ block that checks every `-of` parameter (0, 1, or more -
    e.g. Shape has two), unioning whichever are valid, then falls back to
    render_default_scope_cpp() if none were given. Declares and fills
    `candidates` (a `std::vector<le::{Klass}Id>`)."""
    y = scope.klass
    lines = [f"std::vector<le::{y.name}Id> candidates;"]
    if scope.of_params:
        lines.append("bool of_given = false;")
        for op in scope.of_params:
            parent_snake = op.parent_klass.to_snake_case()
            lines.append("{")
            lines.append(f"    const le::{op.parent_klass.name}Id of_id = from_c(of_{op.parent_field.name});")
            lines.append(f"    if (handle->root.get_{parent_snake}(of_id))")
            lines.append("    {")
            lines.append("        of_given = true;")
            if op.sibling_field.is_list:
                lines.append(f"        const auto &items = handle->root.get_{parent_snake}_{op.sibling_field.name}(of_id);")
                lines.append("        candidates.insert(candidates.end(), items.begin(), items.end());")
            else:
                lines.append(f"        const le::{y.name}Id item = handle->root.get_{parent_snake}_{op.sibling_field.name}(of_id);")
                lines.append(f"        if (handle->root.get_{y.to_snake_case()}(item))")
                lines.append("            candidates.push_back(item);")
            lines.append("    }")
            lines.append("}")
        lines.append("if (!of_given)")
        lines.append("{")
        for line in render_default_scope_cpp(scope).splitlines():
            lines.append(f"    {line}")
        lines.append("}")
    else:
        lines.append(render_default_scope_cpp(scope))
    return "\n".join(lines)
