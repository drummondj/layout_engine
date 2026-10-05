import logging
import tempfile
import unittest
from pathlib import Path

from codegen.render_generator import generate, validate_purposes
from codegen.schema import Purpose, Schema


def _schema(purposes):
    return Schema(name="s", description="", namespace="le", version="1.0.0", classes=[], purposes=purposes)


class TestRender(unittest.TestCase):
    def test_valid_purposes_pass(self):
        schema = _schema([Purpose("ROUTE", "route", "Routed wires"), Purpose("ROW", "row", "Rows", visible_by_default=False)])
        self.assertEqual(validate_purposes(schema), [])

    def test_problems_are_reported(self):
        schema = _schema(
            [
                Purpose("route", "route", "lower-case name"),
                Purpose("ROW", "Row", "capitalised label"),
                Purpose("ROW", "rowAgain", "duplicate name"),
                Purpose("DEBUG", "route", "duplicate label"),
                Purpose("EMPTY", "empty", " "),
            ]
        )
        errors = "\n".join(validate_purposes(schema))
        self.assertIn("UPPER_SNAKE_CASE", errors)
        self.assertIn("camelCase", errors)
        self.assertIn("declared twice", errors)
        self.assertIn("used twice", errors)
        self.assertIn("one non-empty line", errors)
        self.assertEqual(validate_purposes(_schema([])), ["the schema declares no purposes"])

    def test_generates_enum_and_table_in_declaration_order(self):
        schema = _schema(
            [
                Purpose("ROUTE", "route", "Routed wires", has_selectable_objects=True),
                Purpose("GRID", "grid", "A grid", visible_by_default=False, selectable_by_default=False),
            ]
        )
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            text = (Path(out) / "view_layer_purpose.hpp").read_text()
        self.assertLess(text.index("ROUTE, // Routed wires"), text.index("GRID, // A grid"))
        self.assertIn("kViewLayerPurposeCount = 2;", text)
        self.assertIn('{ViewLayerPurpose::ROUTE, "route", true, true, true},', text)
        self.assertIn('{ViewLayerPurpose::GRID, "grid", false, false, false},', text)

    def test_invalid_purposes_fail_generation(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(_schema([]), out, logging.getLogger("test")), 1)


if __name__ == "__main__":
    unittest.main()
