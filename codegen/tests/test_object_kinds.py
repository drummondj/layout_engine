import copy
import logging
import tempfile
import unittest
from pathlib import Path

from codegen import tcl_generator
from examples import eda


class TestObjectKinds(unittest.TestCase):
    def setUp(self):
        # generate() links the schema in place; keep the shared example pristine.
        self.schema = copy.deepcopy(eda.schema)
        self.out = tempfile.TemporaryDirectory()
        self.assertEqual(tcl_generator.generate(self.schema, self.out.name, logging.getLogger("test")), 0)
        self.readable = [k for k in self.schema.classes if k.is_tcl_readable()]

    def tearDown(self):
        self.out.cleanup()

    def read(self, name):
        return (Path(self.out.name) / "api" / name).read_text()

    def test_dispatch_covers_every_kind_and_parent_field(self):
        text = self.read("object_dispatch.inc")
        for index, klass in enumerate(self.readable):
            snake = klass.to_snake_case()
            self.assertIn(f"return build_{snake}_properties(root, std::get<{index}>(id));", text)
            self.assertIn(f"static_assert(std::is_same_v<std::variant_alternative_t<{index}, le::ObjectId>, le::{klass.name}Id>);", text)
            self.assertIn(f'{{"{snake}", {"true" if klass.tcl_friendly_id_field() else "false"}}},', text)
            for field in klass.get_parent_fields():
                self.assertIn(f"if (object->{field.accessor}.valid())\n            return le::ObjectId(object->{field.accessor});", text)


if __name__ == "__main__":
    unittest.main()
