import unittest

from codegen import edit_op_tests
from codegen.schema import Field, Klass, Schema


def _schema(**overrides):
    """A Part has a name, a kind, a corner and a list of rects; `overrides`
    replaces a field's keyword arguments by field name."""
    fields = {
        "name": dict(type="str", example="p1"),
        "kind": dict(type="Kind", example="SOFT"),
        "corner": dict(type="Point", example={"x": 1, "y": 2}, is_optional=True),
        "rects": dict(type="Rect", example={"ll": {"x": 0, "y": 0}, "ur": {"x": 4, "y": 2}}, is_list=True),
        "count": dict(type="int", example=3, default=1),
    }
    for name, kwargs in overrides.items():
        fields[name] = kwargs
    schema = Schema(
        name="t",
        description="",
        namespace="t",
        version="1.0.0",
        classes=[
            Klass(
                name="Kind",
                description="Kind",
                is_enum=True,
                has_pool=False,
                fields=[
                    Field(name="HARD", description="Hard", type="int", value=0),
                    Field(name="SOFT", description="Soft", type="int", value=1),
                ],
            ),
            Klass(
                name="Point",
                description="Point",
                has_pool=False,
                fields=[
                    Field(name="x", description="x", type="int", example=5),
                    Field(name="y", description="y", type="int", example=6),
                ],
            ),
            Klass(
                name="Rect",
                description="Rect",
                has_pool=False,
                fields=[
                    Field(name="ll", description="Lower left", type="Point"),
                    Field(name="ur", description="Upper right", type="Point"),
                ],
            ),
            Klass(
                name="Top",
                description="Root",
                fields=[Field(name="parts", description="Parts", type="Part", is_list=True, is_child=True)],
            ),
            Klass(
                name="Part",
                description="Part",
                fields=[Field(name="top", description="Owner", type="Top", parent="parts")]
                + [Field(name=name, description=name, **kwargs) for name, kwargs in fields.items()],
            ),
        ],
    )
    schema.link()
    return [k for k in schema.classes if k.has_pool and not k.is_enum]


class TestExampleErrors(unittest.TestCase):
    def test_complete_examples_pass(self):
        self.assertEqual(edit_op_tests.example_errors(_schema()), [])

    def test_a_field_without_an_example_is_refused(self):
        errors = edit_op_tests.example_errors(_schema(count=dict(type="int", default=1)))
        self.assertEqual(errors, ["Part.count has no example"])

    def test_an_example_must_be_a_value_of_its_enum(self):
        errors = edit_op_tests.example_errors(_schema(kind=dict(type="Kind", example="MEDIUM")))
        self.assertEqual(errors, ["Part.kind: example 'MEDIUM' is not a Kind value (HARD, SOFT)"])

    def test_an_example_equal_to_the_unset_value_is_refused(self):
        self.assertEqual(
            edit_op_tests.example_errors(_schema(count=dict(type="int", example=1, default=1))),
            ["Part.count: example 1 is its default"],
        )
        self.assertEqual(
            edit_op_tests.example_errors(_schema(kind=dict(type="Kind", example="HARD"))),
            ["Part.kind: example 'HARD' is the value it has when left out"],
        )

    def test_a_struct_example_names_only_its_fields(self):
        errors = edit_op_tests.example_errors(_schema(corner=dict(type="Point", example={"x": 1, "z": 2}, is_optional=True)))
        self.assertEqual(errors, ["Part.corner: Point has no field 'z'"])

    def test_a_struct_example_may_leave_out_fields_that_have_their_own(self):
        errors = edit_op_tests.example_errors(
            _schema(rects=dict(type="Rect", example={"ll": {"x": 0, "y": 0}}, is_list=True))
        )
        self.assertEqual(errors, [])


class TestGeneratedSource(unittest.TestCase):
    def setUp(self):
        self.source = edit_op_tests.generate_test_source(_schema())

    def test_a_required_field_is_created_with_its_example_and_updated_to_another_value(self):
        self.assertIn('data.name = std::string("p1");', self.source)
        self.assertIn('const auto value = std::string("p1_2");', self.source)
        self.assertIn("data.kind = le::Kind::SOFT;", self.source)
        self.assertIn("const auto value = le::Kind::HARD;", self.source)

    def test_an_optional_field_is_updated_to_its_example(self):
        self.assertIn("const auto value = le::Point{.x = 1, .y = 2};", self.source)
        self.assertNotIn("data.corner", self.source)

    def test_a_list_field_is_updated_to_one_element(self):
        self.assertIn(
            "const auto value = std::vector<le::Rect>{le::Rect{.ll = le::Point{.x = 0, .y = 0}, .ur = le::Point{.x = 4, .y = 2}}};",
            self.source,
        )

    def test_each_class_gets_a_lifecycle_test_and_one_per_field(self):
        for name in ("Part_CreateDeleteUndoRedo", "Part_Update_top", "Part_Update_name", "Part_Update_kind",
                     "Part_Update_corner", "Part_Update_rects", "Part_Update_count", "Top_CreateDeleteUndoRedo"):
            self.assertIn(f"TEST(GeneratedEditOps, {name})", self.source)


if __name__ == "__main__":
    unittest.main()
