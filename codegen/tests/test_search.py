import copy
import logging
import tempfile
import unittest
from pathlib import Path

from codegen import tcl_generator
from codegen.tcl_scope import compute_search_scope
from examples import eda


class TestSearch(unittest.TestCase):
    """The C++ search (search_defs.inc, the views' get_<type>) and the C
    search shims over it."""

    def setUp(self):
        # generate() links the schema in place; keep the shared example pristine.
        self.schema = copy.deepcopy(eda.schema)
        self.out = tempfile.TemporaryDirectory()
        self.assertEqual(tcl_generator.generate(self.schema, self.out.name, logging.getLogger("test")), 0)
        self.readable = [k for k in self.schema.classes if k.is_tcl_readable()]
        current = [k for k in self.schema.classes if k.has_current_access]
        self.scopes = {k.name: compute_search_scope(k, current) for k in self.readable}

    def tearDown(self):
        self.out.cleanup()

    def read(self, name):
        return (Path(self.out.name) / "api" / name).read_text()

    def test_a_query_per_class_with_its_of_parents(self):
        text = self.read("edit_types.hpp")
        for klass in self.readable:
            self.assertIn(f"struct {klass.name}Query\n", text)
            for op in self.scopes[klass.name].of_params:
                self.assertIn(f"{op.parent_klass.name}Id of_{op.parent_field.name};", text)

    def test_every_class_is_an_object_id_alternative(self):
        text = self.read("edit_types.hpp")
        for klass in self.readable:
            self.assertIn(f"        {klass.name}Id", text.split("using ObjectId = std::variant<")[1].split(">;")[0])

    def test_each_search_reads_its_query_and_reports_bad_filters(self):
        defs = self.read("search_defs.inc")
        views = self.read("view_search_decls.inc")
        for klass in self.readable:
            plural = klass.tcl_plural_snake_case()
            self.assertIn(f"get_{plural}(const LeHandle &handle_ref, const {klass.name}Query &query)", defs)
            self.assertIn(f"get_{plural}(const {klass.name}Query &query = {{}}) const;", views)
            for op in self.scopes[klass.name].of_params:
                self.assertIn(f"= query.of_{op.parent_field.name};", defs)
        self.assertIn("return std::unexpected(expr.error());", defs)
        self.assertNotIn("from_c(", defs)


if __name__ == "__main__":
    unittest.main()
