import unittest

from codegen import validation, generator


class TestValidation(unittest.TestCase):
    @unittest.skip("expects upstream cmg's validation errors; the fork's rules have since changed")
    def test_failures(self):
        schema = generator.schema_loader("tests/schemas/validation_failures.py")
        errors = validation.SchemaRuleSet().validate(schema)
        for error in errors:
            print(error.message)
        self.assertEqual(len(errors), 11)
