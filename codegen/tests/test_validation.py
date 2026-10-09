import unittest

from codegen import validation, generator


class TestValidation(unittest.TestCase):
    def test_failures(self):
        schema = generator.schema_loader("tests/schemas/validation_failures.py")
        errors = [error.message for error in validation.SchemaRuleSet().validate(schema)]
        self.assertEqual(
            sorted(errors),
            sorted(
                [
                    "Field incorrect_primitive in klass Top has an invalid type string",
                    "Field incorrect_class in klass Top has an invalid type InvalidClass",
                    "Field root in klass BadChild has an invalid type Rootz",
                    "Field incorrect_example in klass Top has an example 1 that does not match the type str",
                    "Field root2 in klass BadChild has a parent field childrenz that does not exist in klass Top",
                    "Field incorrect_default in klass Top has a default value 1 that does not match the type str",
                    "Klass BadChild is not unique",
                    "Field name in klass BadChild is not unique",
                    "Klass Root is reserved for the generated container class",
                    "Field incorrect_parent in klass Top is a child field but does not have a parent field in klass NoParent",
                ]
            ),
        )

    def test_examples_are_valid(self):
        from examples import eda, solar_system

        for schema in (eda.schema, solar_system.schema):
            self.assertEqual([e.message for e in validation.SchemaRuleSet().validate(schema)], [], schema.name)


if __name__ == "__main__":
    unittest.main()
