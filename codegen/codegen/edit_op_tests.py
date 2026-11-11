"""
The generated le::edit tests (edit_ops_generated_test.cpp), built from each
field's `example`: per class, one test creating, deleting, undoing and
redoing an object, and one per field update_<type> changes, setting it,
undoing and redoing. A class's make_<type>() builds the object and
everything it needs: its first parent or owner, its required references
and its required fields.

Every create field needs an example (example_errors): a scalar's value, an
enum value's name, a dict of an embedded struct's fields (a field left out
takes that field's own example), or one element of a list. A required
field is created with its example, so its update test sets a derived
second value (_other).
"""

import json
from typing import Any, List

from codegen.schema import Field, Klass

_ZERO = {"str": "", "bool": False}


def _enum_names(klass: Klass) -> List[str]:
    return [f.name for f in klass.fields]


def _value_errors(where: str, type_name: str, type_klass, value: Any) -> List[str]:
    """Why `value` can't be an example of this type, if it can't."""
    if type_klass is None:
        return []
    if type_klass.is_enum:
        if value not in _enum_names(type_klass):
            return [f"{where}: example {value!r} is not a {type_klass.name} value ({', '.join(_enum_names(type_klass))})"]
        return []
    if not isinstance(value, dict):
        return [f"{where}: a {type_name} example is a dict of its fields, got {value!r}"]
    errors = []
    names = {f.name for f in type_klass.fields}
    for key in value:
        if key not in names:
            errors.append(f"{where}: {type_name} has no field {key!r}")
    for sub in type_klass.fields:
        if sub.name in value:
            items = value[sub.name] if sub.is_list and isinstance(value[sub.name], list) else [value[sub.name]]
            for item in items:
                errors += _value_errors(f"{where}.{sub.name}", sub.type, sub._type_klass, item)
        elif sub.example is None and _derived(sub) is None:
            errors.append(f"{where}: {type_name}.{sub.name} has no example, so the example must give it")
    return errors


def _derived(field: Field) -> Any:
    """The example an embedded field without one of its own falls back to."""
    if field.example is not None:
        return field.example
    tk = field._type_klass
    if tk is None or tk.is_enum or tk.has_pool:
        return None
    parts = {}
    for sub in tk.fields:
        value = _derived(sub)
        if value is None:
            return None
        parts[sub.name] = value
    return parts


def _unset_value(field: Field) -> Any:
    """What a non-optional field without a default holds when left out: its value-initialized C++ value."""
    tk = field._type_klass
    if tk is None:
        return _ZERO.get(field.type, 0)
    if tk.is_enum:
        return next((f.name for f in tk.fields if f.value == 0), None)
    return None


def changeable_fields(klass: Klass) -> List[Field]:
    """The fields with literal values update_<type> changes, each needing an example."""
    return klass.get_create_fields()


def example_errors(classes: List[Klass]) -> List[str]:
    """
    Every create field without a usable example: none, one that isn't a
    value of its type, or one equal to the value the field has when it's
    left out (an update to it would change nothing).
    """
    errors = []
    for klass in classes:
        for field in changeable_fields(klass):
            where = f"{klass.name}.{field.name}"
            if field.example is None:
                errors.append(f"{where} has no example")
                continue
            errors += _value_errors(where, field.type, field._type_klass, field.example)
            if field.default is not None and field.example == field.default:
                errors.append(f"{where}: example {field.example!r} is its default")
            elif not field.is_optional and field.default is None and field.example == _unset_value(field):
                errors.append(f"{where}: example {field.example!r} is the value it has when left out")
    return errors


def _scalar_cpp(type_name: str, value: Any) -> str:
    if type_name == "str":
        return f"std::string({json.dumps(value)})"
    if isinstance(value, bool):
        return "true" if value else "false"
    if type_name == "double":
        return repr(float(value))
    return str(value)


def _cpp(type_name: str, type_klass, value: Any) -> str:
    """A C++ expression for one value of this type."""
    if type_klass is None:
        return _scalar_cpp(type_name, value)
    if type_klass.is_enum:
        return f"le::{type_klass.name}::{value}"
    parts = []
    for sub in type_klass.fields:
        sub_value = value.get(sub.name, _derived(sub))
        if sub.is_list:
            items = sub_value if isinstance(sub_value, list) else [sub_value]
            parts.append(f".{sub.name} = {{{', '.join(_cpp(sub.type, sub._type_klass, i) for i in items)}}}")
        else:
            parts.append(f".{sub.name} = {_cpp(sub.type, sub._type_klass, sub_value)}")
    return f"le::{type_klass.name}{{{', '.join(parts)}}}"


def _other(type_name: str, type_klass, value: Any) -> Any:
    """A value of this type different from `value`."""
    if type_klass is None:
        if type_name == "str":
            return value + "_2"
        if isinstance(value, bool):
            return not value
        if isinstance(value, float):
            return value + 1.5
        return value + 1
    if type_klass.is_enum:
        names = _enum_names(type_klass)
        return names[(names.index(value) + 1) % len(names)]
    result = {}
    for sub in type_klass.fields:
        sub_value = value.get(sub.name, _derived(sub))
        if sub.is_list:
            items = sub_value if isinstance(sub_value, list) else [sub_value]
            result[sub.name] = [_other(sub.type, sub._type_klass, i) for i in items]
        else:
            result[sub.name] = _other(sub.type, sub._type_klass, sub_value)
    return result


def _field_cpp(field: Field, value: Any) -> str:
    """A C++ expression of the field's update value type holding `value`."""
    cpp_type = field.root_value_cpp_type(qualified=True)
    if field.is_list:
        return f"{cpp_type}{{{_cpp(field.type, field._type_klass, value)}}}"
    if field._type_klass is None and field.type != "str":
        return f"static_cast<{cpp_type}>({_scalar_cpp(field.type, value)})"
    return _cpp(field.type, field._type_klass, value)


def _snake(klass: Klass) -> str:
    return klass.to_snake_case()


def _maker(klass: Klass) -> List[str]:
    snake = _snake(klass)
    lines = [f"le::{klass.name}Id make_{snake}(LeHandle &handle)", "{", f"    le::{klass.name}Data data{{}};"]
    parents = klass.non_owner_parent_fields()
    owners = klass.get_owner_fields()
    if parents:
        pf = parents[0]
        lines.append(f"    data.{pf.name} = make_{_snake(pf._parent_klass)}(handle);")
    elif owners:
        of = owners[0]
        lines.append(f"    data.owner = le::{klass.owner_type_name()}::{of.name}(make_{_snake(of._parent_klass)}(handle));")
    for rf in klass.get_reference_create_fields():
        if rf.create_required():
            lines.append(f"    data.{rf.name} = make_{_snake(rf._type_klass)}(handle);")
    global_unique = {f.name for f in klass.get_global_unique_fields()}
    for f in changeable_fields(klass):
        if not f.create_required():
            continue
        value = _field_cpp(f, f.example)
        if f.name in global_unique:
            value = f"{value} + std::to_string(next_serial())"
        lines.append(f"    data.{f.name} = {value};")
    lines += [f'    return created(le::edit::create_{snake}(handle, std::move(data)), "create_{snake}");', "}", ""]
    return lines


def _lifecycle_test(klass: Klass) -> List[str]:
    snake = _snake(klass)
    return [
        f"TEST(GeneratedEditOps, {klass.name}_CreateDeleteUndoRedo)",
        "{",
        "    LeHandle handle;",
        "    le::Root &root = handle.root;",
        '    handle.command_history.begin("create");',
        f"    const le::{klass.name}Id id = make_{snake}(handle);",
        "    handle.command_history.end(true);",
        f"    ASSERT_NE(root.get_{snake}(id), nullptr);",
        "    handle.command_history.undo();",
        f"    EXPECT_EQ(root.get_{snake}(id), nullptr);",
        "    handle.command_history.redo();",
        f"    ASSERT_NE(root.get_{snake}(id), nullptr);",
        "",
        '    handle.command_history.begin("delete");',
        f"    const auto deleted = le::edit::delete_{snake}(handle, id);",
        "    handle.command_history.end(deleted.has_value());",
        '    ASSERT_TRUE(deleted.has_value()) << deleted.error();',
        f"    EXPECT_EQ(root.get_{snake}(id), nullptr);",
        "    handle.command_history.undo();",
        f"    EXPECT_NE(root.get_{snake}(id), nullptr);",
        "    handle.command_history.redo();",
        f"    EXPECT_EQ(root.get_{snake}(id), nullptr);",
        "}",
        "",
    ]


def _update_test(klass: Klass, name: str, value_cpp: str) -> List[str]:
    snake = _snake(klass)
    return [
        f"TEST(GeneratedEditOps, {klass.name}_Update_{name})",
        "{",
        "    LeHandle handle;",
        "    le::Root &root = handle.root;",
        f"    const le::{klass.name}Id id = make_{snake}(handle);",
        f"    ASSERT_NE(root.get_{snake}(id), nullptr);",
        f"    const le::{klass.name}Data before = *root.get_{snake}(id);",
        f"    const auto value = {value_cpp};",
        "",
        '    handle.command_history.begin("update");',
        f"    const auto updated = le::edit::update_{snake}(handle, id, le::{klass.name}Changes{{.{name} = value}});",
        "    handle.command_history.end(updated.has_value());",
        '    ASSERT_TRUE(updated.has_value()) << updated.error();',
        f"    EXPECT_EQ(root.get_{snake}(id)->{name}, value);",
        "    handle.command_history.undo();",
        f"    EXPECT_EQ(root.get_{snake}(id)->{name}, before.{name});",
        "    handle.command_history.redo();",
        f"    EXPECT_EQ(root.get_{snake}(id)->{name}, value);",
        "}",
        "",
    ]


def _needs(klass: Klass) -> List[Klass]:
    """The classes make_<type>() for `klass` and its update tests create."""
    needed = [f._parent_klass for f in klass.get_parent_fields()]
    needed += [f._type_klass for f in klass.get_reference_create_fields()]
    return needed


def _closure(classes: List[Klass]) -> List[Klass]:
    """`classes` plus everything their tests create, in schema order."""
    seen = {k.name: k for k in classes}
    pending = list(classes)
    while pending:
        for k in _needs(pending.pop()):
            if k.name not in seen:
                seen[k.name] = k
                pending.append(k)
    return list(seen.values())


def test_files(classes: List[Klass]) -> dict:
    """File name -> source: core classes' tests in edit_ops_generated_test.cpp,
    each extension's in edit_ops_generated_<extension>_test.cpp, which that
    extension's <name>_generated_tests builds."""
    groups: dict = {}
    for klass in classes:
        groups.setdefault(klass.extension, []).append(klass)
    order = {k.name: i for i, k in enumerate(classes)}
    files = {}
    for extension, tested in groups.items():
        name = "edit_ops_generated_test.cpp" if extension is None else f"edit_ops_generated_{extension}_test.cpp"
        made = sorted(_closure(tested), key=lambda k: order[k.name])
        files[name] = generate_test_source(tested, made)
    return files


def generate_test_source(classes: List[Klass], made: List[Klass] = None) -> str:
    """One test file: `classes`' tests, with a make_<type>() for each of `made`
    (default: `classes`), which must include everything they create."""
    made = classes if made is None else made
    lines = [
        "// GENERATED - do not edit by hand. The build regenerates it",
        "// (codegen --target tcl, from each field's example in schema.py).",
        '#include "api/edit_ops.hpp"',
        '#include "api/le_handle.hpp"',
        "#include <gtest/gtest.h>",
        "",
        "#include <expected>",
        "#include <string>",
        "",
        "namespace",
        "{",
        "    int next_serial()",
        "    {",
        "        static int serial = 0;",
        "        return ++serial;",
        "    }",
        "",
        "    template <typename Id>",
        "    Id created(std::expected<Id, std::string> result, const char *op)",
        "    {",
        "        if (!result)",
        '            ADD_FAILURE() << op << ": " << result.error();',
        "        return result.value_or(Id{});",
        "    }",
        "",
    ]
    for klass in made:
        lines.append(f"    le::{klass.name}Id make_{_snake(klass)}(LeHandle &handle);")
    lines.append("")
    for klass in made:
        lines += ["    " + line if line else "" for line in _maker(klass)]
    lines += ["}", ""]

    for klass in classes:
        snake = _snake(klass)
        lines += _lifecycle_test(klass)
        parents = klass.get_parent_fields()
        if len(parents) == 1:
            pf = parents[0]
            lines += _update_test(klass, pf.name, f"make_{_snake(pf._parent_klass)}(handle)")
        for rf in klass.get_reference_create_fields():
            lines += _update_test(klass, rf.name, f"make_{_snake(rf._type_klass)}(handle)")
        for f in changeable_fields(klass):
            value = _other(f.type, f._type_klass, f.example) if f.create_required() else f.example
            lines += _update_test(klass, f.name, _field_cpp(f, value))
    return "\n".join(lines) + "\n"
