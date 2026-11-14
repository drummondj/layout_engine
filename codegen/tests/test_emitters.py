import copy
import unittest

from codegen.emitters import c_api, edit_ops, root, tcl_procs
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


class TestTclProcEmitters(unittest.TestCase):
    """The generated create_/update_/delete_<type> procs' text, for the
    example EDA schema's Instance: a required parent, a required name and an
    optional plain reference to a Module."""

    def setUp(self):
        self.schema = copy.deepcopy(eda.schema)
        self.schema.link()
        self.instance = self.schema.get_klass("Instance")

    def test_usage_marks_optional_flags(self):
        self.assertEqual(tcl_procs.create_tcl_usage(self.instance),
                         "create_instance -module <token> \\[-reference <token>\\] -name <str> \\[-help\\] - An instance")
        self.assertEqual(tcl_procs.update_tcl_usage(self.instance),
                         "update_instance <id> \\[-module <token>\\] \\[-reference <token>\\] \\[-name <str>\\] \\[-help\\] - An instance")
        self.assertEqual(tcl_procs.delete_tcl_usage(self.instance), "delete_instance <ids> \\[-help\\] - An instance.")

    def test_flags_defaults_and_required(self):
        self.assertEqual(tcl_procs.create_tcl_flag_defaults(self.instance), "-module {} -reference {} -name {}")
        self.assertEqual(tcl_procs.update_tcl_flag_defaults(self.instance), "-module {} -reference {} -name {}")
        self.assertEqual(tcl_procs.create_tcl_required_flags(self.instance), "-module -name")

    def test_update_passes_a_has_flag_for_each_optional_reference(self):
        self.assertEqual(tcl_procs.create_tcl_call_args(self.instance), "$opts(-module) $opts(-reference) $opts(-name)")
        self.assertEqual(
            tcl_procs.update_tcl_call_args(self.instance),
            "[expr {$opts(-module) ne {} ? 1 : 0}] $opts(-module) [expr {$opts(-reference) ne {} ? 1 : 0}] $opts(-reference) $opts(-name)",
        )

    def test_a_reference_token_is_checked_before_the_call(self):
        checks = tcl_procs.tcl_reference_token_checks(self.instance, "create_instance")
        self.assertIn("![module_token_resolves $opts(-reference)]", checks)
        self.assertIn('error "create_instance: unknown module', checks)


class TestTheModelHoldsNoEmitters(unittest.TestCase):
    def test_klass_has_no_body_emitters(self):
        for name in ("create_api_body", "update_api_body", "delete_api_body", "apply_snapshot_body", "create_op_body",
                     "update_op_body", "delete_op_body", "delete_batch_op_body", "update_root_body"):
            self.assertFalse(hasattr(Klass, name), name)

    def test_neither_class_has_the_tcl_proc_emitters(self):
        for name in ("cmd_tcl_preamble", "create_tcl_usage", "update_tcl_usage", "delete_tcl_usage", "create_tcl_help_options",
                     "update_tcl_help_options", "delete_tcl_help_options", "create_tcl_flag_defaults", "update_tcl_flag_defaults",
                     "create_tcl_required_flags", "create_tcl_call_args", "update_tcl_call_args", "create_tcl_aliases",
                     "create_tcl_current_defaults", "tcl_help_attributes", "tcl_reference_token_checks"):
            self.assertFalse(hasattr(Klass, name), name)
        for name in ("cmd_tcl_preamble", "_list_compound_tcl_preamble", "cmd_tcl_call_args", "tcl_help_type_label"):
            self.assertFalse(hasattr(Field, name), name)


if __name__ == "__main__":
    unittest.main()
