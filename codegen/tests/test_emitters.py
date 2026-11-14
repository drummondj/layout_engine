import copy
import unittest

from codegen.emitters import c_api, edit_ops, root
from codegen.schema import Field, Klass, Schema
from examples import eda


class TestUpdateCheck(unittest.TestCase):
    """Klass.create_check/update_check: the generated edit op asks a hand-written
    function before changing anything."""

    def test_update_op_calls_the_check_before_updating(self):
        klass = Klass(name="Marker", description="", update_check="check_marker_update",
                      fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        body = edit_ops.update_op_body(klass)
        check = body.index("if (const std::optional<std::string> refused = check_marker_update(root, before, changes))")
        self.assertIn("return std::unexpected(*refused);", body)
        self.assertLess(check, body.index("root.update_marker("))

    def test_create_op_calls_the_check_before_creating(self):
        klass = Klass(name="Marker", description="", create_check="check_marker_create",
                      fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        body = edit_ops.create_op_body(klass)
        check = body.index("if (const std::optional<std::string> refused = check_marker_create(root, data))")
        self.assertLess(check, body.index("root.create_marker("))

    def test_no_check_by_default(self):
        klass = Klass(name="Marker", description="", fields=[Field(name="size", description="", type="int")])
        Schema(name="t", description="", namespace="t", classes=[klass], version="1.0.0").link()
        self.assertNotIn("refused", edit_ops.update_op_body(klass))
        self.assertNotIn("refused", edit_ops.create_op_body(klass))


class TestEmittersOverAHierarchy(unittest.TestCase):
    """The emitters over the example EDA schema: Top owns Designs, which own
    Modules, which own Instances."""

    def setUp(self):
        self.schema = copy.deepcopy(eda.schema)
        self.schema.link()
        self.klass = {k.name: k for k in self.schema.classes}

    def test_delete_op_cascades_deepest_first(self):
        body = edit_ops.delete_op_body(self.klass["Design"])
        instances = body.index("delete_object(handle.root, txn, *child);")
        modules = body.index("for (auto child = casc0_modules_ids.rbegin()")
        design = body.index("delete_object(handle.root, txn, design_id);")
        self.assertLess(instances, modules)
        self.assertLess(modules, design)
        self.assertIn('return std::unexpected("unknown id");', body)

    def test_delete_batch_op_retires_each_class_as_one_batch(self):
        body = edit_ops.delete_batch_op_body(self.klass["Design"])
        self.assertIn("std::span<const le::InstanceId>(casc1_instances_ids)", body)
        self.assertIn("std::span<const le::ModuleId>(casc0_modules_ids)", body)
        self.assertLess(body.index("casc1_instances_ids));"), body.index("casc0_modules_ids));"))
        self.assertIn("delete_objects(handle.root, txn, ids)", body)

    def test_a_leaf_class_deletes_without_a_cascade(self):
        self.assertNotIn("casc", edit_ops.delete_op_body(self.klass["Instance"]))

    def test_delete_api_takes_the_write_lock_and_calls_the_op(self):
        body = c_api.delete_api_body(self.klass["Design"])
        self.assertIn("HandleWriteLock lock(handle);", body)
        self.assertIn("le::edit::delete_design(*handle, from_c(id))", body)

    def test_update_root_reparents_through_the_child_lists(self):
        body = root.update_root_body(self.klass["Instance"])
        self.assertIn("const bool reparent = module.valid() && module != existing->module;", body)
        self.assertIn("index_.module_instances[module].push_back(id);", body)
        self.assertIn("if (reference) existing->reference = *reference;", body)
        self.assertIn("if (name) existing->name = std::move(*name);", body)

    def test_apply_snapshot_passes_every_field(self):
        self.assertEqual(
            c_api.apply_snapshot_body(self.klass["Instance"]).strip(),
            "return root.update_instance(id, data.module, std::optional<ModuleId>(data.reference), std::optional<std::string>(data.name));",
        )


class TestTheModelHoldsNoEmitters(unittest.TestCase):
    def test_klass_has_no_body_emitters(self):
        for name in ("create_api_body", "update_api_body", "delete_api_body", "apply_snapshot_body", "create_op_body",
                     "update_op_body", "delete_op_body", "delete_batch_op_body", "update_root_body"):
            self.assertFalse(hasattr(Klass, name), name)


if __name__ == "__main__":
    unittest.main()
