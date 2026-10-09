"""A Klass with compact_lists=True stores its list fields as CompactVectors."""

import logging
import tempfile
import unittest
from pathlib import Path

from codegen.generator import generate
from codegen.schema import Field, Klass, Schema


class TestCompactLists(unittest.TestCase):
    def test_only_classes_that_opt_in_use_compact_vectors(self):
        def tags():
            return Field(name="tags", description="Tags", type="str", example="t", is_list=True)

        schema = Schema(
            name="t",
            description="",
            namespace="t",
            version="1.0.0",
            classes=[
                Klass(name="Plain", description="Plain", fields=[tags()]),
                Klass(name="Dense", description="Dense", compact_lists=True, fields=[tags()]),
            ],
        )
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            plain = (Path(out) / "plain.hpp").read_text()
            dense = (Path(out) / "dense.hpp").read_text()
            self.assertTrue((Path(out) / "compact_vector.hpp").is_file())
        self.assertIn("CompactVector<std::string> tags", dense)
        self.assertIn("std::vector<std::string> tags", plain)


if __name__ == "__main__":
    unittest.main()
