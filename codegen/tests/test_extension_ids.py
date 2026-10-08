import copy
import logging
import tempfile
import unittest
from pathlib import Path

from codegen import tcl_generator
from examples import eda


class TestExtensionIds(unittest.TestCase):
    def setUp(self):
        # generate() links the schema in place; keep the shared example pristine.
        self.schema = copy.deepcopy(eda.schema)
        self.readable = [k for k in self.schema.classes if not k.is_enum and k.has_pool and k.is_tcl_readable()]
        self.current = self.readable[0]
        self.current.has_current_access = True
        self.out = tempfile.TemporaryDirectory()
        self.assertEqual(tcl_generator.generate(self.schema, self.out.name, logging.getLogger("test")), 0)

    def tearDown(self):
        self.out.cleanup()

    def read(self, name):
        return (Path(self.out.name) / "api" / name).read_text()

    def test_a_conversion_pair_per_class_with_a_c_id(self):
        text = self.read("id_conversions.hpp")
        for klass in self.readable:
            self.assertIn(f"inline Le{klass.name}Id to_c({klass.name}Id id)", text)
            self.assertIn(f"inline {klass.name}Id from_c(Le{klass.name}Id id)", text)
        self.assertEqual(text.count("inline "), 2 * len(self.readable))

    def test_accessors_only_for_classes_with_a_current_instance(self):
        snake = self.current.to_snake_case()
        decls = self.read("extension_current_decls.inc")
        self.assertIn(f"{self.current.name}Id current_{snake}() const;", decls)
        self.assertEqual(decls.count(" const;"), 1)
        defs = self.read("extension_current_defs.inc")
        for owner in ("ReadView", "WriteView", "ExtensionContext"):
            self.assertIn(f"{self.current.name}Id {owner}::current_{snake}() const", defs)


if __name__ == "__main__":
    unittest.main()
