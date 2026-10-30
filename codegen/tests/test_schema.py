import unittest
from codegen.schema import Schema, Klass, Field, to_snake_case, to_camel_case


class TestSchema(unittest.TestCase):

    def setUp(self):
        self.field1 = Field(
            name="field_one", description="Field One", type="str", example="example"
        )
        self.field2 = Field(
            name="field_two", description="Field Two", type="int", example=42
        )
        self.klass = Klass(
            name="TestKlass",
            description="A test class",
            fields=[self.field1, self.field2],
        )
        self.schema = Schema(
            name="TestSchema",
            description="A test schema",
            namespace="test",
            classes=[self.klass],
            version="something",
        )
        self.schema.link()
        self.schema.set_output_dir("output")

    def test_to_snake_case(self):
        self.assertEqual(to_snake_case("camelCase"), "camel_case")
        self.assertEqual(to_snake_case("PascalCase"), "pascal_case")

    def test_to_camel_case(self):
        self.assertEqual(to_camel_case("snake_case"), "snakeCase")
        self.assertEqual(to_camel_case("snake_case", upper_first=True), "SnakeCase")

    def test_schema_link(self):
        self.schema.link()
        self.assertEqual(self.klass._schema, self.schema)
        self.assertEqual(self.field1._klass, self.klass)

    def test_get_klass(self):
        self.assertEqual(self.schema.get_klass("TestKlass"), self.klass)
        with self.assertRaises(ValueError):
            self.schema.get_klass("NonExistentKlass")

    def test_get_field(self):
        self.schema.link()
        self.assertEqual(self.schema.get_field("TestKlass", "field_one"), self.field1)
        with self.assertRaises(ValueError):
            self.schema.get_field("TestKlass", "nonExistentField")

    def test_get_cmakelists_src(self):
        self.assertEqual(self.schema.get_cmakelists_src(), '"test_klass.cpp"')

    def test_get_lcov_src(self):
        self.assertTrue(
            "test_klass.cpp" in self.schema.get_lcov_src(),
        )

    def test_get_test_includes(self):
        self.assertEqual(self.schema.get_test_includes(), ['#include "test_klass.hpp"'])

    def test_klass_to_snake_case(self):
        self.assertEqual(self.klass.to_snake_case(), "test_klass")

    def test_field_to_camel_case(self):
        self.assertEqual(self.field1.to_camel_case(), "fieldOne")
        self.assertEqual(self.field1.to_camel_case(upper_first=True), "FieldOne")

    def test_field_get_cpp_type(self):
        self.assertEqual(self.field1.get_cpp_type(), "std::string")
        self.assertEqual(self.field2.get_cpp_type(), "int")

    def test_field_is_reference(self):
        self.schema.link()
        self.assertFalse(self.field1.is_reference())
        self.field1.type = "TestKlass"
        self.assertTrue(self.field1.is_reference())

    def test_field_get_example(self):
        self.assertEqual(self.field1.get_example(), 'std::string("example")')
        self.assertEqual(self.field2.get_example(), 42)


if __name__ == "__main__":
    unittest.main()


class TestCompoundCreateField(unittest.TestCase):
    """create_<type>'s initializer for an embedded-struct field (Point)."""

    def setUp(self):
        point = Klass(
            name="Point",
            description="A point",
            has_pool=False,
            fields=[
                Field(name="x", description="X", type="int", example=0),
                Field(name="y", description="Y", type="int", example=0),
            ],
        )
        self.klass = Klass(
            name="Marker",
            description="A marker",
            fields=[
                Field(name="location", description="Required location", type="Point"),
                Field(name="origin", description="Optional origin", type="Point", is_optional=True),
            ],
        )
        self.schema = Schema(name="t", description="", namespace="t", classes=[point, self.klass], version="1.0.0")
        self.schema.link()

    def test_non_optional_field_defaults_to_the_zero_value(self):
        field = self.schema.get_field("Marker", "location")
        self.assertTrue(field.is_compound_create_field())
        self.assertEqual(
            field.create_struct_init_expr(),
            "has_location ? le::Point{.x = location_x, .y = location_y} : le::Point{}",
        )

    def test_optional_field_stays_optional(self):
        field = self.schema.get_field("Marker", "origin")
        self.assertEqual(
            field.create_struct_init_expr(),
            "has_origin ? std::optional<le::Point>(le::Point{.x = origin_x, .y = origin_y}) : std::nullopt",
        )


class TestSmallUnsignedCreateFields(unittest.TestCase):
    """An unsigned char/short create field crosses the C API as an int32_t,
    range-checked, and narrows back on the way in."""

    def setUp(self):
        self.klass = Klass(
            name="Marker",
            description="",
            fields=[
                Field(name="index", description="", type="unsigned char", is_optional=True),
                Field(name="count", description="", type="unsigned short"),
            ],
        )
        self.schema = Schema(name="t", description="", namespace="t", classes=[self.klass], version="1.0.0")
        self.schema.link()

    def test_crosses_the_c_api_as_int32(self):
        field = self.schema.get_field("Marker", "index")
        self.assertEqual(field.create_c_type(), "int32_t")
        self.assertEqual(field.root_value_cpp_type(), "unsigned char")
        self.assertEqual(field.tcl_help_type_label(), "int")
        self.assertTrue(field.create_needs_has_flag("create"))

    def test_narrows_on_the_way_in(self):
        self.assertEqual(
            self.schema.get_field("Marker", "index").create_struct_init_expr(),
            "has_index ? std::optional<unsigned char>(static_cast<unsigned char>(index)) : std::nullopt",
        )
        self.assertEqual(self.schema.get_field("Marker", "count").create_struct_init_expr(), "static_cast<unsigned short>(count)")

    def test_create_and_update_reject_an_out_of_range_value(self):
        create = self.klass.create_api_body()
        self.assertIn("if (has_index && (index < 0 || index > 255))", create)
        self.assertIn("if (count < 0 || count > 65535)", create)
        update = self.klass.update_api_body()
        self.assertIn("if (has_index && (index < 0 || index > 255))", update)
        self.assertIn("if (has_count && (count < 0 || count > 65535))", update)

    def test_unset_default_of_a_multi_word_type_is_a_cast(self):
        self.assertEqual(self.schema.get_field("Marker", "index")._optional_default_cpp(), "static_cast<unsigned char>(0)")


class TestUpdateCheck(unittest.TestCase):
    """Klass.create_check/update_check: the generated edit op asks a hand-written
    function before changing anything."""

    def test_update_op_calls_the_check_before_updating(self):
        klass = Klass(name="Marker", description="", update_check="check_marker_update",
                      fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        body = klass.update_op_body()
        check = body.index("if (const std::optional<std::string> refused = check_marker_update(root, before, changes))")
        self.assertIn("return std::unexpected(*refused);", body)
        self.assertLess(check, body.index("root.update_marker("))

    def test_create_op_calls_the_check_before_creating(self):
        klass = Klass(name="Marker", description="", create_check="check_marker_create",
                      fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        body = klass.create_op_body()
        check = body.index("if (const std::optional<std::string> refused = check_marker_create(root, data))")
        self.assertLess(check, body.index("root.create_marker("))

    def test_no_check_by_default(self):
        klass = Klass(name="Marker", description="", fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        self.assertNotIn("refused", klass.update_op_body())
        self.assertNotIn("refused", klass.create_op_body())
