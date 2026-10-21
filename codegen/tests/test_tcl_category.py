import copy
import logging
import tempfile
import unittest
from pathlib import Path

from codegen import tcl_generator
from codegen.schema import TCL_CATEGORIES
from examples import eda


class TestTclCategory(unittest.TestCase):
    """A class's tcl_category puts its generated commands in that section of
    the Tcl command reference."""

    def setUp(self):
        self.schema = copy.deepcopy(eda.schema)
        self.klass = next(k for k in self.schema.classes if not k.is_enum and k.has_pool and k.is_tcl_readable())

    def generate(self):
        with tempfile.TemporaryDirectory() as out:
            with self.assertLogs("test", level="INFO") as logs:
                code = tcl_generator.generate(self.schema, out, logging.getLogger("test"))
            text = (Path(out) / "tcl" / "le_tcl_procs_generated.tcl").read_text() if code == 0 else ""
        return code, text, logs.output

    def test_the_category_reaches_every_generated_registration(self):
        self.klass.tcl_category = "Layout"
        self.assertEqual(self.klass.tcl_help_attributes(), " {category Layout}")
        code, text, _ = self.generate()
        self.assertEqual(code, 0)
        snake = self.klass.to_snake_case()
        registrations = [line for line in text.splitlines() if line.startswith(f"register_command_help create_{snake} ")
                         or line.startswith(f"register_command_help delete_{snake} ")]
        self.assertTrue(registrations)
        self.assertTrue(all(line.endswith(" {category Layout}") for line in registrations), registrations)

    def test_no_category_adds_nothing(self):
        self.klass.tcl_category = None
        self.assertEqual(self.klass.tcl_help_attributes(), "")

    def test_an_unknown_category_is_refused(self):
        self.klass.tcl_category = "Misc"
        code, _, logs = self.generate()
        self.assertEqual(code, 1)
        self.assertTrue(any(f"tcl_category 'Misc' must be one of {', '.join(TCL_CATEGORIES)}" in line for line in logs), logs)


if __name__ == "__main__":
    unittest.main()
