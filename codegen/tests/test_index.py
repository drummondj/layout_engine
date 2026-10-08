"""Root::Index declares only the maps the generated Root uses."""

import logging
import re
import tempfile
import unittest
from pathlib import Path

from codegen.generator import generate
from codegen.schema import Field, Klass, Schema


def _schema():
    """Every kind of relationship: list and singular children, owners, a
    plain reference, and global and per-parent name indexes."""
    return Schema(
        name="t",
        description="",
        namespace="t",
        version="1.0.0",
        classes=[
            Klass(
                name="Lib",
                description="Lib",
                fields=[
                    Field(name="name", description="Name", type="str", example="a", index=True),
                    Field(name="cells", description="Cells", type="Cell", is_list=True, is_child=True),
                    Field(name="items", description="Items", type="Item", is_list=True, is_child=True),
                ],
            ),
            Klass(
                name="Cell",
                description="Cell",
                fields=[
                    Field(name="lib", description="Lib", type="Lib", parent="cells"),
                    Field(name="name", description="Name", type="str", example="c", index=True, unique_per_parent=True),
                    Field(name="outline", description="Outline", type="Item", is_child=True),
                    Field(name="peer", description="A plain reference", type="Cell"),
                ],
            ),
            Klass(
                name="Item",
                description="Item",
                fields=[
                    Field(name="lib", description="Owning lib", type="Lib", parent="items", owner=True),
                    Field(name="cell", description="Owning cell", type="Cell", parent="outline", owner=True),
                    Field(name="size", description="Size", type="int", example=1),
                ],
            ),
        ],
    )


class TestIndex(unittest.TestCase):
    def test_every_declared_index_is_used_by_root(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(_schema(), out, logging.getLogger("test")), 0)
            index = (Path(out) / "index.hpp").read_text()
            root = (Path(out) / "root.hpp").read_text()
        members = re.findall(r"std::unordered_map<.*> (\w+);", index)
        self.assertEqual(
            sorted(members),
            ["cell_by_name", "cell_outline", "lib_by_name", "lib_cells", "lib_items"],
            "child lists, singular children and name lookups only - no child-to-parent or plain-reference maps",
        )
        for member in members:
            self.assertIn(f"index_.{member}", root, f"Index::{member} is declared but Root never uses it")


if __name__ == "__main__":
    unittest.main()
