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

    def test_one_kind_per_readable_class_in_schema_order(self):
        text = self.read("object_kinds.inc")
        for i, klass in enumerate(self.readable):
            self.assertIn(f"LE_OBJECT_KIND_{klass.to_snake_case().upper()} = {i},", text)

    def test_dispatch_covers_every_kind_and_parent_field(self):
        text = self.read("object_dispatch.inc")
        for index, klass in enumerate(self.readable):
            snake = klass.to_snake_case()
            self.assertIn(f"return build_{snake}_properties(root, std::get<{index}>(id));", text)
            self.assertIn(f"le::ObjectId(std::in_place_index<{index}>, id_from_ref<le::{klass.name}Id>(ref));", text)
            self.assertIn(f"static_assert(LE_OBJECT_KIND_{snake.upper()} == {index} && ", text)
            self.assertIn(f'{{"{snake}", {"true" if klass.tcl_friendly_id_field() else "false"}}},', text)
            for field in klass.get_parent_fields():
                self.assertIn(f"if (object->{field.accessor}.valid())\n            return le::ObjectId(object->{field.accessor});", text)


if __name__ == "__main__":
    unittest.main()
