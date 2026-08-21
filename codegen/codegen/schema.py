"""A schema used to define the structure of the data in a model."""

from dataclasses import dataclass, field
import os
from typing import Any, List, Optional, Tuple

"""
Define the type map for C++ types: https://en.cppreference.com/w/cpp/language/types
The key is the type in the schema, the value is a tuple with the C++ type and the Python type
"""
TYPEMAP = {
    "str": ("std::string", str),
    "signed char*": ("std::string", str),
    "unsigned char*": ("std::string", str),
    "char*": ("std::string", str),
    "signed char": ("signed char", int),
    "unsigned char": ("unsigned char", int),
    "char": ("char", int),
    "short": ("short", int),
    "short int": ("short int", int),
    "signed short": ("signed short", int),
    "signed short int": ("signed short int", int),
    "unsigned short": ("unsigned short", int),
    "unsigned short int": ("unsigned short int", int),
    "int": ("int", int),
    "signed": ("signed", int),
    "signed int": ("signed int", int),
    "unsigned": ("unsigned", int),
    "unsigned int": ("unsigned int", int),
    "long": ("long", int),
    "long int": ("long int", int),
    "signed long": ("signed long", int),
    "signed long int": ("signed long int", int),
    "unsigned long": ("unsigned long", int),
    "unsigned long int": ("unsigned long int", int),
    "long long": ("long long", int),
    "long long int": ("long long int", int),
    "signed long long": ("signed long long", int),
    "signed long long int": ("signed long long int", int),
    "unsigned long long": ("unsigned long long", int),
    "unsigned long long int": ("unsigned long long int", int),
    "float": ("float", float),
    "double": ("double", float),
    "long double": ("long double", float),
    "bool": ("bool", bool),
    # A raw value in database units (this project's LEF/DEF integer
    # coordinate/length convention) - a plain int64_t in the generated
    # struct, but a distinct schema-level type so property-formatting code
    # can tell "this needs micron conversion for display" from a plain
    # count/int without guessing from the C++ type name.
    "dbu": ("int64_t", int),
}


def _leaf_c_type(leaf: "Field") -> str:
    """
    The TCL/API-layer C type of one flattened scalar leaf of a compound
    (embedded-struct) create field - see Klass.embedded_scalar_leaves().
    Leaves are always dbu/int/double/bool (embedded_scalar_leaves()
    already rejects str/enum/reference leaves), so this is a narrow
    subset of Field.create_c_type()'s own mapping, not a duplicate of it.
    """
    if leaf.type == "dbu":
        return "double"
    if leaf.type in ("int", "bool"):
        return "int32_t"
    if leaf.type == "double":
        return "double"
    raise ValueError(f"_leaf_c_type: unsupported compound-leaf type {leaf.type!r} on field {leaf.name!r}")


def _leaf_param_name(field_name: str, flat: str, leaf: "Field") -> str:
    """
    The C-level parameter name for one flattened scalar leaf of a compound
    create field, e.g. Abstract.bbox's ur_y leaf -> "bbox_ur_y_um" (dbu
    leaves get the same _um suffix Field.create_c_param_name() already
    uses for a plain dbu field - matches the same microns-at-the-C-
    boundary convention every dbu-crossing parameter in this generated
    surface follows, e.g. le_create_shape's own rects_flat_um).
    """
    return f"{field_name}_{flat}_um" if leaf.type == "dbu" else f"{field_name}_{flat}"


def _leaf_value_expr(leaf: "Field", param: str) -> str:
    """
    The api.cpp-layer expression converting one flattened scalar leaf's
    raw C-API parameter into its final typed C++ value - to_dbu(...) for
    a dbu leaf (assumes a local `dbu_per_um` already exists, same
    assumption Field.create_struct_init_expr()'s own dbu branch makes),
    a != 0 bool coercion, or identity.
    """
    if leaf.type == "dbu":
        return f"to_dbu({param}, *dbu_per_um)"
    if leaf.type == "bool":
        return f"{param} != 0"
    return param


def to_snake_case(name: str) -> str:
    """
    Convert a camelCase or PascalCase string to snake_case.

    Args:
        name (str): The name to convert.

    Returns:
        str: The name in snake_case.
    """
    name = "".join(["_" + c.lower() if c.isupper() else c for c in name]).lstrip("_")
    return name


def tcl_brace_escape(text: str) -> str:
    """
    Escapes free text for safe embedding inside a generated Tcl
    {...}-quoted literal (used by the help-system registration calls in
    le_tcl_procs_generated_tcl_j2.py - usage/description/option text
    pulled from schema.py's own Klass.description/Field.description).
    Backslash-escapes a literal backslash/`{`/`}` so an unbalanced brace
    in free-text description text can never break the generated file's
    own brace nesting. Tcl's {...} quoting leaves the escaping backslash
    itself in the resulting string value for `\\{`/`\\}` (unlike "..."
    quoting, which would strip it) - a minor cosmetic wart for the rare
    description that actually contains a literal brace, not a parse
    hazard, which is the property that actually matters here. No schema.py
    description contains a literal brace today (checked directly), so
    this is defensive, not a fix for a live problem.
    """
    return text.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}")


def tcl_dquote_escape(text: str) -> str:
    """
    Escapes free text for safe embedding inside a generated Tcl
    "..."-quoted string - get_<type>'s own `-help` return line and every
    family's `register_command_help` usage/description argument
    (le_tcl_procs_generated_tcl_j2.py). Unlike {...} quoting (see
    tcl_brace_escape), Tcl's "..." quoting actively performs backslash/
    variable/command substitution, so a literal backslash, `"`, `$`, or
    `[`/`]` in free text has to be escaped up front or Tcl parses part of
    the description as code - not hypothetical: EnclosureEntry's own
    description contains "WIDTH[+EXCEPTEXTRACUT]", which without this
    was read as a command substitution (invoking a nonexistent
    "+EXCEPTEXTRACUT" command) the moment le_tcl_procs.tcl was sourced,
    found via a real ctest failure.
    """
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("$", "\\$")
        .replace("[", "\\[")
        .replace("]", "\\]")
    )


def to_camel_case(name: str | None, upper_first=False) -> str | None:
    """
    Convert a snake_case string to camelCase.

    Args:
        name (str): The name to convert.
        upper_first (bool): Whether to capitalize the first letter.

    Returns:
        str: The name in camelCase.
    """

    if name is None:
        return None
    name = name.replace("_", " ").title().replace(" ", "")
    if upper_first:
        return name
    return name[0].lower() + name[1:]


@dataclass
class Schema:
    """
    A schema used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the schema.

        description (str): The description of the schema.

        namespace (str): The namespace of the schema.

        classes (List[Klass]): The classes in the schema.
    """

    name: str
    description: str
    namespace: str
    version: str
    classes: List["Klass"] = field(default_factory=list)
    _output_dir: str = field(default=".", repr=False, init=False)

    def set_output_dir(self, output_dir: str) -> None:
        """
        Set the output directory for the schema.

        Args:
            output_dir (str): The output directory.
        """
        self._output_dir = os.path.abspath(output_dir)

    def link(self) -> None:
        """
        Link the schema to its classes and fields.
        """
        for klass in self.classes:
            klass._schema = self
            klass.link()

    def get_klass(self, name: str) -> "Klass":
        """
        Get a klass by name.

        Args:
            name (str): The name of the klass.

        Returns:
            Klass: The klass.

        Raises:
            ValueError: If the klass is not found.
        """
        for klass in self.classes:
            if klass.name == name:
                return klass
        raise ValueError(f"Klass not found: {name}")

    def get_field(self, klass_name: str, field_name: str) -> "Field":
        """
        Get a field by klass and field name.

        Args:
            klass_name (str): The name of the klass.
            field_name (str): The name of the field.

        Returns:
            Field: The field.

        Raises:
            ValueError: If the field is not found.
        """
        klass = self.get_klass(klass_name)
        for field in klass.fields:
            if field.name == field_name:
                return field
        raise ValueError(f"Field not found: {field_name} in klass: {klass_name}")

    def get_cmakelists_src(self) -> str:
        """
        Get the source files for the CMakeLists.txt file.

        Returns:
            str: The source files.
        """
        return " ".join([f'"{klass.to_snake_case()}.cpp"' for klass in self.classes])

    def get_lcov_src(self) -> str:
        """
        Get the source files for the lcov command.

        Returns:
            str: The source files.
        """
        sources = [
            "/".join(
                [
                    self._output_dir.replace(os.path.sep, "/").replace("c:", "", 1),
                    f"{klass.to_snake_case()}.cpp",
                ]
            )
            for klass in self.classes
        ]

        sources.extend(
            [
                "/".join(
                    [
                        self._output_dir.replace(os.path.sep, "/").replace("c:", "", 1),
                        f"{klass}.cpp",
                    ]
                )
                for klass in self.get_supplementary_klasses()
            ]
        )
        return " ".join(sources)

    def get_test_includes(self) -> List[str]:
        """
        Get the include statements for the test file.

        Returns:
            List[str]: A list of include statements for each class in the test file.
        """
        return [f'#include "{klass.to_snake_case()}.hpp"' for klass in self.classes]

    def get_supplementary_klasses(self):
        """
        Get the supplementary sources for the schema.

        Returns:
            List[str]: The supplementary sources.
        """
        return ["identifiable", "index"]

    def get_root_klass(self) -> "Klass":
        """
        Get the root klass for the schema.

        Returns:
            Klass: The root klass.

        Raises:
            ValueError: If no root klass is found.
        """
        for klass in self.classes:
            if not klass.has_parent():
                return klass
        raise ValueError("No root klass found")

    def get_pool_classes(self) -> List["Klass"]:
        """
        Return a list of klasses that need pools.
        """
        return [klass for klass in self.classes if klass.has_pool and not klass.is_enum]

    def get_non_pool_classes(self) -> List["Klass"]:
        """
        Return a list of klasses that do not need pools.
        """
        return [
            klass for klass in self.classes if not klass.has_pool and not klass.is_enum
        ]

    def get_classes_without_enums(self) -> list["Klass"]:
        return [klass for klass in self.classes if not klass.is_enum]

    def get_enums(self) -> list["Klass"]:
        return [klass for klass in self.classes if klass.is_enum]


@dataclass
class Klass:
    """
    A class used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the class.

        description (str): The description of the class.

        fields (List[Field]): The fields in the class.

        has_pool (bool): Store this kclass in a pool. Defaults to true.

    """

    name: str
    description: str
    fields: List["Field"] = field(default_factory=list)
    has_pool: bool = True
    is_enum: bool = False
    enum_values: List["EnumValue"] = field(default_factory=list)

    # TCL codegen (backend/src/tcl) - not database codegen. `None` means
    # "default to has_pool": every pool-backed class is TCL-readable
    # (property tables + friendly-id resolution) unless explicitly opted
    # out with tcl_readable=False. See backend/CLAUDE.md's TCL section.
    tcl_readable: Optional[bool] = None

    # Explicit override for the field backing this class's TCL friendly
    # id (e.g. "type:NAME" instead of "type:N"). Auto-derives to the
    # field with index=True if one exists (see tcl_friendly_id_field()) -
    # only needed when a class's real uniqueness isn't captured by
    # index=True (e.g. Terminal, whose name uniqueness is enforced
    # per-Abstract by hand, not a global cmg index).
    tcl_id_field: Optional[str] = None

    # Whether this class has a generated "current instance" concept
    # (a settable Le{Klass}Id on LeHandle, plus generated current_X/
    # set_current_X_cmd TCL commands) - independent of any hand-written
    # "current view" state elsewhere (e.g. Scene::current_abstract(),
    # which drives GUI rendering and is deliberately not bridged to this).
    # Every readable class's generated get_<type> search command derives
    # its default (-of omitted) scope from whichever has_current_access
    # class governs it, purely from schema parent/is_child structure - see
    # codegen/codegen/tcl_scope.py's own module docstring for the full
    # algorithm. Set on the classes that actually have an "in-progress
    # editing view" concept (Abstract, Schematic) plus any natural
    # session-singleton (Technology) - not every class needs this, only
    # ones other classes' defaults should anchor to.
    has_current_access: bool = False

    _schema: Optional[Schema] = field(default=None, repr=False, init=False)
    # Memoizes embedded_scalar_leaves() - False (not None) means "computed,
    # and this Klass is not flattenable" (None means "not computed yet"),
    # since None is also embedded_scalar_leaves()'s own "not flattenable"
    # return value and can't double as the unpopulated-cache sentinel too.
    _leaves_cache: Any = field(default=None, repr=False, init=False, compare=False)
    _leaves_cache_set: bool = field(default=False, repr=False, init=False, compare=False)

    def is_tcl_readable(self) -> bool:
        """
        Whether this class gets a generated TCL property-reading surface
        (le_X_property_count/_at/_path, friendly-id resolution). Defaults
        to has_pool - a non-pooled (embedded value) class has no
        independent identity to hand out a friendly id for.
        """
        return self.tcl_readable if self.tcl_readable is not None else self.has_pool

    def tcl_friendly_id_field(self) -> Optional["Field"]:
        """
        The field backing this class's TCL friendly id ("type:<value>"),
        or None if this class uses a numeric ("type:<packed index>")
        friendly id instead. tcl_id_field wins if set; otherwise the
        first field with index=True (Root already generates
        get_<klass>_by_<field>() for it - see root_hpp_j2.py - so name-
        based resolution needs no new database-side lookup).
        """
        if self.tcl_id_field is not None:
            for f in self.fields:
                if f.name == self.tcl_id_field:
                    return f
            raise ValueError(
                f"Klass {self.name}: tcl_id_field={self.tcl_id_field!r} does not name a real field"
            )
        for f in self.fields:
            if f.index:
                return f
        return None

    def tcl_indexed_id_field(self) -> Optional["Field"]:
        """
        Like tcl_friendly_id_field(), but only returns a field actually
        backed by a real *global* Root::get_<klass>_by_<field>(value)
        lookup (index=True and not unique_per_parent - a unique_per_parent
        field is also index=True, but its generated Root lookup takes an
        extra parent-id parameter and only enforces/finds uniqueness
        within one parent's sibling group, e.g. Terminal's `name`, unique
        only per-Abstract - see Field.unique_per_parent's own docstring).
        Such a field is still the right one for glob-based
        name_expression search (tcl_friendly_id_field() covers that), but
        there's no *global* Root-level by-name lookup to generate a
        friendly-id resolve/format pair or a le_X_by_<field>/le_X_<field>
        accessor pair from - those stay hand-written for a
        unique_per_parent class (e.g. Terminal's own resolve_terminal_id),
        so this returns None for it instead.
        """
        f = self.tcl_friendly_id_field()
        return f if f is not None and f.index and not f.unique_per_parent else None

    def tcl_child_list_fields(self) -> List["Field"]:
        """
        This class's own is_child list fields (e.g. Technology.layers) -
        the ones the TCL generator emits a friendly-id enumeration
        pair (le_X_<field>_count/_at) and a derived "<field>_count"
        property row for, since to_properties() itself never includes
        is_child fields (they're structural, not struct fields at all).
        """
        return [f for f in self.fields if f.is_child and f.is_list]

    def tcl_plural_snake_case(self) -> str:
        """
        snake_case, pluralized - matches the existing hand-written search
        naming convention (le_get_terminals/get_terminals_cmd/
        get_terminals_at/the "get_terminals" Tcl proc itself are all
        plural; le_search_result_terminal_at and the
        terminal_search_results cache member stay singular - see
        to_snake_case() for those). Simple English pluralization (+s, or
        -y -> -ies after a consonant) - covers every class name in this
        schema; extend here if a future class name needs an irregular
        plural cmg/codegen doesn't already know how to form.
        """
        snake = self.to_snake_case()
        if len(snake) >= 2 and snake[-1] == "y" and snake[-2] not in "aeiou":
            return snake[:-1] + "ies"
        return snake + "s"

    def tcl_child_fields(self) -> List["Field"]:
        """
        Every one of this class's own is_child fields, list or scalar
        (e.g. both Technology.layers - a list - and Design.abstract - a
        single field, since a Design has at most one Abstract). Used by
        codegen/codegen/tcl_scope.py's default-scope graph walk, which
        needs to traverse scalar is_child edges too (Design->Abstract,
        Design->Schematic) - tcl_child_list_fields() alone (list-only)
        would miss them.
        """
        return [f for f in self.fields if f.is_child]

    def get_parent_fields(self) -> List["Field"]:
        """
        Every field on this class that names a parent (has_parent()) -
        usually exactly one, but a class can have more than one distinct
        parent type (e.g. Shape.terminal_port/Shape.obstruction - a Shape
        belongs to exactly one of the two, never both, per schema.py's own
        comment on Shape). Used by tcl_scope.py to walk "up" from a
        has_current_access class's current instance toward an ancestor
        class whose own default get_<type> scope it governs.
        """
        return [f for f in self.fields if f.has_parent()]

    def embedded_scalar_leaves(self) -> Optional[List[Tuple[str, "Field"]]]:
        """
        Every recursively-reached scalar leaf of this embedded
        (has_pool=False, non-enum) Klass, as (flat_name, leaf_field) pairs
        in declaration order - Point -> [("x", x), ("y", y)]; Rect ->
        [("ll_x", Point.x), ("ll_y", Point.y), ("ur_x", ...), ("ur_y",
        ...)]; Symmetry -> [("r90", r90), ("x", x), ("y", y)].

        Returns None (= "not flattenable, out of create_<type>/
        update_<type>'s compound-field scope") if any field, at any depth,
        is is_list/is_optional/is_child/has_parent()/references a pooled
        Klass/is an enum. The only embedded struct reachable singularly
        from a pool-backed Klass that this currently rejects is
        ParallelRunLengthSpacingTable (three dbu *lists* - a genuine
        variable-size table, not a fixed record).
        """
        if self._leaves_cache_set:
            return self._leaves_cache

        def compute() -> Optional[List[Tuple[str, "Field"]]]:
            leaves: List[Tuple[str, "Field"]] = []
            for f in self.fields:
                if f.is_list or f.is_optional or f.is_child or f.has_parent():
                    return None
                tk = f._type_klass
                if tk is not None and tk.is_enum:
                    return None
                if tk is not None:
                    if tk.has_pool:
                        return None
                    sub = tk.embedded_scalar_leaves()
                    if sub is None:
                        return None
                    leaves.extend((f"{f.name}_{flat}", leaf) for flat, leaf in sub)
                    continue
                leaves.append((f.name, f))
            return leaves

        result = compute()
        self._leaves_cache = result
        self._leaves_cache_set = True
        return result

    def compound_leaf_kind(self) -> str:
        """
        "numeric" if every leaf of embedded_scalar_leaves() is
        dbu/int/double (Point/Rect/DensityCheckWindow), "flags" if every
        leaf is bool (Symmetry). Raises if the leaves are mixed - no flag
        spelling is designed for a struct that's part coordinates and
        part switches, and none exists in the schema today.
        """
        leaves = self.embedded_scalar_leaves()
        if not leaves:
            raise ValueError(f"compound_leaf_kind: {self.name} has no flattenable leaves")
        types = {leaf.type for _, leaf in leaves}
        if types <= {"dbu", "int", "double"}:
            return "numeric"
        if types == {"bool"}:
            return "flags"
        raise ValueError(f"compound_leaf_kind: {self.name} has mixed leaf types {types!r} - no flag convention designed for this")

    def is_point_list_wrapper(self) -> bool:
        """
        True for an embedded (has_pool=False, non-enum) Klass shaped like
        Polygon: exactly one field, itself is_list=True, whose own
        element type is flattenable (embedded_scalar_leaves() - e.g.
        Point, 2 scalar leaves). This is the building block
        Field.list_compound_kind()'s "points" and "points_plus_scalars"
        kinds are both defined in terms of - a variable-length list of
        fixed-size records (a polygon/path's own point list), as opposed
        to embedded_scalar_leaves()'s "single fixed-size record"
        (Point/Rect/Symmetry/...).
        """
        if self.has_pool or self.is_enum or len(self.fields) != 1:
            return False
        only = self.fields[0]
        if not only.is_list or only.has_parent() or only.is_child:
            return False
        leaf_tk = only._type_klass
        return leaf_tk is not None and not leaf_tk.is_enum and not leaf_tk.has_pool and leaf_tk.embedded_scalar_leaves() is not None

    def get_create_fields(self) -> List["Field"]:
        """
        Every field that gets its own flag on this class's generated
        create_<type> command (see Field.is_create_field()), in schema
        (declaration) order.
        """
        return [f for f in self.fields if f.is_create_field()]

    def get_unique_per_parent_fields(self) -> List["Field"]:
        """
        Every field on this class with unique_per_parent=True - used by
        create_<type> generation to know whether Root::create_<type>() is
        fallible (a sibling collision under the same parent) and needs an
        error message distinct from the ordinary "unknown parent" case.
        """
        return [f for f in self.fields if f.unique_per_parent]

    def _create_field_param_parts(self, mode: str) -> List[str]:
        """
        "<type> <name>" declaration fragments for every create field's own
        slot(s) (Field.cmd_param_slots(mode)), in order - shared by
        create_api_params/create_shim_params (mode="create") and
        update_api_params/update_shim_params (mode="update"). Parent-field
        parts are each aggregator's own concern (the shape differs too
        much between create's "always every parent, required" and
        update's "at most one, optional" to share here).
        """
        parts = []
        for f in self.get_create_fields():
            for ctype, pname in f.cmd_param_slots(mode):
                parts.append(f"{ctype}{pname}" if ctype.endswith("*") else f"{ctype} {pname}")
        return parts

    def _create_field_forward_parts(self, mode: str) -> List[str]:
        """Forwarding-argument counterpart of _create_field_param_parts()."""
        parts = []
        for f in self.get_create_fields():
            parts.extend(f.cmd_forward_exprs(mode))
        return parts

    def create_api_params(self) -> str:
        """
        Full comma-joined parameter list (excluding the leading
        LeHandle*) for this class's generated le_create_<type> - one
        Le<Parent>Id per parent field (get_parent_fields(), usually one,
        more for a multi-parent class like Shape/ViaLayer/Foreign/
        LayerDensityEntry), then each create field's own slot(s) (plus a
        has_<field> companion for the ones that need one - see
        Field.create_needs_has_flag()).
        """
        parts = [f"Le{pf.type}Id {pf.name}_id" for pf in self.get_parent_fields()]
        parts += self._create_field_param_parts("create")
        return ", ".join(parts)

    def create_shim_params(self) -> str:
        """
        Same shape as create_api_params(), but each parent field is a
        `const char *` friendly-id token instead of its real Le<Parent>Id
        - this is the signature create_<type>_cmd (the SWIG/Tcl-facing
        shim) actually takes, resolving each token via
        resolve_<parent_klass>_id() in its own body before forwarding to
        le_create_<type> (mirrors create_terminal_port_cmd's existing
        `const char *terminal_id` precedent, not the older packed-longlong
        convention create_terminal_cmd/create_obstruction_cmd used before
        Root-level friendly-id resolution existed for every class - see
        the create_<type> generation round's own notes for why those two
        are being replaced, not extended).
        """
        parts = [f"const char *{pf.name}_id" for pf in self.get_parent_fields()]
        parts += self._create_field_param_parts("create")
        return ", ".join(parts)

    def create_shim_forward_args(self) -> str:
        """
        Comma-joined argument list forwarding a create_<type>_cmd call
        (create_shim_params()'s own parameter names) to le_create_<type>
        (create_api_params()) - each parent token resolved via
        resolve_<parent_klass>_id(), each create field forwarded via its
        own Field.cmd_forward_exprs() (identity for numeric/compound-leaf
        slots, empty-to-nullptr for an optional str/enum field).
        """
        parts = [f"resolve_{pf._parent_klass.to_snake_case()}_id({pf.name}_id)" for pf in self.get_parent_fields()]
        parts += self._create_field_forward_parts("create")
        return ", ".join(parts)

    def list_compound_swig_applies(self) -> str:
        """
        `%apply` lines (le_api.i) letting each of this class's own
        list_compound_kind() create fields reuse the existing
        POINTS_ARRAY_UM/POINTS_COORD_COUNT typemap (le_api.i's own
        coordinate-list typemap, hand-written there since it needs real
        Tcl_Interp/Tcl_ListObjGetElements access no generated code
        touches) under its own `<field>_flat_um`/
        `<field>_flat_count` parameter names - SWIG's %apply matches by
        exact parameter name, so a class with more than one such field
        (e.g. Shape's rects/polygons/paths) needs one line per field, and
        every one must appear before any declaration using those names
        (le_api_generated_i_j2.py emits these as a preamble, ahead of
        every create_<type>_cmd/update_<type>_cmd declaration, for
        exactly this reason). Empty string if this class has no
        list_compound_kind() create fields.
        """
        lines = []
        for f in self.get_create_fields():
            if f.list_compound_kind() is not None:
                lines.append(
                    "%apply (const double *POINTS_ARRAY_UM, int32_t POINTS_COORD_COUNT) "
                    f"{{ (const double *{f.list_compound_c_param_name()}, int32_t {f.name}_flat_count) }};"
                )
        return "\n".join(lines)

    def create_api_body(self) -> str:
        """
        The full statement-list body of le_create_<type>(LeHandle *handle,
        <create_api_params()>) - built as one Python string rather than
        deeply nested Jinja {%- if %} chains, since the per-field-type/
        per-optionality branching (required-null checks, enum parsing,
        the exactly-one-parent check for a multi-parent class, the shared
        dbu_per_um lookup) is easier to get right and keep readable as
        real Python control flow than as template logic. Mirrors the
        hand-written le_create_terminal_port/le_create_obstruction shape
        for the common case (one parent, no optional/enum/dbu fields) and
        le_create_terminal's own shape (str/enum fields, is_optional
        handling) for the richer ones - generated instead of duplicated
        per class.
        """
        indent = "        "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        parent_fields = self.get_parent_fields()
        create_fields = self.get_create_fields()
        enum_fields = [f for f in create_fields if f.is_enum_type()]
        dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]
        unique_fields = self.get_unique_per_parent_fields()

        add(f"const Le{self.name}Id invalid{{.index = UINT32_MAX, .generation = 0}};")
        add("if (!handle)")
        add("    return invalid;")
        for f in create_fields:
            if (f.type == "str" or f.is_enum_type()) and f.create_required():
                add(f"if (!{f.create_c_param_name()})")
                add("{")
                add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: {f.name} is required"));')
                add("    return invalid;")
                add("}")
        add("std::lock_guard<std::mutex> lock(handle->mutex_);")

        if parent_fields:
            add()
            for pf in parent_fields:
                add(f"const le::{pf.type}Id {pf.name} = from_c({pf.name}_id);")
            if len(parent_fields) == 1:
                pf = parent_fields[0]
                add(f"if (!handle->root.get_{pf._parent_klass.to_snake_case()}({pf.name}))")
                add("{")
                add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: unknown {pf.name} - no such {pf.type} exists"));')
                add("    return invalid;")
                add("}")
            else:
                add("// Exactly one parent must resolve - a {} belongs to exactly one of these, never zero or several (see its own schema.py comment).".format(self.name))
                add("int32_t provided_parent_count = 0;")
                for pf in parent_fields:
                    add(f"if (handle->root.get_{pf._parent_klass.to_snake_case()}({pf.name}))")
                    add("    ++provided_parent_count;")
                add("if (provided_parent_count != 1)")
                add("{")
                add(
                    f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: exactly one of '
                    f'{"/".join(p.name for p in parent_fields)} must resolve to a valid parent, got {{}}", '
                    f'provided_parent_count));'
                )
                add("    return invalid;")
                add("}")

        if enum_fields:
            add()
            for f in enum_fields:
                enum_snake = f._type_klass.to_snake_case()
                if f.create_required():
                    add(f"const std::optional<le::{f.type}> parsed_{f.name} = le::{enum_snake}_from_string({f.name});")
                    add(f"if (!parsed_{f.name})")
                    add("{")
                    add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: unrecognized {f.name} \'{{}}\'", {f.name}));')
                    add("    return invalid;")
                    add("}")
                else:
                    add(f"const std::optional<le::{f.type}> parsed_{f.name} = ({f.name} && {f.name}[0]) ? le::{enum_snake}_from_string({f.name}) : std::nullopt;")
                    add(f"if ({f.name} && {f.name}[0] && !parsed_{f.name})")
                    add("{")
                    add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: unrecognized {f.name} \'{{}}\'", {f.name}));')
                    add("    return invalid;")
                    add("}")

        if dbu_fields:
            add()
            add("const std::optional<double> dbu_per_um = database_units_microns(handle->root);")
            add("if (!dbu_per_um)")
            add("{")
            add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: no Technology has been read yet (needed for micron-to-dbu conversion)"));')
            add("    return invalid;")
            add("}")

        list_compound_fields = [f for f in create_fields if f.list_compound_kind() is not None]
        for f in list_compound_fields:
            add()
            lines.extend(f.list_compound_parse_lines("create", "return invalid;", f"le_create_{snake}"))

        add()
        add(f"const le::{self.name}Data new_data{{")
        for pf in parent_fields:
            add(f"    .{pf.name} = {pf.name},")
        for f in create_fields:
            add(f"    .{f.name} = {f.create_struct_init_expr()},")
        add("};")
        add(f"const le::{self.name}Id created = handle->root.create_{snake}(new_data);")

        if unique_fields:
            field = unique_fields[0]
            add("if (!created.valid())")
            add("{")
            add(
                f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: a sibling {self.name} with this '
                f"{field.name} (\'{{}}\') already exists\", {field.create_c_param_name()}));"
            )
            add("    return invalid;")
            add("}")

        add("handle->root.bump_mutation_version();")
        add()
        add("// UPDATES.md item 21 - record this mutation into whatever")
        add("// transaction is currently recording (a typed Tcl command via")
        add("// le_repl_eval, or a GUI edit like Move), so Ctrl-Z can undo it.")
        add("if (handle->command_history.is_recording())")
        add("{")
        add(f"    handle->command_history.current()->record_create<le::{self.name}Id, le::{self.name}Data>(")
        add("        created, new_data,")
        add(f"        [](le::Root &r, const le::{self.name}Data &d) {{ return r.create_{snake}(d); }},")
        add(f"        [](le::Root &r, le::{self.name}Id i) {{ return r.delete_{snake}(i); }});")
        add("}")
        add("return to_c(created);")

        return "\n".join(lines)

    def create_tcl_flag_defaults(self) -> str:
        """
        The `array set opts {...}` initializer body (space-joined
        `-flag {}` pairs) for this class's generated `create_<type>
        {args}` proc - one entry per parent field, then per create field,
        in create_shim_params() order.
        """
        parts = [f"-{pf.name} {{}}" for pf in self.get_parent_fields()]
        parts += [f"-{f.name} {{}}" for f in self.get_create_fields()]
        return " ".join(parts)

    def create_tcl_required_flags(self) -> str:
        """
        Space-joined `-flag` names the generated `create_<type> {args}`
        proc rejects as missing before calling create_<type>_cmd - each
        parent flag when there's only one (get_parent_fields() length > 1
        means "exactly one of these", a distinct check - see
        create_api_body()'s own parent-count validation, mirrored here),
        plus every create_required() create field.
        """
        parts = []
        if len(self.get_parent_fields()) == 1:
            parts.append(f"-{self.get_parent_fields()[0].name}")
        parts += [f"-{f.name}" for f in self.get_create_fields() if f.create_required()]
        return " ".join(parts)

    def create_tcl_current_defaults(self) -> str:
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
        """
        parent_fields = self.get_parent_fields()
        lines = []
        for pf in parent_fields:
            if not pf._parent_klass.has_current_access:
                continue
            others = [p for p in parent_fields if p is not pf]
            condition = " && ".join([f"$opts(-{pf.name}) eq {{}}"] + [f"$opts(-{o.name}) eq {{}}" for o in others])
            lines.append(f"    if {{{condition}}} {{")
            lines.append(f"        set opts(-{pf.name}) [current_{pf._parent_klass.to_snake_case()}]")
            lines.append("    }")
        return "\n".join(lines)

    def create_tcl_call_args(self) -> str:
        """
        Space-joined Tcl argument list for calling create_<type>_cmd from
        this class's own create_<type> {args} proc, in create_shim_params()
        order - `$opts(-<parent_field>)` per parent token, then each
        create field's own Field.cmd_tcl_call_args("create").
        """
        parts = [f"$opts(-{pf.name})" for pf in self.get_parent_fields()]
        for f in self.get_create_fields():
            parts.extend(f.cmd_tcl_call_args("create"))
        return " ".join(parts)

    def cmd_tcl_preamble(self, mode: str) -> str:
        """
        Generated Tcl statements, emitted after flag parsing and before
        the _cmd call, that turn every compound or list_compound_kind()-
        eligible create field's own Tcl list flag value into its has-flag
        (+ leaf-parameter locals, or a single flattened list - see
        Field.cmd_tcl_preamble()) - shared verbatim between create_<type>
        and update_<type> generation. Empty string if this class has no
        such create fields.
        """
        cmd_name = f"{mode}_{self.to_snake_case()}"
        lines = [
            f.cmd_tcl_preamble(mode, cmd_name)
            for f in self.get_create_fields()
            if f.compound_klass() is not None or f.list_compound_kind() is not None
        ]
        return "\n".join(line for line in lines if line)

    def update_api_params(self) -> str:
        """
        Full comma-joined parameter list (excluding the leading
        LeHandle*) for this class's generated le_update_<type> -
        Le<Klass>Id id, then (iff this class has exactly one parent
        field) a has_<parent>/Le<Parent>Id pair letting the caller
        reassign it, then every create field's own slot(s) via
        Field.cmd_param_slots("update") - every field gets a has-flag
        (or, for str/enum, relies on the same nullptr-means-omitted
        signal create_<type> already uses) here, since an omitted update
        flag means "leave unchanged", not "unset"/"apply the zero value"
        (see update_api_body()). Multi-parent classes (Shape/ViaLayer/
        Foreign/LayerDensityEntry) get no parent flag at all -
        reassigning one parent field alone would violate their "exactly
        one parent set" invariant, and there's no atomic "swap parent,
        clear siblings" primitive designed (decision 5 - see the plan).
        Generated for every pool class unconditionally, same as
        create_<type> - even a class with zero non-parent create fields
        (Obstruction, Schematic) still has a real, useful reparent-only
        update_<type> (every pool class has at least a parent field or a
        create field, confirmed by inspection - never neither).
        """
        parts = [f"Le{self.name}Id id"]
        parent_fields = self.get_parent_fields()
        if len(parent_fields) == 1:
            pf = parent_fields[0]
            parts.append(f"int32_t has_{pf.name}")
            parts.append(f"Le{pf.type}Id {pf.name}_id")
        parts += self._create_field_param_parts("update")
        return ", ".join(parts)

    def update_shim_params(self) -> str:
        """
        Same shape as update_api_params(), but `id` and (for a single-
        parent class) the parent are `const char *` friendly-id tokens
        instead of real Ids - the signature update_<type>_cmd (the
        SWIG/Tcl-facing shim) actually takes, resolving each token via
        resolve_<klass>_id()/resolve_<parent_klass>_id() in its own body
        before forwarding to le_update_<type> (mirrors
        create_shim_params() vs. create_api_params()'s existing split).
        """
        parts = ["const char *id"]
        parent_fields = self.get_parent_fields()
        if len(parent_fields) == 1:
            pf = parent_fields[0]
            parts.append(f"int32_t has_{pf.name}")
            parts.append(f"const char *{pf.name}_id")
        parts += self._create_field_param_parts("update")
        return ", ".join(parts)

    def update_shim_forward_args(self) -> str:
        """
        Comma-joined argument list forwarding an update_<type>_cmd call's
        own parameters (update_shim_params(), *excluding* `id` - the
        caller resolves that once into its own `typed_id` local, since
        it's needed twice: as le_update_<type>'s own first argument
        *and*, unlike create_<type>_cmd (which formats the id le_create_
        <type> itself returns), to format the *return* value too, from
        the id resolved *before* the update call ran - re-resolving the
        caller's original friendly-id string afterward would break for
        a class whose friendly id can change as a result of the update
        itself, e.g. Terminal's own name-based one after a rename) to
        le_update_<type> (update_api_params(), also id-less from this
        method's point of view) - the parent token (if single-parent)
        forwarded alongside its own has-flag unchanged and resolved via
        resolve_<parent_klass>_id(), each create field forwarded via its
        own Field.cmd_forward_exprs("update").
        """
        parts = []
        parent_fields = self.get_parent_fields()
        if len(parent_fields) == 1:
            pf = parent_fields[0]
            parts.append(f"has_{pf.name}")
            parts.append(f"resolve_{pf._parent_klass.to_snake_case()}_id({pf.name}_id)")
        parts += self._create_field_forward_parts("update")
        return ", ".join(parts)

    def update_tcl_flag_defaults(self) -> str:
        """
        The `array set opts {...}` initializer body for this class's
        generated `update_<type> {args}` proc - one entry per parent
        field (only for a single-parent class), then per create field,
        in update_shim_params() order.
        """
        parent_fields = self.get_parent_fields()
        parts = [f"-{parent_fields[0].name} {{}}"] if len(parent_fields) == 1 else []
        parts += [f"-{f.name} {{}}" for f in self.get_create_fields()]
        return " ".join(parts)

    def update_tcl_call_args(self) -> str:
        """
        Space-joined Tcl argument list for calling update_<type>_cmd
        from this class's own update_<type> {args} proc, in
        update_shim_params() order - no required-flag list (nothing is
        ever required to update - update_<type> has no counterpart of
        create_tcl_required_flags()). The optional parent flag (single-
        parent classes only) passes a has-flag/value pair the same way
        an optional numeric create field does; every create field
        follows via its own Field.cmd_tcl_call_args("update").
        """
        parts = []
        parent_fields = self.get_parent_fields()
        if len(parent_fields) == 1:
            opt = f"$opts(-{parent_fields[0].name})"
            parts.append(f"[expr {{{opt} ne {{}} ? 1 : 0}}]")
            parts.append(opt)
        for f in self.get_create_fields():
            parts.extend(f.cmd_tcl_call_args("update"))
        return " ".join(parts)

    # --- Help system (UPDATES.md item 20) - create_<type>/update_<type>
    # only. get_<type>'s own usage/options depend on external SearchScope
    # data (tcl_scope.py's -of param computation, not knowable from a
    # Klass alone), so that string is still built inline in
    # le_tcl_procs_generated_tcl_j2.py's own get_<type> block, same as
    # today - these four methods cover the two families that are fully
    # self-contained on this Klass instead. ---

    def create_tcl_usage(self) -> str:
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
        parent_fields = self.get_parent_fields()
        single_parent = len(parent_fields) == 1
        parts = [f"create_{self.to_snake_case()}"]
        for pf in parent_fields:
            frag = f"-{pf.name} <token>"
            parts.append(frag if single_parent else f"\\[{frag}\\]")
        for f in self.get_create_fields():
            frag = f"-{f.name} <{f.tcl_help_type_label()}>"
            parts.append(frag if f.create_required() else f"\\[{frag}\\]")
        parts.append("\\[-help\\]")
        return " ".join(parts) + " - " + self.tcl_description_escaped()

    def tcl_description_escaped(self) -> str:
        """
        This class's own description, escaped for safe embedding inside a
        generated Tcl "..."-quoted string (see tcl_dquote_escape) - used
        by create_tcl_usage()/update_tcl_usage() and by every family's
        register_command_help call in le_tcl_procs_generated_tcl_j2.py
        wherever it embeds the bare description as its own argument.
        """
        return tcl_dquote_escape(self.description)

    def create_tcl_help_options(self) -> str:
        """
        Tcl list literal (inner content - wrap in {...} at the call site)
        of this class's generated create_<type> command's own options, one
        `{-flag {type T required R description {D}}}` entry per parent
        field then create field, in create_tcl_flag_defaults()'s own field
        order - the registration payload help/man/complete_command
        (le_tcl_procs.tcl) read.
        """
        parent_fields = self.get_parent_fields()
        single_parent = len(parent_fields) == 1
        parts = []
        for pf in parent_fields:
            required = 1 if single_parent else 0
            if single_parent:
                desc = f"Parent {pf.type} token"
            else:
                desc = f"Parent {pf.type} token - exactly one of this class's parent flags is required"
            parts.append(
                f"{{-{pf.name} {{type token required {required} description {{{tcl_brace_escape(desc)}}}}}}}"
            )
        for f in self.get_create_fields():
            required = 1 if f.create_required() else 0
            parts.append(
                f"{{-{f.name} {{type {f.tcl_help_type_label()} required {required} "
                f"description {{{tcl_brace_escape(f.description)}}}}}}}"
            )
        return " ".join(parts)

    def update_tcl_usage(self) -> str:
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
        parent_fields = self.get_parent_fields()
        parts = [f"update_{self.to_snake_case()}", "<id>"]
        if len(parent_fields) == 1:
            parts.append(f"\\[-{parent_fields[0].name} <token>\\]")
        for f in self.get_create_fields():
            parts.append(f"\\[-{f.name} <{f.tcl_help_type_label()}>\\]")
        parts.append("\\[-help\\]")
        return " ".join(parts) + " - " + self.tcl_description_escaped()

    def update_tcl_help_options(self) -> str:
        """
        update_tcl_usage()'s own options-list counterpart, mirroring
        create_tcl_help_options() but with every `required` entry forced
        to 0 (see update_tcl_call_args()'s own "nothing is ever required
        to update" rule) and, for a single-parent class, its own optional
        parent-reassignment flag instead of create's always-parent-token
        entry.
        """
        parent_fields = self.get_parent_fields()
        parts = []
        if len(parent_fields) == 1:
            pf = parent_fields[0]
            desc = f"Reassign this object's parent {pf.type} (token)"
            parts.append(f"{{-{pf.name} {{type token required 0 description {{{tcl_brace_escape(desc)}}}}}}}")
        for f in self.get_create_fields():
            parts.append(
                f"{{-{f.name} {{type {f.tcl_help_type_label()} required 0 "
                f"description {{{tcl_brace_escape(f.description)}}}}}}}"
            )
        return " ".join(parts)

    def delete_tcl_usage(self) -> str:
        """
        The one-line `delete_<type> <id> [-help] - <description>` usage
        string registered for this class's generated delete_<type> command
        (and returned by its own `-help` flag) - much narrower than
        create_tcl_usage()/update_tcl_usage(): delete_<type> takes exactly
        one positional `<id>` and no flags of its own (nothing to specify -
        see delete_api_body() for the cascade a delete triggers). When this
        class owns any tcl_child_list_fields(), the description names every
        child type deleted along with it, so `-help`/`man` surfaces the
        cascade instead of a caller discovering it only after the fact. See
        create_tcl_usage()'s own comment for why every `[`/`]` here is
        backslash-escaped.
        """
        cascade = self.tcl_child_list_fields()
        if cascade:
            child_types = ", ".join(sorted({f.type for f in cascade}))
            cascade_note = f" Also deletes every {tcl_dquote_escape(child_types)} it owns (recursively, through any of their own owned children too)."
        else:
            cascade_note = ""
        return (
            f"delete_{self.to_snake_case()} <id> \\[-help\\] - "
            f"{self.tcl_description_escaped()}.{cascade_note}"
        )

    def delete_tcl_help_options(self) -> str:
        """
        delete_tcl_usage()'s own options-list counterpart - just the one
        required `<id>` positional (delete_<type> has no flags of its own,
        unlike create_<type>/update_<type> - the call site appends the
        usual `-help` entry itself, same as every other family's own
        registration call).
        """
        desc = f"Friendly id of the {self.name} to delete"
        return f"{{<id> {{type token required 1 description {{{tcl_brace_escape(desc)}}}}}}}"

    def tcl_property_scalars(self) -> str:
        """
        Tcl list literal (inner content) of this class's own scalar
        property names available for dot-path completion
        (complete_command, le_tcl_procs.tcl) - exactly
        get_filterable_scalar_fields()'s own field names, the same source
        api_filter_tables_inc_j2.py's C++ filter-field table already uses,
        so a suggested path can never diverge from what -filter/
        get_properties actually accepts.
        """
        return " ".join(f.name for f in self.get_filterable_scalar_fields())

    def tcl_property_hops(self) -> str:
        """
        Tcl list literal (inner content) of `{hop_name target_snake_case}`
        pairs for this class's own traversal hops
        (get_filterable_hop_fields()) - the target's own to_snake_case()
        key back into ::property_scalars/::property_hops, so
        complete_command can walk a chained dot-path
        (.terminal_port.terminal.name) one hop at a time.
        """
        return " ".join(
            f"{{{f.name} {f.hop_target_snake_case()}}}" for f in self.get_filterable_hop_fields()
        )

    def update_api_body(self) -> str:
        """
        The full statement-list body of le_update_<type>(LeHandle
        *handle, <update_api_params()>) - built the same "Python
        string" way create_api_body() is, for the same reason. Unlike
        create_api_body(), every enum field is parsed the way create's
        *optional* branch already does (create_required("update") is
        always False, so there's only one branch here, not two);
        dbu_per_um is fetched unconditionally whenever this class has
        any dbu-using create field (cmd_uses_dbu()), matching
        create_api_body()'s own unconditional fetch - safe because
        Field.update_root_arg_expr()'s `has_<field> ? std::optional<T>
        (cmd_value_expr()) : std::nullopt` is a ternary, which only
        evaluates the branch it selects, so a `*dbu_per_um` dereference
        inside a field's own value expression never runs unless that
        field's own has-flag is true (this is the same reasoning that
        already makes create_api_body()'s identical unconditional fetch
        safe for an *optional* dbu create field today).

        Ends with exactly one call into Root::update_<klass>(), passing
        every field as an always-std::optional<T> argument (see Field.
        update_root_arg_expr()) - Root itself decides per field whether
        "provided" (has_value()) means apply it, mirroring how
        Root::create_<klass>() is the only place that ever touches
        index_ for creation.
        """
        indent = "        "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        parent_fields = self.get_parent_fields()
        create_fields = self.get_create_fields()
        enum_fields = [f for f in create_fields if f.is_enum_type()]
        dbu_fields = [f for f in create_fields if f.cmd_uses_dbu()]
        unique_fields = self.get_unique_per_parent_fields()
        single_parent = parent_fields[0] if len(parent_fields) == 1 else None

        add("if (!handle)")
        add("    return 1;")
        add("std::lock_guard<std::mutex> lock(handle->mutex_);")
        add()
        add(f"const le::{self.name}Id typed_id = from_c(id);")
        add(f"const le::{self.name}Data *existing_{snake} = handle->root.get_{snake}(typed_id);")
        add(f"if (!existing_{snake})")
        add("{")
        add(f'    handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: unknown id"));')
        add("    return 1;")
        add("}")
        add()
        add("// UPDATES.md item 21 - snapshotted before the mutation below,")
        add("// so a currently-recording transaction can undo back to this")
        add("// exact state (see the record_update call further down).")
        add(f"const le::{self.name}Data before_{snake} = *existing_{snake};")

        if single_parent is not None:
            add()
            add(f"le::{single_parent.type}Id {single_parent.name} = le::{single_parent.type}Id{{}};")
            add(f"if (has_{single_parent.name})")
            add("{")
            add(f"    {single_parent.name} = from_c({single_parent.name}_id);")
            add(f"    if (!handle->root.get_{single_parent._parent_klass.to_snake_case()}({single_parent.name}))")
            add("    {")
            add(
                f'        handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: unknown '
                f'{single_parent.name} - no such {single_parent.type} exists"));'
            )
            add("        return 1;")
            add("    }")
            add("}")

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
                add(
                    f'    handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: unrecognized '
                    f"{f.name} '{{}}'\", {f.name}));"
                )
                add("    return 1;")
                add("}")

        if dbu_fields:
            add()
            add("const std::optional<double> dbu_per_um = database_units_microns(handle->root);")
            add("if (!dbu_per_um)")
            add("{")
            add(f'    handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: no Technology has been read yet (needed for micron-to-dbu conversion)"));')
            add("    return 1;")
            add("}")

        list_compound_fields = [f for f in create_fields if f.list_compound_kind() is not None]
        for f in list_compound_fields:
            add()
            lines.extend(f.list_compound_parse_lines("update", "return 1;", f"le_update_{snake}"))

        add()
        call_args = ["typed_id"]
        if single_parent is not None:
            call_args.append(single_parent.name)
        for f in create_fields:
            call_args.append(f.update_root_arg_expr())
        add(f"const bool ok = handle->root.update_{snake}({', '.join(call_args)});")

        if unique_fields:
            add("if (!ok)")
            add("{")
            add(
                f'    handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: a sibling {self.name} '
                f'with this {unique_fields[0].name} already exists"));'
            )
            add("    return 1;")
            add("}")
        else:
            add("if (!ok)")
            add("{")
            add(f'    handle->messages.push_back(fmt::format("ERROR: le_update_{snake}: update failed"));')
            add("    return 1;")
            add("}")

        add("handle->root.bump_mutation_version();")
        add()
        add("// UPDATES.md item 21 - see create_api_body()'s own comment.")
        add("if (handle->command_history.is_recording())")
        add("{")
        add(f"    const le::{self.name}Data after_{snake} = *handle->root.get_{snake}(typed_id);")
        add(f"    handle->command_history.current()->record_update<le::{self.name}Id, le::{self.name}Data>(")
        add(f"        typed_id, before_{snake}, after_{snake}, &le::apply_{snake}_snapshot);")
        add("}")
        add("return 0;")

        return "\n".join(lines)

    def apply_snapshot_body(self) -> str:
        """
        The full statement-list body of `inline bool apply_<snake>_snapshot
        (Root &root, <Klass>Id id, const <Klass>Data &data)`
        (api_snapshot_appliers_hpp_j2.py) - reduces a *whole* field
        snapshot to one real Root::update_<klass>() call, reusing exactly
        the same field list/single-parent handling update_root_params()
        does. Every create field is wrapped in `std::optional<T>(data.
        <field>)` (T = root_value_cpp_type()) - this constructs correctly
        whether `data.<field>` is itself a bare T or already a
        std::optional<T> (Field.root_value_cpp_type()'s own comment
        explains why Root::update_<klass>() never double-wraps an
        is_optional-in-storage field), so no is_optional branching is
        needed here unlike update_api_body()'s enum/str/dbu handling -
        this operates on already-typed C++ values, not TCL C-string
        arguments, so none of that parsing applies.
        """
        indent = "        "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        parent_fields = self.get_parent_fields()
        create_fields = self.get_create_fields()
        single_parent = parent_fields[0] if len(parent_fields) == 1 else None

        call_args = ["id"]
        if single_parent is not None:
            call_args.append(f"data.{single_parent.name}")
        for f in create_fields:
            call_args.append(f"std::optional<{f.root_value_cpp_type(qualified=False)}>(data.{f.name})")

        add(f"return root.update_{snake}({', '.join(call_args)});")

        return "\n".join(lines)

    def update_root_params(self) -> str:
        """
        Full comma-joined parameter list for this class's generated
        Root::update_<klass>(...) (root_hpp_j2.py) - <Klass>Id id, then
        (iff single-parent) a bare <Parent>Id <parent_field> whose own
        .valid() *is* the "reassign?" signal (mirrors create_x's own
        convention for an optional/absent parent - see the pre-existing
        create_<klass> template's `if (d.<field>.valid()) index_...`
        pattern), then one std::optional<T> per create field (T =
        Field.root_value_cpp_type(qualified=False) - bare, no "le::"
        prefix, since this is emitted inside `namespace le` already) -
        has_value() is Root's own "was this field provided" signal, no
        separate has_<field> bool needed at this layer (unlike
        update_api_params(), which still needs explicit has-flags/
        nullptr-style raw C-API slots, since that's the boundary where
        the "was it provided" signal first has to be materialized out of
        raw Tcl/C input).
        """
        parts = [f"{self.name}Id id"]
        parent_fields = self.get_parent_fields()
        if len(parent_fields) == 1:
            pf = parent_fields[0]
            parts.append(f"{pf.type}Id {pf.name}")
        for f in self.get_create_fields():
            parts.append(f"std::optional<{f.root_value_cpp_type(qualified=False)}> {f.name}")
        return ", ".join(parts)

    def update_root_body(self) -> str:
        """
        The full statement-list body of Root::update_<klass>
        (update_root_params()) - the *only* place this class's fields
        are ever mutated after creation (the user's explicit "no per-
        field setters, generated or hand-written" constraint - see the
        plan's Context section). Built the same "Python string" way
        create_api_body()/update_api_body() are; reuses the exact
        index_ member names/shapes the pre-existing (and untouched)
        generated set_<klass>_<field>() already established for a
        parent field (root_hpp_j2.py's own `{%- if field.parent or
        field.index %}` block) and a unique_per_parent field, but -
        unlike that pre-existing per-field setter, which reassigns a
        parent without moving a unique_per_parent field's sibling-index
        bucket (a documented, previously-unreachable gap, since nothing
        called it with both present) - this method does both in one
        call, in the order decision 6 requires: reparent (with its own
        new-parent collision check) fully applied *before* the
        unique_per_parent rename, so `existing-><parent_field>` already
        reflects the new parent by the time the rename's own sibling
        bucket is computed.

        Every ordinary field: `if (<field>) existing-><field> =
        *<field>;` - trivial, no index to maintain. Returns true unless
        the reparent's new-parent collision check or the
        unique_per_parent rename's own collision check fails (checked,
        and can return false, before any mutation happens in that block
        - matches create_x's own "nothing inserted on failure" atomicity
        for the common single-change case; see decision 6 for the one
        documented two-change exception).
        """
        indent = "            "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        parent_fields = self.get_parent_fields()
        create_fields = self.get_create_fields()
        unique_fields = self.get_unique_per_parent_fields()
        single_parent = parent_fields[0] if len(parent_fields) == 1 else None

        add(f"auto* existing = {snake}_.get(id);")
        add("if (!existing) return false;")

        if single_parent is not None:
            pf = single_parent
            parent_index = f"index_.{pf._parent_klass.to_snake_case()}_{pf.parent}"
            is_list = pf._parent_field.is_list
            add()
            # `&& {pf.name} != existing->{pf.name}` - reparenting onto the
            # object's own current parent is a no-op, not a collision: with
            # a unique_per_parent field, the new-parent bucket lookup below
            # would otherwise find the object's own existing entry (itself,
            # not a real sibling) and spuriously report a collision.
            add(f"if ({pf.name}.valid() && {pf.name} != existing->{pf.name})")
            add("{")
            if unique_fields:
                uf = unique_fields[0]
                add(f"    auto new_bucket_it = index_.{snake}_by_{uf.name}.find({pf.name});")
                add(
                    f"    if (new_bucket_it != index_.{snake}_by_{uf.name}.end() && "
                    f"new_bucket_it->second.find(existing->{uf.name}) != new_bucket_it->second.end())"
                )
                add("        return false;")
                add()
            if is_list:
                add("    {")
                add(f"        auto& old_siblings = {parent_index}[existing->{pf.name}];")
                add(
                    "        old_siblings.erase(std::remove(old_siblings.begin(), old_siblings.end(), id), "
                    "old_siblings.end());"
                )
                add("    }")
            else:
                add(f"    {parent_index}.erase(existing->{pf.name});")
            if unique_fields:
                uf = unique_fields[0]
                add("    {")
                add(f"        auto old_bucket_it = index_.{snake}_by_{uf.name}.find(existing->{pf.name});")
                add(f"        if (old_bucket_it != index_.{snake}_by_{uf.name}.end())")
                add(f"            old_bucket_it->second.erase(existing->{uf.name});")
                add("    }")
            add(f"    existing->{pf.name} = {pf.name};")
            if is_list:
                add("    {")
                add(f"        auto& new_siblings = {parent_index}[{pf.name}];")
                add("        if (std::find(new_siblings.begin(), new_siblings.end(), id) == new_siblings.end())")
                add("            new_siblings.push_back(id);")
                add("    }")
            else:
                add(f"    {parent_index}[{pf.name}] = id;")
            if unique_fields:
                uf = unique_fields[0]
                add(f"    index_.{snake}_by_{uf.name}[{pf.name}][existing->{uf.name}] = id;")
            add("}")

        for f in create_fields:
            if f.unique_per_parent:
                add()
                # `&& *{f.name} != existing->{f.name}` - renaming to the
                # object's own current value is a no-op, not a collision:
                # the sibling-bucket lookup below would otherwise find the
                # object's own existing entry (itself) and spuriously
                # report a collision (same reasoning as the reparent guard
                # above).
                add(f"if ({f.name} && *{f.name} != existing->{f.name})")
                add("{")
                add(f"    auto& siblings = index_.{snake}_by_{f.name}[existing->{single_parent.name}];")
                add(f"    if (siblings.find(*{f.name}) != siblings.end())")
                add("        return false;")
                add(f"    siblings.erase(existing->{f.name});")
                add(f"    existing->{f.name} = *{f.name};")
                add(f"    siblings[*{f.name}] = id;")
                add("}")
            else:
                add(f"if ({f.name}) existing->{f.name} = *{f.name};")

        add()
        add("return true;")
        return "\n".join(lines)

    def delete_api_body(self) -> str:
        """
        The full statement-list body of le_delete_<type>(LeHandle *handle,
        Le<Klass>Id id) - built the same "Python string" way
        create_api_body()/update_api_body() are. Unlike those two, most of
        the work here is cascading: every tcl_child_list_fields() entry is
        an owned pool-backed child collection that would otherwise become
        permanently unreachable garbage if left behind after this object is
        deleted (a TerminalPort, for instance, is only ever enumerated
        through its parent Terminal's own port list - there is no top-level
        "every TerminalPort" API), so deleting a <Klass> recursively
        deletes every descendant reachable through tcl_child_list_fields(),
        however many levels deep this class's own schema subtree actually
        goes (e.g. Technology's own non_default_rules -> vias -> layers
        chain is 3 levels deep; the formerly hand-written
        le_delete_terminal was only ever written 1 level deep - cascading
        to each TerminalPort but never that port's own Shapes - a real bug
        this generator fixes by construction, not just reproduces, since it
        expands the *actual* schema graph rather than a hand-copied slice
        of it).

        The recursion happens at *Python codegen time*
        (see the nested plan_edge() closure below), not runtime C++ - each
        schema-graph depth level becomes one more flat, unrolled loop in
        the emitted body (this codebase's own "flat generated code"
        aesthetic - see backend/CLAUDE.md's TCL codegen section - not a
        generic recursive C++ helper). A level's own children are always
        collected into one flat vector *across every parent object at that
        level* (not one nested vector per parent), so a 3-level cascade
        emits exactly 3 collection loops total, not a combinatorial nesting
        of loops per branch; each collected child's own immediate parent
        travels alongside it in a parallel IdCellPtr vector (built only
        when a transaction is actually recording - see below), so a later
        record_delete's own undo-recreate lambda can still fix up exactly
        the right parent-pointing field on that child, without needing to
        re-derive which parent it came from.

        UPDATES.md item 21: every cascaded descendant (deepest first) plus
        this object itself (last) is recorded via Transaction::record_delete,
        the same primitive create_api_body()/update_api_body() already use
        for their own single-object mutation. *Order* matters here
        specifically, unlike those two - see Transaction::undo_all's
        reverse-order replay (transaction.hpp): recording deepest-first/
        self-last means undo replays in the opposite order (self-first,
        deepest-last), so by the time any cascaded child's own undo-
        recreate lambda runs, it can read its immediate parent's
        *already-recreated* live id back out of the very same IdCellPtr
        Transaction::id_cell_for(id) memoizes by id value - captured once
        per parent object while it was still being collected as a "child"
        one level up, well before it (or any of its own ancestors) is
        actually deleted. This is exactly the pattern the 4 formerly
        hand-written cascading deletes (le_delete_terminal/_terminal_port/
        _obstruction, api.cpp - now deleted, superseded by this method)
        already established for their own single level; this generalizes
        it to however many levels deep a given class's own schema subtree
        actually goes, computed once per class from tcl_child_list_fields()
        rather than hand-copied per class.

        A class with no tcl_child_list_fields() at all (most of the ~35 -
        e.g. Shape, most technology-reference leaf classes) gets the
        trivial non-cascading shape instead: snapshot, erase, record_delete
        the one object, nothing else - a genuinely new delete surface for
        every one of those classes (previously none of them had any delete
        command at all, generated or hand-written).
        """
        indent = "        "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        root_fields = self.tcl_child_list_fields()

        add("if (!handle)")
        add("    return 1;")
        add("std::lock_guard<std::mutex> lock(handle->mutex_);")
        add()
        add(f"const le::{self.name}Id {snake}_id = from_c(id);")
        add(f"const le::{self.name}Data *existing_{snake} = handle->root.get_{snake}({snake}_id);")
        add(f"if (!existing_{snake})")
        add("    return 1;")
        add(f"const le::{self.name}Data {snake}_snapshot = *existing_{snake};")

        if not root_fields:
            # Trivial, non-cascading leaf delete - no owned children to walk.
            add()
            add(f"const bool deleted = handle->root.delete_{snake}({snake}_id);")
            add("handle->root.bump_mutation_version();")
            add()
            add("// UPDATES.md item 21 - a leaf delete (no tcl_child_list_fields()),")
            add("// so no id-cell indirection is needed the way a cascading delete")
            add("// needs it below - this object's own parent field(s), if any, aren't")
            add("// touched by this call, so the snapshot above stays valid regardless")
            add("// of undo/redo.")
            add("if (le::editing::Transaction *txn = handle->command_history.current())")
            add("{")
            add(f"    txn->record_delete<le::{self.name}Id, le::{self.name}Data>(")
            add(f"        {snake}_id, {snake}_snapshot,")
            add(f"        [](le::Root &r, const le::{self.name}Data &d) {{ return r.create_{snake}(d); }},")
            add(f"        [](le::Root &r, le::{self.name}Id i) {{ return r.delete_{snake}(i); }});")
            add("}")
            add("return deleted ? 0 : 1;")
            return "\n".join(lines)

        # --- Cascading delete plan - see this method's own docstring for
        # the overall shape. `edges` ends up ordered deepest-first/self-
        # last (plan_edge() recurses into a child's own grandchildren
        # *before* appending that child's own edge), which is exactly the
        # order the deletion and recording passes below both need to walk
        # it in.
        edges: List[dict] = []
        counter = [0]

        def plan_edge(owner_klass: "Klass", fld: "Field", owner_ids_expr: str, owner_is_scalar: bool, owner_cell_expr: str) -> None:
            child_klass = fld._child_klass
            child_field = fld._child_field
            if child_klass is None or child_field is None:
                raise ValueError(
                    f"delete_api_body: {owner_klass.name}.{fld.name} (type={fld.type!r}) has no "
                    f"resolved back-reference field - Klass.link() couldn't find a has_parent() "
                    f"field of type {owner_klass.name!r} on {fld.type!r} to fix up on undo-recreate."
                )
            idx = counter[0]
            counter[0] += 1
            owner_snake = owner_klass.to_snake_case()
            child_snake = child_klass.to_snake_case()
            ids_var = f"casc{idx}_{fld.name}_ids"
            snap_var = f"casc{idx}_{fld.name}_snapshots"
            parent_cell_var = f"casc{idx}_{fld.name}_parent_cells"
            grandchild_fields = child_klass.tcl_child_list_fields()
            cells_var = f"casc{idx}_{fld.name}_cells" if grandchild_fields else None

            add()
            if owner_is_scalar:
                add(f"const std::vector<le::{fld.type}Id> {ids_var} = handle->root.get_{owner_snake}_{fld.name}({owner_ids_expr});")
                add(f"std::vector<le::{fld.type}Data> {snap_var};")
                add(f"{snap_var}.reserve({ids_var}.size());")
                add(f"for (const le::{fld.type}Id child_id : {ids_var})")
                add(f"    {snap_var}.push_back(*handle->root.get_{child_snake}(child_id));")
            else:
                add(f"std::vector<le::{fld.type}Id> {ids_var};")
                add(f"std::vector<le::{fld.type}Data> {snap_var};")
                add(f"std::vector<le::editing::IdCellPtr<le::{owner_klass.name}Id>> {parent_cell_var};")
                add(f"for (size_t i = 0; i < {owner_ids_expr}.size(); ++i)")
                add("{")
                add(f"    const le::{owner_klass.name}Id owner_id = {owner_ids_expr}[i];")
                add(f"    for (const le::{fld.type}Id child_id : handle->root.get_{owner_snake}_{fld.name}(owner_id))")
                add("    {")
                add(f"        {ids_var}.push_back(child_id);")
                add(f"        {snap_var}.push_back(*handle->root.get_{child_snake}(child_id));")
                add("        if (txn)")
                add(f"            {parent_cell_var}.push_back({owner_cell_expr}[i]);")
                add("    }")
                add("}")

            if cells_var:
                add(f"std::vector<le::editing::IdCellPtr<le::{fld.type}Id>> {cells_var};")
                add("if (txn)")
                add("{")
                add(f"    {cells_var}.reserve({ids_var}.size());")
                add(f"    for (const le::{fld.type}Id child_id : {ids_var})")
                add(f"        {cells_var}.push_back(txn->id_cell_for(child_id));")
                add("}")

            for grandchild_field in grandchild_fields:
                plan_edge(child_klass, grandchild_field, ids_var, False, cells_var)

            edges.append(
                {
                    "field": fld,
                    "child_klass": child_klass,
                    "child_field_name": child_field.name,
                    "ids_var": ids_var,
                    "snap_var": snap_var,
                    "owner_is_scalar": owner_is_scalar,
                    "owner_cell_expr": owner_cell_expr,
                    "parent_cell_var": parent_cell_var,
                }
            )

        add()
        add("le::editing::Transaction *txn = handle->command_history.current();")
        add(f"le::editing::IdCellPtr<le::{self.name}Id> {snake}_cell = txn ? txn->id_cell_for({snake}_id) : nullptr;")

        for fld in root_fields:
            plan_edge(self, fld, f"{snake}_id", True, f"{snake}_cell")

        add()
        add("// Cascade delete, deepest descendants first (see this method's own")
        add("// docstring) - a direct pool erase (root.delete_x), not the public")
        add("// cascading le_delete_x API, to avoid a recursive mutex lock.")
        for e in edges:
            child_snake = e["child_klass"].to_snake_case()
            add(f"for (const le::{e['field'].type}Id child_id : {e['ids_var']})")
            add(f"    handle->root.delete_{child_snake}(child_id);")
        add(f"const bool deleted = handle->root.delete_{snake}({snake}_id);")
        add("handle->root.bump_mutation_version();")

        add()
        add("// UPDATES.md item 21 - same deepest-first/self-last recording order")
        add("// as the deletion pass above (see this method's own docstring for why).")
        add("if (txn)")
        add("{")
        for e in edges:
            child_type = e["field"].type
            child_snake = e["child_klass"].to_snake_case()
            fixup_field = e["child_field_name"]
            ids_var = e["ids_var"]
            snap_var = e["snap_var"]
            add(f"    for (size_t i = 0; i < {ids_var}.size(); ++i)")
            add("    {")
            if e["owner_is_scalar"]:
                parent_cell_expr = e["owner_cell_expr"]
            else:
                add(f"        const auto parent_cell = {e['parent_cell_var']}[i];")
                parent_cell_expr = "parent_cell"
            add(f"        txn->record_delete<le::{child_type}Id, le::{child_type}Data>(")
            add(f"            {ids_var}[i], {snap_var}[i],")
            add(f"            [{parent_cell_expr}](le::Root &r, const le::{child_type}Data &d)")
            add("            {")
            add(f"                le::{child_type}Data fixed = d;")
            add(f"                fixed.{fixup_field} = {parent_cell_expr}->id;")
            add(f"                return r.create_{child_snake}(fixed);")
            add("            },")
            add(f"            [](le::Root &r, le::{child_type}Id i) {{ return r.delete_{child_snake}(i); }});")
            add("    }")
        add(f"    txn->record_delete<le::{self.name}Id, le::{self.name}Data>(")
        add(f"        {snake}_id, {snake}_snapshot,")
        add(f"        [](le::Root &r, const le::{self.name}Data &d) {{ return r.create_{snake}(d); }},")
        add(f"        [](le::Root &r, le::{self.name}Id i) {{ return r.delete_{snake}(i); }});")
        add("}")
        add("return deleted ? 0 : 1;")

        return "\n".join(lines)

    def get_include_define(self) -> str:
        """
        Get the include define for the class.

        Returns:
            str: The include define.
        """
        return f"_{self.name.upper()}_HPP_"

    def get_forward_declarations(self, has_pool: bool | None = None) -> List[str]:
        """
        Get the forward declarations for the class.

        Returns:
            List[str]: The forward declarations.
        """
        forward_declarations = set()
        for field in self.fields:
            if field.is_reference() and field._type_klass is not None:
                if has_pool is not None:
                    if field._type_klass.has_pool != has_pool:
                        continue
                forward_declarations.add(field.type)
        return sorted(forward_declarations)

    def get_forward_includes(self, has_pool: bool | None = None) -> List[str]:
        """
        Get the forward includes for the class.

        Returns:
            List[str]: The forward includes.
        """
        return [
            to_snake_case(dec)
            for dec in self.get_forward_declarations(has_pool=has_pool)
        ]

    def to_snake_case(self) -> str:
        """
        Convert the class name to snake_case.

        Returns:
            str: The class name in snake_case.
        """
        return to_snake_case(self.name)

    def init_fields(self, parents=True) -> List["Field"]:
        """
        Initialize the fields for the class.

        Args:
            parents (bool): Whether to include parent fields.

        Returns:
            List[Field]: The initialized fields.
        """
        init_fields = []
        for field in self.fields:
            if field.has_parent() and not parents:
                continue
            if not field.has_default() and not field.is_child and not field.is_optional:
                init_fields.append(field)
        return init_fields

    def get_create_arguments(self) -> str:
        """
        Get the create arguments for the class.

        Returns:
            str: The create arguments.
        """
        arguments = []
        for field in self.init_fields():
            arguments.append(f"{field.get_cpp_type()} {field.to_camel_case()}")
        return ", ".join(arguments)

    def get_example_arguments(self) -> str:
        """
        Get the example arguments for the class.

        Returns:
            str: The example arguments.
        """
        arguments = []
        for field in self.init_fields():
            arguments.append(str(field.get_example()))
        return ", ".join(arguments)

    def get_example_update_arguments(self) -> str:
        """
        Get the example update arguments for the class.

        Returns:
            str: The example update arguments.
        """
        arguments = []
        for field in self.get_updatable_fields():
            if field.get_example() != "nullptr" and not field.has_parent():
                tuple_arg = f'{{"{field.to_camel_case()}", {field.get_example()}}}'
                arguments.append(tuple_arg)
        argument_str = ", ".join(arguments)

        return f"{{ {argument_str} }}"

    def get_ordered_fields(self):
        """
        Get the ordered fields for the class.

        Returns:
            List[Field]: The ordered fields.
        """
        # Returns a list of fields with the parent fields first, then other ordered alphabetically by name
        parent_fields = []
        other_fields = []
        for field in self.fields:
            if field.has_parent():
                parent_fields.append(field)
            else:
                other_fields.append(field)
        return sorted(parent_fields, key=lambda x: x.name) + sorted(
            other_fields, key=lambda x: x.name
        )

    def get_struct_fields(self):
        """
        Get the struct fields when using INDEXED_POOL export style

        Returns:
            List[Field]: The struct fields.
        """
        struct_fields = []
        for field in self.fields:
            ## Ignore child references
            if field.is_child and field.is_reference():
                continue

            struct_fields.append(field)
        return struct_fields

    def get_property_fields(self):
        """
        Fields to expose as a to_properties() row (INDEXED_POOLS only) -
        get_struct_fields() minus parent back-references, which are
        structural (an Id into another pool), not user-facing data.
        """
        return [f for f in self.get_struct_fields() if not f.has_parent()]

    def get_filterable_scalar_fields(self):
        """
        Leaf/scalar fields usable directly in a filter-expression
        comparison (INDEXED_POOLS only) - see get_filterable_hop_fields()
        for the traversal counterpart. get_struct_fields() minus parent
        back-references (structural, not user-facing data - same
        exclusion get_property_fields() already makes) and minus
        anything list or a reference to another (non-enum) klass, since
        those are hops rather than leaves. An enum field stays a leaf -
        it's a small closed set, compared as a string, same as
        to_properties() already treats it via wrap_with_to_property().
        """
        return [
            f
            for f in self.get_struct_fields()
            if not f.is_list
            and not f.has_parent()
            and (not f.is_reference() or f._type_klass.is_enum)
        ]

    def get_filterable_hop_fields(self):
        """
        Fields that are a traversal hop to another class, for
        filter-expression path resolution (INDEXED_POOLS only) - see
        get_filterable_scalar_fields() for the leaf counterpart. Includes
        is_child fields, which are never struct fields themselves (see
        get_struct_fields() - reachable only through Root's own
        child-list/child accessor) alongside ordinary reference struct
        fields, so this uses the full field list rather than
        get_struct_fields().
        """
        return [
            f
            for f in self.get_ordered_fields()
            if f.is_reference() and not f._type_klass.is_enum
        ]

    def get_updatable_fields(self):
        """
        Get the updatable fields for the class.

        Returns:
            List[Field]: The updatable fields.
        """
        return [field for field in self.get_ordered_fields() if not field.is_list]

    def link(self) -> None:
        """
        Link the class to its fields and schema.
        """
        if not self._schema:
            raise ValueError("Schema is not linked")
        for field in self.fields:
            field._klass = self
            if field.is_reference():
                field._type_klass = self._schema.get_klass(field.type)

            if field.has_parent():
                field._parent_klass = self._schema.get_klass(field.type)
                field._parent_field = self._schema.get_field(field.type, field.parent)  # type: ignore
            elif field.is_child:
                # Look for opposite field in child klass that has a matching parent
                child_klass = self._schema.get_klass(field.type)
                for child_field in child_klass.fields:
                    if child_field.has_parent() and child_field.type == self.name:
                        field._child_klass = child_klass
                        field._child_field = child_field
                        break

    def get_var_name(self) -> str:
        """
        Get the variable name for the class.

        Returns:
            str: The variable name.
        """
        # Return name with first letter lowercase
        return self.name[0].lower() + self.name[1:]

    def has_parent(self) -> bool:
        """
        Check if the class has a parent.

        Returns:
            bool: True if the class has a parent, False otherwise.
        """
        for field in self.fields:
            if field.has_parent():
                return True
        return False

    def get_create_ptr_type(self) -> str:
        """
        Get the create pointer type for the class.

        Returns:
            str: The create pointer type.
        """
        if self.has_parent():
            return f"std::weak_ptr<{self.name}>"
        else:
            return f"std::shared_ptr<{self.name}>"

    def get_parent_field(self) -> "Field":
        """
        Get the parent field for the class.

        Returns:
            Field: The parent field.

        Raises:
            ValueError: If no parent field is found.
        """
        for field in self.fields:
            if field.has_parent():
                return field
        raise ValueError("No parent field found")

    def is_root(self) -> bool:
        """
        Check if the class is the root klass.

        Returns:
            bool: True if the class is the root klass, False otherwise.
        """
        return not self.has_parent()

    def has_indecies(self) -> bool:
        for field in self.fields:
            if field.parent:
                return True
            if field.index:
                return True
        return False


@dataclass
class Field:
    """
    A field used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the field.

        description (str): The description of the field.

        type (str): The type of the field.

        example (Any): The example value of the field.

        default (Optional[Any]): The default value of the field.

        parent (Optional[str]): The parent of the field.

        is_child (bool): Whether the field is a child.

        is_list (bool): Whether the field is a list.

        is_optional (bool): Whether the field is optional.

        index (bool): Create an index for this field.

        unique_per_parent (bool): Requires index=True. Instead of a single
            flat index shared by every instance of this Klass, uniqueness
            (and the generated lookup) is scoped per sibling group sharing
            the same parent - e.g. Terminal.name is unique only within one
            Abstract, not globally, since real LEF libraries legitimately
            reuse pin names like VDD/IN0 across different Abstracts. The
            owning Klass must have exactly one parent field (get_parent_fields()
            == 1) - the scope is ambiguous otherwise. create_<klass> becomes
            fallible for such a Klass: it returns an invalid Id (rather than
            inserting) when a sibling under the same parent already has this
            field's value, mirroring this codebase's existing Id::valid()
            sentinel convention instead of introducing a new failure signal.

        create_excluded (bool): Opt this field out of create_<type>/
            update_<type> flag generation even though it would otherwise
            structurally qualify (a scalar, a flattenable compound, or a
            list_compound_kind()-eligible list) - e.g. Shape.rect_iterates/
            path_iterates/polygon_iterates (raw LEF ITERATE statements,
            re-expanded into concrete rects/paths/polygons by
            Pipeline::generate_shapes, never meant to be directly
            authored) and Shape.texts (a Pipeline-computed render-time
            label, never LEF-authored data - see is_create_field()'s own
            docstring for the general "not user-authorable data" theme
            this flag exists for).

        value (int): Optional enum value.
    """

    name: str
    description: str
    type: str
    example: Optional[Any] = None
    default: Optional[Any] = None
    parent: Optional[str] = None
    is_child: bool = False
    is_list: bool = False
    is_optional: bool = False
    index: bool = False
    unique_per_parent: bool = False
    create_excluded: bool = False
    value: Optional[int] = None
    _parent_klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _parent_field: Optional["Field"] = field(default=None, repr=False, init=False)
    _child_klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _child_field: Optional["Field"] = field(default=None, repr=False, init=False)
    _klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _type_klass: Optional[Klass] = field(default=None, repr=False, init=False)

    def has_parent(self) -> bool:
        """
        Check if the field has a parent.

        Returns:
            bool: True if the field has a parent, False otherwise.
        """
        return self.parent is not None

    def has_default(self) -> bool:
        """
        Check if the field has a default value.

        Returns:
            bool: True if the field has a default value, False otherwise.
        """
        return self.default is not None

    def to_camel_case(self, upper_first=False) -> str | None:
        """
        Convert the field name to camelCase.

        Args:
            upper_first (bool): Whether to capitalize the first letter.

        Returns:
            str: The field name in camelCase.
        """
        return to_camel_case(self.name, upper_first)

    def get_cpp_type(
        self,
        nolist=False,
        cast=False,
    ) -> str:
        """
        Get the C++ type for the field.

        Args:
            nolist (bool): Whether to exclude list types.
            cast (bool): Whether to cast the type.

        Returns:
            str: The C++ type.
        """
        # Handle parent fields
        if self.has_parent():
            return f"{self.type}Id"

        # Handle child fields
        if self.is_reference():
            if self.is_list and not nolist:
                return f"std::vector<{self.type}>"
            if (
                not self.is_child
                and self._type_klass
                and not self._type_klass.is_enum
                and self._type_klass.has_pool
            ):
                return f"{self.type}Id"

        # Handle other (primitive) fields
        if self.type in TYPEMAP:
            type = TYPEMAP[self.type][0]
        else:
            type = self.type

        # A list of a primitive type (e.g. a plain list of str) - the
        # is_child/is_reference branches above already handle is_list for
        # fields that reference another Klass; this covers the remaining
        # case. Takes priority over is_optional (same convention as those
        # branches, which return their vector type without ever consulting
        # is_optional) - an empty vector already conveys "no items".
        if self.is_list and not nolist:
            return f"std::vector<{type}>"

        # Handle optional fields
        if self.is_optional:
            type = f"std::optional<{type}>"

        return type

    def is_reference(self) -> bool:
        """
        Check if the field is a reference.

        Returns:
            bool: True if the field is a reference, False otherwise.

        Raises:
            ValueError: If the class or schema is not linked.
        """
        if self._klass is None:
            raise ValueError("Klass is not linked")

        if self._klass._schema is None:
            raise ValueError("Schema is not linked")

        return self.type in [klass.name for klass in self._klass._schema.classes]

    def is_enum_type(self) -> bool:
        """
        Whether this field's type is an enum Klass (RoutingDirection,
        SignalDirection, Orientation, ...) rather than a primitive
        (str/int/double/dbu/bool) or an embedded (has_pool=False) struct
        like Point/Rect/Symmetry.
        """
        return self._type_klass is not None and self._type_klass.is_enum

    def hop_target_snake_case(self) -> str:
        """
        snake_case key of this hop field's own target Klass
        (self._type_klass) - le_tcl_procs_generated_tcl_j2.py's own
        ::property_hops table key, so complete_command's dot-path walk
        (le_tcl_procs.tcl) can look up the next class's own
        ::property_scalars/::property_hops entry after following this hop.
        Only meaningful for a field get_filterable_hop_fields() would
        return (is_reference() and not an enum) - self._type_klass is
        None/an enum otherwise.
        """
        return self._type_klass.to_snake_case()

    def tcl_help_type_label(self) -> str:
        """
        Human-readable `<type>` label for this field's own create_<type>/
        update_<type> usage-string flag (Klass.create_tcl_usage()/
        update_tcl_usage()) and help-options registration (Klass.
        create_tcl_help_options()/update_tcl_help_options()) - this
        field's own scalar/enum/compound type name as-is (str/int/double/
        dbu/bool, an enum's own name, or a compound field's embedded-
        struct Klass name, e.g. "Point"), with a trailing "..." for a
        list_compound_kind() field (e.g. Shape.rects -> "Rect...") since
        its flag actually takes a Tcl list of several such records.
        """
        return f"{self.type}..." if self.is_list else self.type

    def compound_klass(self) -> Optional[Klass]:
        """
        This field's embedded-struct type Klass, if it's a flattenable
        compound field (Point/Rect/Symmetry/DensityCheckWindow/...) - see
        Klass.embedded_scalar_leaves(). None for a plain scalar/enum
        field, a pooled-class reference, or an embedded struct that isn't
        flattenable (e.g. ParallelRunLengthSpacingTable).
        """
        tk = self._type_klass
        if tk is None or tk.is_enum or tk.has_pool:
            return None
        return tk if tk.embedded_scalar_leaves() is not None else None

    def is_compound_create_field(self) -> bool:
        """
        Whether this field is a flattenable-embedded-struct create field
        (see compound_klass()) - distinct from is_create_field()'s
        broader "gets a flag at all" check, since a compound field's flag
        explodes into several C slots (one per scalar leaf) rather than
        one, and is always optional (see Field.create_required()).
        """
        if self.has_parent() or self.is_child or self.is_list:
            return False
        return self.compound_klass() is not None

    def list_compound_kind(self) -> Optional[str]:
        """
        Whether this is_list field is a *list* of flattenable embedded
        structs eligible for its own create_<type>/update_<type> flag -
        distinct from compound_klass()'s single-struct case (Point/Rect/
        Symmetry) in both shape (N records, not one) and Tcl wire format
        (a nested list, flattened with embedded per-record lengths rather
        than a fixed slot count - see Klass.cmd_tcl_preamble()/Field.
        list_compound_parse_lines()). Three kinds, all built on top of
        Klass.embedded_scalar_leaves()/is_point_list_wrapper():

        - "flat": the element Klass is itself flattenable
          (embedded_scalar_leaves() - e.g. Shape.rects: List[Rect], each
          Rect always exactly 4 scalar leaves) - every record has the
          same fixed arity, so the Tcl wire format needs no per-record
          length prefix, just `{{ll_x ll_y ur_x ur_y} ...}`.
        - "points": the element Klass is itself a point-list wrapper
          (Klass.is_point_list_wrapper() - e.g. Shape.polygons:
          List[Polygon], each Polygon a variable-length list of Points) -
          each record's own length varies, so the wire format needs a
          per-record point-count prefix.
        - "points_plus_scalars": the element Klass has exactly one
          point-list-wrapper-typed field (e.g. Path.polygon) plus one or
          more other flattenable scalar/compound fields (e.g. Path.width)
          - see list_compound_point_field()/list_compound_scalar_fields().
          The wire format prefixes each record's scalar field(s) (in
          declaration order) before its own point-count/coordinates.

        None for a plain scalar list (e.g. Shape.rect_masks - already
        excluded from is_create_field() the same way it always was), an
        is_child/parent list, or an embedded-struct list whose element
        doesn't match any of the three shapes above.
        """
        if not self.is_list or self.has_parent() or self.is_child:
            return None
        if self.create_excluded:
            return None
        tk = self._type_klass
        if tk is None or tk.is_enum or tk.has_pool:
            return None
        leaves = tk.embedded_scalar_leaves()
        if leaves is not None and all(leaf.type in ("dbu", "int", "double", "bool") for _, leaf in leaves):
            # Excludes a str-leaved flattenable struct (e.g. Text - label
            # is a str leaf embedded_scalar_leaves() itself doesn't
            # reject, since it only checks structural well-formedness,
            # not leaf types) - list_compound_parse_lines()'s "flat" kind
            # always converts leaves numerically (_leaf_value_expr()), so
            # a str leaf would either misconvert silently or fail to
            # compile; excluded here instead of ever generating either.
            return "flat"
        if tk.is_point_list_wrapper():
            return "points"
        point_fields = [
            f
            for f in tk.fields
            if not f.is_list
            and not f.has_parent()
            and not f.is_child
            and f._type_klass is not None
            and f._type_klass.is_point_list_wrapper()
        ]
        if len(point_fields) != 1:
            return None
        others = [f for f in tk.fields if f is not point_fields[0]]
        others_ok = others and all(
            not f.is_list
            and not f.has_parent()
            and not f.is_child
            and (f._type_klass is None or f._type_klass.is_enum or f.is_compound_create_field())
            for f in others
        )
        return "points_plus_scalars" if others_ok else None

    def list_compound_point_field(self) -> Optional["Field"]:
        """
        For a "points_plus_scalars" list-compound field (see
        list_compound_kind()), the element Klass's own single
        point-list-wrapper-typed field (e.g. Path.polygon) - None for
        "flat"/"points" kinds, which have no such field to single out.
        """
        if self.list_compound_kind() != "points_plus_scalars":
            return None
        tk = self._type_klass
        for f in tk.fields:
            if not f.is_list and not f.has_parent() and not f.is_child and f._type_klass is not None and f._type_klass.is_point_list_wrapper():
                return f
        return None

    def list_compound_scalar_fields(self) -> List["Field"]:
        """
        For a "points_plus_scalars" list-compound field (see
        list_compound_kind()), the element Klass's own remaining fields
        besides list_compound_point_field() (e.g. Path.width) - in
        declaration order, the order list_compound_parse_lines() reads
        them from the flat wire encoding (each record's own scalars
        prefix its point-count/coordinates). Empty for "flat"/"points"
        kinds.
        """
        point_field = self.list_compound_point_field()
        if point_field is None:
            return []
        return [f for f in self._type_klass.fields if f is not point_field]

    @staticmethod
    def _compound_ctor_expr_indexed(klass: "Klass", array_expr: str, base_expr: str, start_offset: int = 0) -> Tuple[str, int]:
        """
        `_compound_ctor_expr()`'s own array-indexed counterpart, used for
        the "flat" list_compound_kind() (e.g. Rect) instead of the
        named-parameter version create_<type>'s own single-struct
        compound fields use - there are no named per-leaf parameters
        here, only a flat `array_expr[base_expr + offset]` positional
        walk (offset 0 omits the "+ 0"). `start_offset` threads a plain
        integer accumulator through nested recursion (Rect -> ll/ur, each
        a Point) rather than nesting "+ 2 + 1"-style string
        concatenation, so a doubly-nested leaf's own index still reads as
        a single "+ 3", not a chain of additions. Returns (ctor
        expression, number of flat slots this call itself consumed - not
        counting start_offset), so a caller stepping through several such
        records in a loop knows how far to advance.
        """
        parts = []
        offset = start_offset
        for f in klass.fields:
            tk = f._type_klass
            if tk is not None and not tk.is_enum:
                sub_expr, consumed = Field._compound_ctor_expr_indexed(tk, array_expr, base_expr, offset)
                parts.append(f".{f.name} = {sub_expr}")
                offset += consumed
            else:
                index_expr = base_expr if offset == 0 else f"{base_expr} + {offset}"
                parts.append(f".{f.name} = " + _leaf_value_expr(f, f"{array_expr}[{index_expr}]"))
                offset += 1
        return f"le::{klass.name}{{" + ", ".join(parts) + "}", offset - start_offset

    def list_compound_c_param_name(self) -> str:
        """The flat-array C parameter name for this list-compound create field - `<field>_flat_um`."""
        return f"{self.name}_flat_um"

    def list_compound_parse_lines(self, mode: str, invalid_return: str, cmd_name: str) -> List[str]:
        """
        api.cpp-layer statements (8-space indented, matching create_api_body()/
        update_api_body()'s own base indent) that parse this list-compound
        create field's own `<field>_flat_um`/`<field>_flat_count` slots
        (cmd_param_slots()) - the flat, per-record-length-prefixed
        encoding Klass.cmd_tcl_preamble() builds Tcl-side (see its own
        docstring for the exact wire format per kind) - into a ready-to-
        use `<field>_value` local: a plain `std::vector<ElementKlass>` in
        create mode (an omitted flag already means "no records", so the
        parse loop just runs unconditionally over a possibly-empty flat
        array); the same declaration but the parse loop itself guarded by
        `if (has_<field>)` in update mode, so `<field>_value` stays
        default-constructed (empty) - matched by
        Field.update_root_arg_expr()'s own `has_<field> ? std::optional
        (...) : std::nullopt` wrapping - when the flag was never provided
        at all. Assumes a local `dbu_per_um` already exists
        (cmd_uses_dbu() is always True for a list-compound field, so
        create_api_body()/update_api_body() already guarantee this by
        the time this runs). Every arity/malformed-encoding check here is
        defense in depth against a caller reaching *_cmd directly with a
        hand-built flat array - the generated Tcl preamble already
        guarantees a well-formed one.
        """
        kind = self.list_compound_kind()
        element_klass = self._type_klass
        name = self.name
        param = self.list_compound_c_param_name()
        count_param = f"{name}_flat_count"
        lines: List[str] = []

        def add(text: str = "", depth: int = 0) -> None:
            lines.append(f"        {'    ' * depth}{text}" if text else "")

        add(f"std::vector<le::{element_klass.name}> {name}_value;")
        guarded = mode == "update"
        if guarded:
            add(f"if (has_{name})")
            add("{")
        d = 1 if guarded else 0

        if kind == "flat":
            leaf_count = len(element_klass.embedded_scalar_leaves())
            add(f"if ({count_param} % {leaf_count} != 0)", d)
            add("{", d)
            add(
                f'    handle->messages.push_back(fmt::format("ERROR: {cmd_name}: -{name}: each entry needs '
                f'{leaf_count} coordinates"));',
                d,
            )
            add(f"    {invalid_return}", d)
            add("}", d)
            ctor_expr, _ = Field._compound_ctor_expr_indexed(element_klass, param, "i")
            add(f"for (int32_t i = 0; i < {count_param}; i += {leaf_count})", d)
            add(f"    {name}_value.push_back({ctor_expr});", d)
        elif kind == "points":
            # {count_param} == 0 is a genuinely valid "zero records"
            # input (not just the Tcl preamble's own always-present
            # record-count prefix collapsing to a 1-element {0} - a
            # direct C caller reasonably passes a truly empty array/0
            # count for "none provided" too), so it's guarded separately
            # from the malformed-encoding checks inside the loop, which
            # only apply once we know at least the record-count prefix
            # itself is present.
            point_field = element_klass.fields[0]  # the sole is_list field (Klass.is_point_list_wrapper())
            add(f"if ({count_param} > 0)", d)
            add("{", d)
            add("    int32_t i = 0;", d)
            add(f"    const int32_t {name}_record_count = static_cast<int32_t>({param}[i++]);", d)
            add(f"    for (int32_t r = 0; r < {name}_record_count; ++r)", d)
            add("    {", d)
            add(f"        if (i >= {count_param})", d)
            add("        {", d)
            add(f'            handle->messages.push_back(fmt::format("ERROR: {cmd_name}: -{name}: malformed entry"));', d)
            add(f"            {invalid_return}", d)
            add("        }", d)
            add(f"        const int32_t {name}_point_count = static_cast<int32_t>({param}[i++]);", d)
            add(f"        if ({name}_point_count < 2 || i + {name}_point_count * 2 > {count_param})", d)
            add("        {", d)
            add(
                f'            handle->messages.push_back(fmt::format("ERROR: {cmd_name}: -{name}: each entry '
                f'needs at least 2 points"));',
                d,
            )
            add(f"            {invalid_return}", d)
            add("        }", d)
            add(f"        le::{element_klass.name} {name}_record;", d)
            add(f"        for (int32_t c = 0; c < {name}_point_count; ++c)", d)
            add("        {", d)
            add(
                f"            {name}_record.{point_field.name}.push_back(le::Point{{.x = to_dbu({param}[i], "
                f"*dbu_per_um), .y = to_dbu({param}[i + 1], *dbu_per_um)}});",
                d,
            )
            add("            i += 2;", d)
            add("        }", d)
            add(f"        {name}_value.push_back(std::move({name}_record));", d)
            add("    }", d)
            add("}", d)
        else:  # "points_plus_scalars"
            # See the "points" branch's own comment on why {count_param}
            # == 0 is guarded separately as a valid "zero records" input,
            # not folded into the malformed-encoding checks below.
            point_field = self.list_compound_point_field()
            scalar_fields = self.list_compound_scalar_fields()
            add(f"if ({count_param} > 0)", d)
            add("{", d)
            add("    int32_t i = 0;", d)
            add(f"    const int32_t {name}_record_count = static_cast<int32_t>({param}[i++]);", d)
            add(f"    for (int32_t r = 0; r < {name}_record_count; ++r)", d)
            add("    {", d)
            add(f"        if (i + {len(scalar_fields)} >= {count_param})", d)
            add("        {", d)
            add(f'            handle->messages.push_back(fmt::format("ERROR: {cmd_name}: -{name}: malformed entry"));', d)
            add(f"            {invalid_return}", d)
            add("        }", d)
            scalar_value_exprs = []
            for sf in scalar_fields:
                scalar_value_exprs.append(_leaf_value_expr(sf, f"{param}[i]"))
                add(f"        const auto {name}_{sf.name} = {scalar_value_exprs[-1]};", d)
                add("        ++i;", d)
            add(f"        const int32_t {name}_point_count = static_cast<int32_t>({param}[i++]);", d)
            add(f"        if ({name}_point_count < 2 || i + {name}_point_count * 2 > {count_param})", d)
            add("        {", d)
            add(
                f'            handle->messages.push_back(fmt::format("ERROR: {cmd_name}: -{name}: each entry '
                f'needs at least 2 points"));',
                d,
            )
            add(f"            {invalid_return}", d)
            add("        }", d)
            add(f"        le::{point_field.type} {name}_points;", d)
            add(f"        for (int32_t c = 0; c < {name}_point_count; ++c)", d)
            add("        {", d)
            add(
                f"            {name}_points.{point_field._type_klass.fields[0].name}.push_back(le::Point{{.x = "
                f"to_dbu({param}[i], *dbu_per_um), .y = to_dbu({param}[i + 1], *dbu_per_um)}});",
                d,
            )
            add("            i += 2;", d)
            add("        }", d)
            ctor_parts = [f".{point_field.name} = std::move({name}_points)"]
            ctor_parts += [f".{sf.name} = {name}_{sf.name}" for sf in scalar_fields]
            add(f"        {name}_value.push_back(le::{element_klass.name}{{" + ", ".join(ctor_parts) + "});", d)
            add("    }", d)
            add("}", d)

        if guarded:
            add("}")
        return lines

    def is_create_field(self) -> bool:
        """
        Whether this field gets its own flag on the generated
        create_<type>/update_<type> TCL/API commands - a scalar leaf value
        (str/int/double/dbu/bool/enum), not a parent reference, not a
        child relationship (is_child, set via a future add_X/set_X round,
        not at creation), a flattenable embedded-struct field (Point/Rect/
        Symmetry/DensityCheckWindow/... - see is_compound_create_field()),
        exploded into one flag with several C slots, or a *list* of
        flattenable embedded structs (see list_compound_kind() - e.g.
        Shape.rects/polygons/paths). A plain scalar list (e.g.
        Shape.rect_masks) or a non-flattenable embedded struct (e.g.
        ParallelRunLengthSpacingTable, a genuine variable-size table)
        stays excluded, deferred to a future add_X/set_X round. Never
        True if create_excluded is set, regardless of what this field
        would otherwise structurally qualify as.
        """
        if self.create_excluded or self.has_parent() or self.is_child:
            return False
        if self.is_list:
            return self.list_compound_kind() is not None
        if self._type_klass is None or self._type_klass.is_enum:
            return True
        return self.is_compound_create_field()

    def create_required(self, mode: str = "create") -> bool:
        """
        Whether create_<type>/update_<type> makes this an unconditionally-
        required flag. mode="update" is always False - nothing is ever
        required to *update* an existing object, that's the entire point
        of the command (an omitted flag there means "leave unchanged", not
        "unset" - see Klass.update_api_body()). In mode="create", mirrors
        is_optional, with two deliberate exceptions:
        - `bool` fields are never is_optional=True anywhere in this schema
          (false is already a zero-cost "not specified" default - see
          is_optional's own convention, established across the whole
          is_optional audit), so treating a bool create-flag as "required"
          would force every caller to spell out every boolean flag on
          every create call for no real benefit - always optional,
          regardless of the field's own is_optional value.
        - A compound (embedded-struct) field is likewise always optional
          regardless of is_optional - several, e.g. Abstract.size/origin/
          bbox, are is_optional=False today only because they were
          explicitly skipped during the is_optional audit (not a real
          LEF-syntax judgment), and forcing them required would enforce
          that scoping accident rather than a deliberate one.
        """
        if mode == "update":
            return False
        if self.type == "bool":
            return False
        if self.is_compound_create_field():
            return False
        if self.list_compound_kind() is not None:
            return False
        return not self.is_optional

    def create_needs_has_flag(self, mode: str = "create") -> bool:
        """
        Whether this create-flag needs a companion `has_<field>` int32_t
        parameter to distinguish "omitted" from "explicitly set to the
        type's zero value" (0/0.0/false) - true nullopt (create) or "leave
        unchanged" (update), not a zero-value default or an unconditional
        overwrite.
        - A compound field always needs one (one has-flag for the whole
          struct, not one per leaf - create_required() is always False for
          these, so this fires in both modes). A list_compound field
          (Field.list_compound_kind()) likewise always needs one - the
          flat array's own element count can't distinguish "omitted" from
          "explicitly provided as an empty list" (both flatten to count
          0), the same "zero is a legitimate value" reasoning `bool`
          needs one for in update mode.
        - str/enum never need one: they already have an unambiguous
          "omitted" signal at the C layer (nullptr, distinct from a real,
          even empty, string/enum name) that a companion flag would be
          redundant with, in both modes.
        - `bool` needs one only in mode="update": in create, `false`
          already means "unspecified" (see create_required()); in update,
          "omitted" (leave unchanged) and "explicitly set to false" are
          genuinely different and must be distinguishable.
        - `int`/`double`/`dbu` need one whenever the flag isn't
          unconditionally required - i.e. always in update mode (nothing
          is required there), and in create mode whenever create_required()
          is False.
        """
        if self.is_compound_create_field() or self.list_compound_kind() is not None:
            return True
        if mode == "update":
            return self.type in ("int", "double", "dbu", "bool")
        return self.create_required() is False and self.type in ("int", "double", "dbu")

    def create_c_param_name(self) -> str:
        """
        The C-level parameter name for this create-field's value slot -
        `<name>_um` for a dbu field (its value crosses the C API in
        microns, converted via database_units_microns()/to_dbu() - the
        same microns-at-the-C-boundary convention every dbu-crossing
        parameter in this generated surface follows), the bare field
        name for everything else.
        """
        return f"{self.name}_um" if self.type == "dbu" else self.name

    def create_c_type(self) -> str:
        """
        The C type of this create-field's own value slot (not counting
        any companion has_<field> parameter - see create_needs_has_flag).
        str and enum fields both cross the C API as `const char *` -
        enum-typed flags take the same spelling to_string()/from_string()
        use (e.g. "INPUT"), parsed via the matching generated
        <enum>_from_string() inside the generated create_<type> body,
        rather than a raw numeric code a caller would have to already
        know the encoding of.
        """
        if self.type == "str" or self.is_enum_type():
            return "const char *"
        if self.type == "dbu":
            return "double"
        if self.type == "bool":
            return "int32_t"
        if self.type == "int":
            return "int32_t"
        if self.type == "double":
            return "double"
        raise ValueError(f"create_c_type: unsupported create-field type {self.type!r} on field {self.name!r}")

    def create_c_param_decl(self) -> str:
        """
        The full "<type> <name>" declaration fragment for this create-
        field's own value-slot parameter (e.g. "const char *shape",
        "double width_um") - identical text used in le_create_<type>'s
        declaration (api.hpp) and definition (api.cpp) and the matching
        create_<type>_cmd shim declaration/definition, so those four
        always stay in sync by construction rather than by hand-copying.
        """
        ctype = self.create_c_type()
        name = self.create_c_param_name()
        return f"{ctype}{name}" if ctype.endswith("*") else f"{ctype} {name}"

    def cmd_param_slots(self, mode: str = "create") -> List[Tuple[str, str]]:
        """
        Every (c_type, param_name) positional slot this field occupies in
        a generated le_create_<type>/le_update_<type> (or the matching
        create_<type>_cmd/update_<type>_cmd shim) signature, in order: the
        has_<field> companion first (create_needs_has_flag(mode)), then
        one value slot per scalar leaf - exactly one for a plain field,
        N for a compound one (Point 2, Rect 4, Symmetry 3). This is the
        single place "one field -> one or more positional slots" is
        defined - create_api_params/create_shim_params/their update_*
        counterparts and cmd_forward_exprs() all derive from it, so a
        signature can never drift from a call site.
        """
        slots: List[Tuple[str, str]] = []
        if self.create_needs_has_flag(mode):
            slots.append(("int32_t", f"has_{self.name}"))
        if self.list_compound_kind() is not None:
            slots.append(("const double *", self.list_compound_c_param_name()))
            slots.append(("int32_t", f"{self.name}_flat_count"))
            return slots
        ck = self.compound_klass()
        if ck is None:
            slots.append((self.create_c_type(), self.create_c_param_name()))
            return slots
        for flat, leaf in ck.embedded_scalar_leaves():
            slots.append((_leaf_c_type(leaf), _leaf_param_name(self.name, flat, leaf)))
        return slots

    def cmd_forward_exprs(self, mode: str = "create") -> List[str]:
        """
        The expression(s) forwarding this field's own value(s) from a
        create_<type>_cmd/update_<type>_cmd shim call to
        le_create_<type>/le_update_<type>, in cmd_param_slots(mode) order
        - identical to the raw parameter for a numeric/compound-leaf slot,
        but converts an empty/null string to a real nullptr for an
        optional (or update-mode) str/enum field. Tcl/SWIG can't produce a
        null `const char *` argument directly (a Tcl string is never
        itself "null", only empty), so "empty means omitted" has to be
        applied here, at the first point value is actually plain C++ -
        matches the same "empty flag value means omitted" convention this
        codebase's hand-written -flag parsing already used everywhere
        (e.g. create_terminal_port's own `if {$opts(-terminal) eq {}}`
        check) before this generator existed.
        """
        exprs: List[str] = []
        if self.create_needs_has_flag(mode):
            exprs.append(f"has_{self.name}")
        if self.list_compound_kind() is not None:
            exprs.append(self.list_compound_c_param_name())
            exprs.append(f"{self.name}_flat_count")
            return exprs
        ck = self.compound_klass()
        if ck is not None:
            for flat, leaf in ck.embedded_scalar_leaves():
                exprs.append(_leaf_param_name(self.name, flat, leaf))
            return exprs
        name = self.create_c_param_name()
        if (self.type == "str" or self.is_enum_type()) and not self.create_required(mode):
            exprs.append(f"({name} && {name}[0]) ? {name} : nullptr")
        else:
            exprs.append(name)
        return exprs

    def cmd_uses_dbu(self) -> bool:
        """
        Whether this create/update field needs the shared `dbu_per_um`
        local (database_units_microns()) computed somewhere in the
        generated body - true for a plain dbu field, or a compound field
        with at least one dbu leaf (Point/Rect/DensityCheckWindow all
        qualify; Symmetry, all-bool, does not).
        """
        if self.type == "dbu":
            return True
        if self.list_compound_kind() is not None:
            return True
        ck = self.compound_klass()
        if ck is None:
            return False
        return any(leaf.type == "dbu" for _, leaf in ck.embedded_scalar_leaves())

    def cmd_value_expr(self) -> str:
        """
        The api.cpp-layer expression producing this field's final typed
        C++ value, assuming it was provided - no has-flag/nullopt
        wrapping (create_struct_init_expr() and Klass.update_api_body()
        each apply that themselves, since they wrap it differently -
        std::optional in a struct initializer vs. a plain value forwarded
        to Root::update_<klass>()). A plain field reuses the exact
        to_dbu(...)/*parsed_<enum>/!= 0/identity conversions
        create_struct_init_expr() already established; a compound field
        recurses over the *struct* (not the flat leaf list, so nesting
        like Rect{ll: Point, ur: Point} round-trips correctly) via
        _compound_ctor_expr().
        """
        ck = self.compound_klass()
        if ck is not None:
            return self._compound_ctor_expr(ck, self.name)
        name = self.create_c_param_name()
        if self.is_enum_type():
            return f"*parsed_{self.name}"
        if self.type == "dbu":
            return f"to_dbu({name}, *dbu_per_um)"
        if self.type == "bool":
            return f"{name} != 0"
        return name

    @staticmethod
    def _compound_ctor_expr(klass: Klass, prefix: str) -> str:
        """
        `le::<Klass>{.field = <expr>, ...}` for a compound field's value,
        recursing into any nested embedded struct (e.g. Rect's own ll/ur
        Points) so the constructed value has the right nested shape, not
        a flat list of leaves. `prefix` accumulates the flattened
        parameter-name prefix (e.g. "bbox", then "bbox_ll" for Rect.ll).
        """
        parts = []
        for f in klass.fields:
            tk = f._type_klass
            if tk is not None and not tk.is_enum:
                parts.append(f".{f.name} = " + Field._compound_ctor_expr(tk, f"{prefix}_{f.name}"))
            else:
                parts.append(f".{f.name} = " + _leaf_value_expr(f, _leaf_param_name(prefix, f.name, f)))
        return f"le::{klass.name}{{" + ", ".join(parts) + "}"

    def create_struct_init_expr(self) -> str:
        """
        The `.field = <expr>` initializer value for this create-field
        inside le_create_<type>'s own `le::<Klass>Data{...}` construction.
        Assumes (built by Klass.create_api_body(), which emits them in
        this order): a local `parsed_<name>` variable already exists for
        an enum field (declared/validated earlier in the function body),
        and `dbu_per_um` already exists for a field with cmd_uses_dbu().
        """
        if self.is_compound_create_field():
            return f"has_{self.name} ? std::optional<le::{self.type}>({self.cmd_value_expr()}) : std::nullopt"
        if self.list_compound_kind() is not None:
            # Klass.create_api_body() already parsed the flat wire
            # encoding into a `<field>_value` std::vector<ElementKlass>
            # local (see Field.list_compound_parse_lines()) - an omitted
            # flag already means "no records" there (create mode's parse
            # loop always runs, has_<field> or not), so this is a plain
            # move, never optional-wrapped - ShapeData.rects etc. are
            # themselves plain std::vector<T>, never std::optional<...>.
            return f"std::move({self.name}_value)"
        name = self.create_c_param_name()
        if self.is_enum_type():
            return f"*parsed_{self.name}" if self.create_required() else f"parsed_{self.name}"
        if self.type == "str":
            if self.create_required():
                return name
            return f"{name} ? std::optional<std::string>({name}) : std::nullopt"
        if self.type == "bool":
            return f"{name} != 0"
        if self.type == "dbu":
            expr = f"to_dbu({name}, *dbu_per_um)"
            return expr if self.create_required() else f"has_{self.name} ? std::optional<int64_t>({expr}) : std::nullopt"
        if self.type in ("int", "double"):
            cpp_type = "int" if self.type == "int" else "double"
            return name if self.create_required() else f"has_{self.name} ? std::optional<{cpp_type}>({name}) : std::nullopt"
        raise ValueError(f"create_struct_init_expr: unsupported create-field type {self.type!r} on field {self.name!r}")

    def cmd_tcl_call_args(self, mode: str = "create") -> List[str]:
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
        opt = f"$opts(-{self.name})"
        ck = self.compound_klass()
        args: List[str] = []
        if self.create_needs_has_flag(mode):
            if ck is not None or self.list_compound_kind() is not None:
                args.append(f"$has_{self.name}")
            else:
                args.append(f"[expr {{{opt} ne {{}} ? 1 : 0}}]")
        if self.list_compound_kind() is not None:
            # The flat list itself, not a separate count - the shared
            # POINTS_ARRAY_UM SWIG typemap derives the count from the
            # Tcl list's own length (le_api.i), the same as every
            # existing points_um-taking command already relies on.
            args.append(f"${self.name}_flat")
            return args
        if ck is not None:
            for flat, leaf in ck.embedded_scalar_leaves():
                args.append(f"${_leaf_param_name(self.name, flat, leaf)}")
            return args
        if self.type == "bool":
            args.append(f"[expr {{{opt} ne {{}} && {opt}}}]")
            return args
        if self.create_needs_has_flag(mode):
            args.append(f"[expr {{{opt} ne {{}} ? {opt} : 0}}]")
            return args
        args.append(opt)
        return args

    def _list_compound_tcl_preamble(self, cmd_name: str) -> str:
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
        """
        kind = self.list_compound_kind()
        element_klass = self._type_klass
        vals_var = f"{self.name}_vals"
        has_var = f"has_{self.name}"
        flat_var = f"{self.name}_flat"
        lines = [
            f"    set {vals_var} $opts(-{self.name})",
            f"    set {has_var} [expr {{[llength ${vals_var}] > 0 ? 1 : 0}}]",
            f"    set {flat_var} {{}}",
        ]
        if kind == "flat":
            n = len(element_klass.embedded_scalar_leaves())
            lines.append(f"    foreach entry ${vals_var} {{")
            lines.append(f"        if {{[llength $entry] != {n}}} {{")
            lines.append(f'            error "{cmd_name}: -{self.name}: each entry needs {n} coordinates, got [llength $entry]"')
            lines.append("        }")
            lines.append(f"        foreach c $entry {{ lappend {flat_var} $c }}")
            lines.append("    }")
        elif kind == "points":
            lines.append(f"    lappend {flat_var} [llength ${vals_var}]")
            lines.append(f"    foreach entry ${vals_var} {{")
            lines.append("        if {[llength $entry] % 2 != 0} {")
            lines.append(
                f'            error "{cmd_name}: -{self.name}: each entry needs an even number of x/y '
                'coordinates, got [llength $entry]"'
            )
            lines.append("        }")
            lines.append(f"        lappend {flat_var} [expr {{[llength $entry] / 2}}]")
            lines.append(f"        foreach c $entry {{ lappend {flat_var} $c }}")
            lines.append("    }")
        else:  # "points_plus_scalars"
            scalar_fields = self.list_compound_scalar_fields()
            scalar_names = " ".join(sf.name for sf in scalar_fields)
            arity = len(scalar_fields) + 1
            example = " ".join(f"<{sf.name}>" for sf in scalar_fields) + " {x y x y ...}"
            lines.append(f"    lappend {flat_var} [llength ${vals_var}]")
            lines.append(f"    foreach entry ${vals_var} {{")
            lines.append(f"        if {{[llength $entry] != {arity}}} {{")
            lines.append(f'            error "{cmd_name}: -{self.name}: each entry must be {{{example}}}"')
            lines.append("        }")
            lines.append(f"        lassign $entry {scalar_names} points")
            lines.append("        if {[llength $points] % 2 != 0} {")
            lines.append(
                f'            error "{cmd_name}: -{self.name}: each entry\'s point list needs an even number '
                'of x/y coordinates, got [llength $points]"'
            )
            lines.append("        }")
            for sf in scalar_fields:
                lines.append(f"        lappend {flat_var} ${sf.name}")
            lines.append(f"        lappend {flat_var} [expr {{[llength $points] / 2}}]")
            lines.append(f"        foreach c $points {{ lappend {flat_var} $c }}")
            lines.append("    }")
        return "\n".join(lines)

    def cmd_tcl_preamble(self, mode: str, cmd_name: str) -> str:
        """
        Generated Tcl statements for this compound field, emitted (by
        Klass.cmd_tcl_preamble(), which loops over every compound create
        field) after flag parsing and before the _cmd call - turns the
        flag's own Tcl list value into its has-flag + leaf-parameter
        locals cmd_tcl_call_args() references. Empty string if this field
        isn't compound or list_compound_kind()-eligible (see
        _list_compound_tcl_preamble() for that branch). `cmd_name` (e.g.
        "create_abstract") is only used to make an arity/keyword error
        message name the actual command.
        """
        if self.list_compound_kind() is not None:
            return self._list_compound_tcl_preamble(cmd_name)
        ck = self.compound_klass()
        if ck is None:
            return ""
        leaves = ck.embedded_scalar_leaves()
        vals_var = f"{self.name}_vals"
        has_var = f"has_{self.name}"
        lines = [
            f"    set {vals_var} $opts(-{self.name})",
            f"    set {has_var} [expr {{[llength ${vals_var}] > 0 ? 1 : 0}}]",
        ]
        if ck.compound_leaf_kind() == "numeric":
            n = len(leaves)
            names = " ".join(flat for flat, _ in leaves)
            lines.append(f"    if {{${has_var} && [llength ${vals_var}] != {n}}} {{")
            lines.append(
                f'        error "{cmd_name}: -{self.name} expects {n} values '
                + "{" + names + "}"
                + ', got [llength $' + vals_var + ']"'
            )
            lines.append("    }")
            zeros = " ".join("0" for _ in leaves)
            lines.append(f"    if {{!${has_var}}} {{ set {vals_var} {{{zeros}}} }}")
            params = " ".join(_leaf_param_name(self.name, flat, leaf) for flat, leaf in leaves)
            lines.append(f"    lassign ${vals_var} {params}")
        else:  # "flags" - Symmetry, keyword-set syntax mirroring LEF's own SYMMETRY X Y R90 ;
            for flat, leaf in leaves:
                lines.append(f"    set {_leaf_param_name(self.name, flat, leaf)} 0")
            lines.append(f"    foreach kw ${vals_var} {{")
            lines.append("        switch -- [string toupper $kw] {")
            for flat, leaf in leaves:
                lines.append(f"            {flat.upper()} {{ set {_leaf_param_name(self.name, flat, leaf)} 1 }}")
            expected = "/".join(flat.upper() for flat, _ in leaves)
            lines.append(
                f'            default {{ error "{cmd_name}: -{self.name}: unknown keyword '
                + '\\"$kw\\" - expected ' + expected + '" }'
            )
            lines.append("        }")
            lines.append("    }")
        return "\n".join(lines)

    def root_value_cpp_type(self, qualified: bool = True) -> str:
        """
        The bare (non-is_optional-wrapped) final C++ type of this create
        field's own value - the T in Root::update_<klass>'s
        std::optional<T> parameter for this field (Klass.
        update_root_params()) and in the std::optional<T>(...) this
        field's own update_root_arg_expr() constructs at the api.cpp
        call site. Deliberately independent of get_cpp_type()'s own
        is_optional wrapping - Root's "was this field provided in this
        update call" signal (the outer std::optional this method's
        result gets wrapped in) is a different concept from whether the
        field's own database storage is itself is_optional (the inner
        type here), and wrapping both together would double-wrap a field
        that's_optional=True in storage (e.g. Terminal.shape,
        Abstract.size). `qualified=True` (the api.cpp-layer call site,
        outside `namespace le`) prefixes an enum/compound type with
        "le::"; `qualified=False` (root_hpp_j2.py's own parameter
        declaration, already inside `namespace le`) leaves it bare.
        """
        if self.list_compound_kind() is not None:
            element = f"le::{self.type}" if qualified else self.type
            return f"std::vector<{element}>"
        ck = self.compound_klass()
        if ck is not None:
            return f"le::{self.type}" if qualified else self.type
        if self.is_enum_type():
            return f"le::{self.type}" if qualified else self.type
        if self.type == "str":
            return "std::string"
        if self.type == "dbu":
            return "int64_t"
        if self.type == "bool":
            return "bool"
        if self.type == "int":
            return "int"
        if self.type == "double":
            return "double"
        raise ValueError(f"root_value_cpp_type: unsupported create-field type {self.type!r} on field {self.name!r}")

    def update_root_arg_expr(self) -> str:
        """
        The api.cpp-layer argument this field contributes to the single
        Root::update_<klass>(...) call in Klass.update_api_body() -
        always std::optional<T> (T = root_value_cpp_type()), regardless
        of whether the field's own database storage is is_optional
        (Root's "was this field provided" signal is independent of
        storage optionality - see root_value_cpp_type()). An enum field
        reuses the parsed_<name> local update_api_body() already builds
        (identical shape to create_api_body()'s own non-required enum
        branch, since create_required("update") is always False); a str
        field applies the same "empty means omitted" convention inline
        (no has_<field> companion exists for str - see
        create_needs_has_flag()); every other field (numeric/bool/
        compound) uses its own has_<field> flag and cmd_value_expr() -
        safe to dereference *dbu_per_um inside that expression even
        though it's fetched unconditionally, since the surrounding
        ternary only evaluates the branch it selects.
        """
        if self.is_enum_type():
            return f"parsed_{self.name}"
        if self.type == "str":
            name = self.create_c_param_name()
            return f"({name} && {name}[0]) ? std::optional<std::string>({name}) : std::nullopt"
        if self.list_compound_kind() is not None:
            # Klass.update_api_body()'s own list-compound parsing already
            # guards the parse loop itself with `if (has_<field>)`, but
            # `<field>_value` stays a plain std::vector<ElementKlass> (see
            # Field.list_compound_parse_lines()) - the optional wrap
            # happens here, at the same call site every other has-flag
            # field wraps its own value at.
            return f"has_{self.name} ? std::optional<{self.root_value_cpp_type()}>({self.name}_value) : std::nullopt"
        cpp_type = self.root_value_cpp_type()
        return f"has_{self.name} ? std::optional<{cpp_type}>({self.cmd_value_expr()}) : std::nullopt"

    def get_example(self) -> Any:
        """
        Get the example value for the field.

        Returns:
            Any: The example value.
        """
        example = self.example
        if self._parent_field is not None:
            example = self.to_camel_case()
        elif isinstance(example, str):
            example = f'std::string("{example}")'
        elif isinstance(example, list):
            example = "{}"
        elif example is None:
            example = "nullptr"
        elif isinstance(example, bool):
            example = str(example).lower()
        elif isinstance(example, float):
            example = f"{example}f"
        return example

    def _optional_default_cpp(self) -> str:
        """
        Value-initialized default ("<bare-type>{}") for an is_optional
        field's unset case - 0/0.0 for numeric types, "" for std::string,
        a zero-initialized aggregate for a reference to another (always
        non-pooled, plain-struct) Klass like Point/Foreign. Same bare-type
        resolution get_cpp_type() itself uses before wrapping in
        std::optional<...>, kept in sync with it deliberately (not calling
        get_cpp_type() directly, since that already applies the
        std::optional<> wrapper this method exists to unwrap).
        """
        return f"{TYPEMAP[self.type][0] if self.type in TYPEMAP else self.type}{{}}"

    def wrap_with_to_string(self, code, namespace) -> str:
        if self.is_list:
            return f"std::to_string({code}.size())"

        if self.is_optional:
            # .value_or(...), not .value() - an unset optional field (LEF
            # marks plenty of fields genuinely optional, e.g. Abstract's
            # own POWER statement) must never throw
            # std::bad_optional_access just because to_string()/
            # to_properties() below got called on it - matches this
            # project's "no exceptions for expected-missing-data paths"
            # convention (CLAUDE.md), not merely a style preference here.
            code = f"{code}.value_or({self._optional_default_cpp()})"

        if self.is_reference():
            return f"{namespace}::to_string({code})"

        if self.type == "str":
            return code

        return f"std::to_string({code})"

    def _references_embedded_klass(self) -> bool:
        """
        True only for a reference field whose target has its own
        to_property_string() overload - a plain embedded (non-pooled)
        struct like Point/Rect/Polygon. False for an enum (only
        to_string() is generated for those) and false for a reference to
        a pooled Klass (INDEXED_POOLS export represents those as a bare
        XxxId - e.g. Instance.reference_design - which likewise only has
        a generic to_string() overload in ids.hpp, not a per-class
        to_property_string()).
        """
        return (
            self._type_klass is not None
            and not self._type_klass.is_enum
            and not self._type_klass.has_pool
        )

    def wrap_with_to_property_string(self, code, namespace, dbu_var="dbu_per_um") -> str:
        """
        Fully-expanded string form of this field, used by
        to_property_string() - the get_properties()-facing analog of
        wrap_with_to_string(), which deliberately collapses list fields
        (and, transitively, anything nested inside them) to a bare count
        for compact debug logging. This one recurses all the way down
        instead, so e.g. Shape.polygons's own Polygon.points list - and
        Shape.paths's Path.polygon.points, one level deeper still -
        actually appear, not just their counts. An enum-typed reference,
        or a reference to a pooled Klass (stored as a bare Id, e.g.
        Instance.reference_design), is the exception: neither has a
        to_property_string() overload (see _references_embedded_klass()),
        so those keep going through to_string(), same as
        wrap_with_to_string() already does for them.

        A `dbu`-typed scalar converts to microns via `dbu_var` (in scope
        at every to_property_string()/to_property_list_string() call site
        - see struct_hpp_j2.py) and formats with format_coordinate_um(),
        instead of the raw integer std::to_string() would otherwise give -
        this is the fix for Shape's rects/polygons/paths having been the
        only clean, unit-converted display anywhere (previously hand-
        patched in api.cpp; every `dbu` field anywhere now converts the
        same way, recursively, for free).

        A scalar reference to an embedded klass is wrapped in its own
        "{...}" here - to_property_string() itself returns bare,
        unbracketed content (its own fields space-joined, no enclosing
        brace of its own), so every *splice* site (this one, a scalar
        reference field nested in a parent struct; and
        wrap_with_to_display_property()'s own matching branch, a
        top-level property-table row) is responsible for adding exactly
        one wrapping brace pair around the nested value it embeds - the
        same responsibility to_property_list_string() already carries
        per list item. This reproduces api.cpp's old hand-written
        format_rect_um()'s exact "{{llx lly} {urx ury}}" shape
        generically: Point has no embedded-klass fields of its own so it
        stays bare ("llx lly"), Rect's own "ll"/"ur" fields each wrap
        their Point in one brace pair, and whatever embeds a whole Rect
        (a list, or a scalar field like Abstract.bbox) adds the outer
        pair.
        """
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f"{namespace}::to_property_list_string({code}, {dbu_var})"
            return f"std::to_string({code}.size())"

        if self.is_optional:
            code = f"{code}.value_or({self._optional_default_cpp()})"

        if self.is_reference():
            if self._references_embedded_klass():
                return f'"{{" + {namespace}::to_property_string({code}, {dbu_var}) + "}}"'
            return f"{namespace}::to_string({code})"

        if self.type == "str":
            return code

        if self.type == "dbu":
            return f"{namespace}::format_coordinate_um({namespace}::to_um({code}, {dbu_var}))"

        return f"std::to_string({code})"

    def wrap_with_to_property(self, code, namespace) -> str:
        """
        PropertyValue::make_*(name, ...) call for this field - the
        property-table analog of wrap_with_to_string. A list of a
        non-pooled Klass (Rect/Polygon/Path/etc. - get_property_fields()
        already excludes is_child+is_reference fields like
        Abstract.terminals as structural, not user-facing data, so any
        is_list+is_reference field reaching here is always an embedded
        value list) reports its full contents, one brace-delimited
        Tcl-list item per element via that element's own
        to_property_string() (the fully-recursive sibling of to_string()
        - see wrap_with_to_property_string()), rather than just
        "<name>_count" - a bare count was confusing once the real data
        (e.g. every Shape.rects coordinate) was one property lookup away
        anyway. A list of a plain scalar (int/str/double/...) still
        reports only "<name>_count", numeric kind (INT vs DOUBLE)
        preserved rather than collapsed to a string. A scalar
        (non-list) embedded-struct reference field is expanded the same
        fully-recursive way (e.g. Path.polygon's own points list, not
        just its count) - an enum, or a reference to a pooled Klass
        (e.g. Instance.reference_design, stored as a bare Id), is the
        exception, via to_string() same as before - see
        _references_embedded_klass().

        Deliberately leaves `dbu` fields as a raw PropertyValue::INT (not
        micron-converted) - this is also the method backing get_field(),
        the filter-expression leaf lookup (struct_hpp_j2.py), which has
        no Root/dbu_per_um context and whose numeric comparisons
        (`-filter "width > 1000"`) are defined in raw database units. Use
        wrap_with_to_display_property() instead for anything actually
        shown to a user - see its own docstring.
        """
        # Note: get_filterable_scalar_fields() (the only real caller, via
        # get_field()) excludes every reference-to-non-enum-klass field
        # (see its own docstring), so the is_list/is_reference-embedded
        # branches below are unreachable in practice - kept correct
        # (with a literal dbu_per_um=1.0, since no such variable is ever
        # in scope in get_field()) as defense-in-depth rather than left
        # subtly broken if that filter ever changes.
        name = self.name
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_property_list_string({code}, 1.0))'
            return f'{namespace}::PropertyValue::make_int("{name}_count", static_cast<int64_t>({code}.size()))'

        # .value_or(...), not .value() - see wrap_with_to_string's own
        # comment for why an unset optional field must degrade to a
        # default instead of throwing.
        value = (
            f"{code}.value_or({self._optional_default_cpp()})"
            if self.is_optional
            else code
        )

        if self.is_reference():
            if not self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_string({value}))'
            return f'{namespace}::PropertyValue::make_string("{name}", "{{" + {namespace}::to_property_string({value}, 1.0) + "}}")'
        if self.type == "str":
            return f'{namespace}::PropertyValue::make_string("{name}", {value})'
        if self.type in ("float", "double", "long double"):
            return f'{namespace}::PropertyValue::make_double("{name}", static_cast<double>({value}))'
        return f'{namespace}::PropertyValue::make_int("{name}", static_cast<int64_t>({value}))'  # int/dbu/bool/etc.

    def wrap_with_to_display_property(self, code, namespace, dbu_var="dbu_per_um") -> str:
        """
        The to_properties()-facing sibling of wrap_with_to_property(),
        used for every user-visible property table (get_properties/
        report_properties, the Property Viewer) - identical except a
        `dbu` scalar becomes a micron-formatted STRING
        (format_coordinate_um(to_um(value, dbu_var))) instead of a raw
        PropertyValue::INT, and every recursive to_property_string()/
        to_property_list_string() call threads `dbu_var` through so a
        `dbu` field nested inside an embedded struct (e.g. Point.x via
        Rect.ll) converts too. This is the fix for every non-Shape type's
        embedded Rect/Point/Polygon/Path fields having shown raw-dbu,
        debug-style `Rect{ll=Point{x=...}}` text while Shape's own
        rects/polygons/paths were hand-patched (in api.cpp) to show
        clean microns - see backend/src/api/api.cpp's now-deleted
        replace_shape_geometry_properties for the override this
        generalizes and replaces.
        """
        name = self.name
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_property_list_string({code}, {dbu_var}))'
            return f'{namespace}::PropertyValue::make_int("{name}_count", static_cast<int64_t>({code}.size()))'

        value = (
            f"{code}.value_or({self._optional_default_cpp()})"
            if self.is_optional
            else code
        )

        if self.is_reference():
            if not self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_string({value}))'
            return f'{namespace}::PropertyValue::make_string("{name}", "{{" + {namespace}::to_property_string({value}, {dbu_var}) + "}}")'
        if self.type == "str":
            return f'{namespace}::PropertyValue::make_string("{name}", {value})'
        if self.type == "dbu":
            return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::format_coordinate_um({namespace}::to_um({value}, {dbu_var})))'
        if self.type in ("float", "double", "long double"):
            return f'{namespace}::PropertyValue::make_double("{name}", static_cast<double>({value}))'
        return f'{namespace}::PropertyValue::make_int("{name}", static_cast<int64_t>({value}))'  # int/bool/etc.


@dataclass
class EnumValue:
    name: str
    value: int
