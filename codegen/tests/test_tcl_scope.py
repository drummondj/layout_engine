import unittest

from codegen.schema import Field, Klass, Schema
from codegen.tcl_scope import (
    AncestorScope,
    DescendantScope,
    RootScope,
    SelfScope,
    compute_search_scope,
    render_default_scope_cpp,
    render_of_check_cpp,
)


def _schema():
    """Lib -> Cell (list) -> View (scalar, current access) -> Pin (list) and
    Group (list) -> Pin (list): a Pin is reached from View by two paths."""
    schema = Schema(
        name="t",
        description="",
        namespace="t",
        version="1.0.0",
        classes=[
            Klass(
                name="Lib",
                description="Lib",
                fields=[Field(name="cells", description="x", type="Cell", is_list=True, is_child=True)],
            ),
            Klass(
                name="Cell",
                description="Cell",
                fields=[
                    Field(name="lib", description="x", type="Lib", parent="cells"),
                    Field(name="view", description="x", type="View", is_child=True, is_optional=True),
                ],
            ),
            Klass(
                name="View",
                description="View",
                has_current_access=True,
                fields=[
                    Field(name="cell", description="x", type="Cell", parent="view"),
                    Field(name="groups", description="x", type="Group", is_list=True, is_child=True),
                    Field(name="pins", description="x", type="Pin", is_list=True, is_child=True),
                ],
            ),
            Klass(
                name="Group",
                description="Group",
                fields=[
                    Field(name="view", description="x", type="View", parent="groups"),
                    Field(name="pins", description="x", type="Pin", is_list=True, is_child=True),
                ],
            ),
            Klass(
                name="Pin",
                description="Pin",
                fields=[
                    Field(name="view", description="x", type="View", parent="pins"),
                    Field(name="group", description="x", type="Group", parent="pins"),
                ],
            ),
        ],
    )
    schema.link()
    return schema


class TestTclScope(unittest.TestCase):
    def setUp(self):
        self.schema = _schema()
        self.anchors = [k for k in self.schema.classes if k.has_current_access]

    def scope(self, name):
        return compute_search_scope(self.schema.get_klass(name), self.anchors)

    @staticmethod
    def names(fields):
        return [f"{f._klass.name}.{f.name}" for f in fields]

    def test_current_access_class_is_its_own_scope(self):
        case = self.scope("View").default_case
        self.assertIsInstance(case, SelfScope)
        self.assertIn("handle->current_view_id", render_default_scope_cpp(self.scope("View")))

    def test_descendant_unions_every_path_down_from_the_anchor(self):
        scope = self.scope("Pin")
        self.assertIsInstance(scope.default_case, DescendantScope)
        self.assertEqual(scope.default_case.anchor.name, "View")
        self.assertCountEqual(
            [self.names(p) for p in scope.default_case.paths], [["View.pins"], ["View.groups", "Group.pins"]]
        )
        cpp = render_default_scope_cpp(scope)
        self.assertIn("handle->root.get_view_pins(handle->current_view_id)", cpp)
        self.assertIn("for (const le::GroupId hop0 : handle->root.get_view_groups(handle->current_view_id))", cpp)
        self.assertIn("handle->root.get_group_pins(hop0)", cpp)

    def test_ancestor_takes_the_siblings_of_the_one_holding_the_anchor(self):
        scope = self.scope("Cell")
        case = scope.default_case
        self.assertIsInstance(case, AncestorScope)
        self.assertEqual(case.anchor.name, "View")
        self.assertEqual(self.names(case.up_chain), ["View.cell"])
        self.assertEqual(self.names([case.sibling_param.parent_field, case.sibling_param.sibling_field]), ["Cell.lib", "Lib.cells"])
        cpp = render_default_scope_cpp(scope)
        self.assertIn("handle->root.get_lib_cells(up0_data->lib)", cpp)
        # With no current View, every Cell.
        self.assertIn("candidates = handle->root.get_cell_ids();", cpp)

    def test_parentless_ancestor_is_a_flat_scan(self):
        # Lib contains View, but has no parent to take siblings from.
        scope = self.scope("Lib")
        self.assertIsInstance(scope.default_case, RootScope)
        self.assertEqual(scope.of_params, [])
        self.assertEqual(render_default_scope_cpp(scope), "candidates = handle->root.get_lib_ids();")

    def test_one_of_param_per_parent_field(self):
        scope = self.scope("Pin")
        self.assertEqual(
            [(p.parent_field.name, f"{p.parent_klass.name}.{p.sibling_field.name}") for p in scope.of_params],
            [("view", "View.pins"), ("group", "Group.pins")],
        )
        cpp = render_of_check_cpp(scope)
        self.assertIn("from_c(of_view)", cpp)
        self.assertIn("from_c(of_group)", cpp)
        self.assertIn("handle->root.get_view_pins(handle->current_view_id)", cpp)


if __name__ == "__main__":
    unittest.main()
