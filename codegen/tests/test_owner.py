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


class TestOwnerApi(unittest.TestCase):
    def test_create_takes_the_owner_as_one_kind_and_id(self):
        from codegen import tcl_generator

        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(tcl_generator.generate(_schema(), out, logging.getLogger("test")), 0)
            declarations = (Path(out) / "api" / "declarations.inc").read_text()
            shim = (Path(out) / "tcl" / "le_tcl_shim_generated.inc").read_text()
            procs = (Path(out) / "tcl" / "le_tcl_procs_generated.tcl").read_text()
        self.assertIn("} LeItemOwner;", declarations)
        self.assertIn("LE_ITEM_OWNER_ROOT = 1,", declarations)
        self.assertIn("LE_ITEM_OWNER_HOLDER = 2,", declarations)
        self.assertIn("static inline LeItemOwner le_item_owner_holder(LeHolderId id)", declarations)
        self.assertIn("LeItemId le_create_item(LeHandle *handle, LeItemOwner owner, int32_t size)", declarations)
        self.assertIn("resolve_item_owner(owner_kind, owner_id)", shim)
        # Tcl keeps a flag per owner and passes the given one's name and token.
        self.assertIn("foreach kind {root holder}", procs)
        self.assertIn("create_item_cmd $owner_kind $owner_id", procs)


class TestSingularOwnerSlot(unittest.TestCase):
    """An owner whose child field isn't a list holds at most one child."""

    def _schema(self):
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
                        Field(name="item", description="The one item", type="Item", is_child=True),
                    ],
                ),
                Klass(
                    name="Item",
                    description="Item",
                    fields=[
                        Field(name="root", description="Owning root", type="Root", parent="items", owner=True),
                        Field(name="holder", description="Owning holder", type="Holder", parent="item", owner=True),
                    ],
                ),
            ],
        )

    def test_only_the_non_list_owner_is_singular(self):
        schema = self._schema()
        schema.link()
        item = schema.get_klass("Item")
        self.assertEqual([f.name for f in item.get_singular_parent_fields()], ["holder"])
        self.assertEqual(schema.get_klass("Holder").get_singular_parent_fields(), [])

    def test_create_and_set_owner_refuse_a_taken_slot(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(self._schema(), out, logging.getLogger("test")), 0)
            root = (Path(out) / "root.hpp").read_text()
        self.assertIn("if (data.holder().valid() && index_.holder_item.contains(data.holder()))", root)
        self.assertIn("if (value.kind == ItemOwnerKind::Holder && index_.holder_item.contains(HolderId{value.index, value.generation}))", root)
        # Deleting or moving an Item only clears the slot if it's still this Item's.
        self.assertIn("if (it != index_.holder_item.end() && it->second == id)", root)


class TestGlobalUniqueIndex(unittest.TestCase):
    """A plain index=True field is unique across the Root; a unique_per_parent one isn't global."""

    def _schema(self):
        return Schema(
            name="t",
            description="",
            namespace="t",
            version="1.0.0",
            classes=[
                Klass(
                    name="Shelf",
                    description="Shelf",
                    fields=[
                        Field(name="name", description="Name", type="str", example="a", index=True),
                        Field(name="books", description="Books", type="Book", is_list=True, is_child=True),
                    ],
                ),
                Klass(
                    name="Book",
                    description="Book",
                    fields=[
                        Field(name="shelf", description="Shelf", type="Shelf", parent="books"),
                        Field(name="name", description="Name", type="str", example="b", index=True, unique_per_parent=True),
                    ],
                ),
            ],
        )

    def test_only_the_global_index_is_globally_unique(self):
        schema = self._schema()
        schema.link()
        self.assertEqual([f.name for f in schema.get_klass("Shelf").get_global_unique_fields()], ["name"])
        self.assertEqual(schema.get_klass("Book").get_global_unique_fields(), [])

    def test_create_update_and_rebuild_refuse_a_clash(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(self._schema(), out, logging.getLogger("test")), 0)
            root = (Path(out) / "root.hpp").read_text()
        self.assertIn("if (index_.shelf_by_name.contains(data.name))\n                return ShelfId{};", root)
        self.assertIn("if (index_.shelf_by_name.contains(*name))", root)
        self.assertIn("if (index_.shelf_by_name.contains(value))\n                return false;", root)
        self.assertIn('": duplicate name"', root)
