import json
import logging
import tempfile
import unittest
from pathlib import Path

from codegen.descriptor import build_descriptor
from codegen.generator import generate
from codegen.schema import Field, Klass, Schema
from codegen.validation import SchemaRuleSet


def _schema(item_fields=None):
    """Root owns Holders; an Item belongs to either the Root or a Holder."""
    item_fields = item_fields or [
        Field(name="root", description="Owning root", type="Root", parent="items", owner=True),
        Field(name="holder", description="Owning holder", type="Holder", parent="items", owner=True),
        Field(name="size", description="A size", type="int", example=1),
    ]
    return Schema(
        name="t",
        description="",
        namespace="t",
        version="1.0.0",
        classes=[
            Klass(
                name="Root",
                description="Root",
                fields=[
                    Field(name="holders", description="Holders", type="Holder", is_list=True, is_child=True),
                    Field(name="items", description="Items", type="Item", is_list=True, is_child=True),
                ],
            ),
            Klass(
                name="Holder",
                description="Holder",
                fields=[
                    Field(name="root", description="Root", type="Root", parent="holders"),
                    Field(name="items", description="Items", type="Item", is_list=True, is_child=True),
                ],
            ),
            Klass(name="Item", description="Item", fields=item_fields),
        ],
    )


class TestOwner(unittest.TestCase):
    def test_owner_fields_share_one_stored_member(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(_schema(), out, logging.getLogger("test")), 0)
            item = (Path(out) / "item.hpp").read_text()
            root = (Path(out) / "root.hpp").read_text()
            tables = (Path(out) / "native_tables.hpp").read_text()
        self.assertIn("enum class ItemOwnerKind : uint8_t", item)
        self.assertIn("Root,", item)
        self.assertIn("Holder,", item)
        self.assertIn("ItemOwner owner;", item)
        self.assertNotIn("RootId root;", item)
        self.assertIn("constexpr HolderId holder() const noexcept", item)
        self.assertIn("static constexpr ItemOwner holder(HolderId id) noexcept", item)
        # Index upkeep goes through the owner, and moving owners is one call.
        self.assertIn("bool set_item_owner(ItemId id, ItemOwner value)", root)
        self.assertIn("index_.holder_items[d.holder()].push_back(id);", root)
        self.assertIn('Member<ItemData, ItemOwner>{"owner", &ItemData::owner}', tables)
        self.assertIn("struct OwnerInfo<ItemOwner>", tables)

    def test_descriptor_stores_one_owner_field_with_its_options(self):
        schema = _schema()
        schema.link()
        item = next(k for k in build_descriptor(schema)["classes"] if k["name"] == "Item")
        names = [f["name"] for f in item["fields"]]
        self.assertEqual(names, ["owner", "size"])
        self.assertEqual(
            item["fields"][0],
            {
                "name": "owner",
                "kind": "owner",
                "options": [
                    {"name": "root", "type": "Root", "parent_field": "items"},
                    {"name": "holder", "type": "Holder", "parent_field": "items"},
                ],
            },
        )
        json.dumps(item)  # serialisable, as the snapshot and file require

    def test_validation(self):
        bad = _schema(
            [
                Field(name="root", description="r", type="Root", owner=True),
                Field(name="holder", description="h", type="Holder", parent="items", owner=True, is_optional=True),
                Field(name="owner", description="clash", type="int", example=1),
            ]
        )
        errors = "\n".join(e.message for e in SchemaRuleSet().validate(bad))
        self.assertIn("root in klass Item has owner=True but no parent", errors)
        self.assertIn("can't be a list or optional", errors)
        self.assertIn("can't also have a field named owner", errors)


if __name__ == "__main__":
    unittest.main()
