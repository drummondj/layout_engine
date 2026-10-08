import copy
import logging
import re
import tempfile
import unittest
from pathlib import Path

from codegen import tcl_generator
from examples import eda


class TestEditOps(unittest.TestCase):
    """The C API's create/update/delete are shims over le::edit's ops, which
    WriteView exposes to extensions."""

    def setUp(self):
        # generate() links the schema in place; keep the shared example pristine.
        self.schema = copy.deepcopy(eda.schema)
        self.out = tempfile.TemporaryDirectory()
        self.assertEqual(tcl_generator.generate(self.schema, self.out.name, logging.getLogger("test")), 0)
        self.readable = [k for k in self.schema.classes if not k.is_enum and k.has_pool and k.is_tcl_readable()]

    def tearDown(self):
        self.out.cleanup()

    def read(self, name):
        return (Path(self.out.name) / "api" / name).read_text()

    def test_changes_hold_exactly_what_le_update_can_change(self):
        text = self.read("edit_types.hpp")
        for klass in self.readable:
            body = re.search(rf"struct {klass.name}Changes\n    {{\n(.*?)    }};", text, re.S)
            self.assertIsNotNone(body, klass.name)
            members = re.findall(r"std::optional<.*> (\w+);", body.group(1))
            parents = klass.get_parent_fields()
            expected = ([parents[0].name] if len(parents) == 1 else [])
            expected += [f.name for f in klass.get_reference_create_fields()]
            expected += [f.name for f in klass.get_create_fields()]
            self.assertEqual(members, expected, klass.name)

    def test_each_c_body_calls_its_op(self):
        text = self.read("property_accessors_public.inc")
        for klass in self.readable:
            snake = klass.to_snake_case()
            for op in ("create", "update", "delete"):
                self.assertIn(f"le::edit::{op}_{snake}(*handle", text)

    def test_ops_record_undo_and_the_c_bodies_do_not(self):
        self.assertNotIn("command_history", self.read("property_accessors_public.inc"))
        ops = self.read("edit_ops_defs.inc")
        for klass in self.readable:
            snake = klass.to_snake_case()
            self.assertIn(f"std::expected<{klass.name}Id, std::string> create_{snake}(LeHandle &handle, {klass.name}Data data)", ops)
        self.assertGreaterEqual(ops.count("record_create<"), len(self.readable))

    def test_write_view_has_each_op(self):
        decls = self.read("extension_edit_decls.inc")
        defs = self.read("extension_edit_defs.inc")
        for klass in self.readable:
            snake = klass.to_snake_case()
            for op in ("create", "update", "delete"):
                self.assertIn(f" {op}_{snake}(", decls)
                self.assertIn(f"WriteView::{op}_{snake}(", defs)


if __name__ == "__main__":
    unittest.main()
