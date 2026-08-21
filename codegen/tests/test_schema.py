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
