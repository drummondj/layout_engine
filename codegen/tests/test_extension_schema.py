import json
import logging
import tempfile
import textwrap
import unittest
from pathlib import Path

from codegen import extension_schema
from codegen import migration as m
from codegen.descriptor import build_descriptor, build_extension_descriptor, fingerprint
from codegen.generator import generate, make_migration
from codegen.schema import Field, Klass, Schema


def _core():
    return Schema(
        name="t",
        description="",
        namespace="t",
        version="1.0.0",
        classes=[
            Klass(
                name="Root",
                description="Root",
                fields=[Field(name="libraries", description="Libraries", type="Library", is_list=True, is_child=True)],
            ),
            Klass(
                name="Library",
                description="A library",
                fields=[
                    Field(name="root", description="Root", type="Root", parent="libraries"),
                    Field(name="name", description="Name", type="str", example="lib", index=True),
                ],
            ),
        ],
    )


NOTE = """
from codegen.schema import Field, Klass

VERSION = "0.1.0"

def extend(schema):
    schema.classes.append(Klass(name="HelloNote", description="A note", fields=[
        Field(name="library", description="Owner", type="Library", parent="hello_notes"),
        Field(name="text", description="Text", type="str", example="hi"),
    ]))
"""


class TestExtensionSchema(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def extension(self, name="hello_ext", prefix="Hello", body=NOTE):
        directory = self.root / name
        directory.mkdir()
        (directory / "le_extension.toml").write_text(
            textwrap.dedent(
                f"""\
                [extension]
                name = "{name}"
                version = "1.2.3"
                prefix = "{prefix}"

                [compatibility]
                layout_engine = ">=0.3, <0.4"
                extension_api = 1

                [contents]
                schema = "schema_ext.py"
                """
            )
        )
        (directory / "schema_ext.py").write_text(textwrap.dedent(body))
        ext = extension_schema.load(directory)
        self.assertIsNotNone(ext)
        return ext

    def test_classes_merge_and_parents_get_a_synthesized_child_list(self):
        schema = _core()
        core_fingerprint = fingerprint(build_descriptor(_linked(_core())))
        ext = self.extension()
        self.assertEqual(extension_schema.apply(schema, [ext]), [])
        self.assertEqual(ext.version, "0.1.0")
        self.assertEqual(ext.package_version, "1.2.3")
        note = schema.get_klass("HelloNote")
        self.assertEqual(note.extension, "hello_ext")
        child_list = next(f for f in schema.get_klass("Library").fields if f.name == "hello_notes")
        self.assertTrue(child_list.is_child and child_list.is_list)
        self.assertEqual(child_list.synthesized_by, "hello_ext")

        schema.link()
        # The core descriptor doesn't see the extension at all.
        self.assertEqual(fingerprint(build_descriptor(schema)), core_fingerprint)
        ext_descriptor = build_extension_descriptor(schema, "hello_ext", "0.1.0")
        self.assertEqual([k["name"] for k in ext_descriptor["classes"]], ["HelloNote"])

    def test_generated_code_and_snapshots(self):
        schema = _core()
        ext = self.extension()
        self.assertEqual(extension_schema.apply(schema, [ext]), [])
        history = self.root / "core_history"
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test"), history_dir=str(history), extensions=[ext]), 0)
            root = (Path(out) / "root.hpp").read_text()
            version = (Path(out) / "schema_version.hpp").read_text()
        self.assertIn("create_hello_note", root)
        self.assertIn('{"hello_ext", "1.2.3", "0.1.0", "', version)
        self.assertIn("kExtensionCount = 1", version)
        snapshot = json.loads((ext.history_dir / "0.1.0.json").read_text())
        self.assertEqual([k["name"] for k in snapshot["descriptor"]["classes"]], ["HelloNote"])
        core_snapshot = json.loads((history / "1.0.0.json").read_text())
        self.assertNotIn("HelloNote", json.dumps(core_snapshot))

    def test_a_schema_change_without_a_version_bump_fails(self):
        schema = _core()
        ext = self.extension()
        extension_schema.apply(schema, [ext])
        log = logging.getLogger("test")
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, log, history_dir=str(self.root / "h"), extensions=[ext]), 0)
            changed = _core()
            (ext.schema_path).write_text(textwrap.dedent(NOTE).replace('example="hi"),', 'example="hi"),\n        Field(name="size", description="Size", type="int", example=1),'))
            ext2 = extension_schema.load(ext.schema_path.parent)
            self.assertEqual(extension_schema.apply(changed, [ext2]), [])
            with self.assertLogs("test", level="ERROR") as logs:
                self.assertEqual(generate(changed, out, log, history_dir=str(self.root / "h"), extensions=[ext2]), 1)
        self.assertIn("Bump VERSION in", "\n".join(logs.output))

    def test_classes_it_does_not_own_are_read_only(self):
        body = NOTE + textwrap.dedent(
            """
            def extend(schema):
                schema.get_klass("Library").fields.append(Field(name="hello_flag", description="x", type="bool", example=True))
            """
        )
        errors = extension_schema.apply(_core(), [self.extension(body=body)])
        self.assertTrue(any("changed class Library, which belongs to core" in e for e in errors), errors)

    def test_classes_need_the_prefix_and_unique_names(self):
        body = NOTE.replace('name="HelloNote"', 'name="Note"')
        errors = extension_schema.apply(_core(), [self.extension(body=body)])
        self.assertTrue(any("must be named with the extension's prefix Hello" in e for e in errors), errors)

        body = NOTE.replace('name="HelloNote"', 'name="Library"')
        errors = extension_schema.apply(_core(), [self.extension(name="other_ext", body=body)])
        self.assertTrue(any("class Library is defined twice" in e for e in errors), errors)

    def test_a_child_list_name_already_on_the_parent_is_refused(self):
        body = NOTE.replace('parent="hello_notes"', 'parent="name"')
        errors = extension_schema.apply(_core(), [self.extension(body=body)])
        self.assertTrue(any("Library already has a field name" in e for e in errors), errors)

    def test_version_and_extend_are_required(self):
        errors = extension_schema.apply(_core(), [self.extension(body="def extend(schema):\n    pass\n")])
        self.assertTrue(any("must set VERSION" in e for e in errors), errors)

    def test_an_extension_can_own_children_of_an_earlier_extensions_class(self):
        later = """
        from codegen.schema import Field, Klass

        VERSION = "1.0.0"

        def extend(schema):
            schema.classes.append(Klass(name="ByeTag", description="A tag", fields=[
                Field(name="note", description="Owner", type="HelloNote", parent="bye_tags"),
            ]))
        """
        schema = _core()
        errors = extension_schema.apply(schema, [self.extension(), self.extension(name="bye_ext", prefix="Bye", body=later)])
        self.assertEqual(errors, [])
        child_list = next(f for f in schema.get_klass("HelloNote").fields if f.name == "bye_tags")
        self.assertEqual(child_list.synthesized_by, "bye_ext")
        schema.link()
        self.assertNotIn("bye_tags", json.dumps(build_extension_descriptor(schema, "hello_ext", "0.1.0")))


RENAMED = NOTE.replace('VERSION = "0.1.0"', 'VERSION = "0.2.0"').replace('name="text"', 'name="body"')

RENAME_MIGRATION = """
from codegen.migration import *

migration = Migration(
    extension="hello_ext",
    depends_on_core="1.0.0",
    from_version="0.1.0",
    to_version="0.2.0",
    description="text renamed to body",
    ops=[RenameField("HelloNote", "text", "body")],
)
"""


class TestExtensionMigrations(unittest.TestCase):
    """An extension's own migration chain, and how it meets core's."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.log = logging.getLogger("test")
        self.core_history = self.root / "core" / "schema_history"
        self.core_migrations = self.root / "core" / "migrations"
        self.ext_dir = self.root / "hello_ext"
        self.ext_dir.mkdir()
        (self.ext_dir / "le_extension.toml").write_text(
            '[extension]\nname = "hello_ext"\nversion = "1.0.0"\nprefix = "Hello"\n\n'
            '[compatibility]\nlayout_engine = ">=0.3"\nextension_api = 1\n\n[contents]\nschema = "schema_ext.py"\n'
        )
        self.set_schema(NOTE)

    def set_schema(self, body):
        (self.ext_dir / "schema_ext.py").write_text(textwrap.dedent(body))

    def generate(self, core=None, renames=None):
        schema = core or _core()
        ext = extension_schema.load(self.ext_dir)
        errors = extension_schema.apply(schema, [ext], renames)
        if errors:
            return errors, ""
        with tempfile.TemporaryDirectory() as out:
            with self.assertLogs("test", level="INFO") as logs:
                code = generate(schema, out, self.log, history_dir=str(self.core_history), migrations_dir=str(self.core_migrations), extensions=[ext])
            header = (Path(out) / "migrations.hpp").read_text() if code == 0 else ""
        return ([] if code == 0 else [line for line in logs.output if "ERROR" in line]), header

    def write_migration(self, text, name="0001_text_to_body.py"):
        (self.ext_dir / "migrations").mkdir(exist_ok=True)
        (self.ext_dir / "migrations" / name).write_text(textwrap.dedent(text))

    def test_a_rename_migration_reaches_the_runtime_table_tagged_with_its_extension(self):
        self.assertEqual(self.generate()[0], [])
        snapshot = json.loads((self.ext_dir / "schema_history" / "0.1.0.json").read_text())
        self.assertEqual(snapshot["core_version"], "1.0.0")

        self.set_schema(RENAMED)
        errors, _ = self.generate()
        self.assertTrue(any("has no migration" in e and "--migrate-extension hello_ext" in e for e in errors), errors)
        self.write_migration(RENAME_MIGRATION)
        errors, header = self.generate()
        self.assertEqual(errors, [])
        self.assertIn('Op{ "0.2.0", OpKind::RenameField, "HelloNote", "text", "body", "text renamed to body", "hello_ext", "1.0.0" }', header)

    def test_makemigration_drafts_into_the_extensions_directory_with_its_core_version(self):
        self.assertEqual(self.generate()[0], [])
        self.set_schema(RENAMED)
        schema = _core()
        ext = extension_schema.load(self.ext_dir)
        self.assertEqual(extension_schema.apply(schema, [ext]), [])
        code = make_migration(schema, str(self.core_history), str(self.core_migrations), "text_to_body", self.log, interactive=False, extension=ext)
        self.assertEqual(code, 0)
        drafted = (self.ext_dir / "migrations" / "0001_text_to_body.py").read_text()
        self.assertIn("extension='hello_ext'", drafted)
        self.assertIn("depends_on_core='1.0.0'", drafted)
        self.assertIn("from_version='0.1.0'", drafted)
        self.assertFalse(self.core_migrations.exists(), "core's migrations are untouched")

    def test_an_extension_migration_may_only_change_its_own_classes(self):
        self.assertEqual(self.generate()[0], [])
        self.set_schema(RENAMED)
        self.write_migration(RENAME_MIGRATION.replace('ops=[RenameField("HelloNote", "text", "body")]', 'ops=[RenameField("HelloNote", "text", "body"), RenameField("Library", "name", "title")]'))
        errors, _ = self.generate()
        self.assertTrue(any("Library isn't one of hello_ext's classes" in e for e in errors), errors)

    def test_depends_on_core_is_required_and_never_newer_than_core(self):
        self.assertEqual(self.generate()[0], [])
        self.set_schema(RENAMED)
        self.write_migration(RENAME_MIGRATION.replace('depends_on_core="1.0.0"', 'depends_on_core="9.0.0"'))
        errors, _ = self.generate()
        self.assertTrue(any("newer than this core schema (1.0.0)" in e for e in errors), errors)
        self.write_migration(RENAME_MIGRATION.replace('    depends_on_core="1.0.0",\n', ""))
        errors, _ = self.generate()
        self.assertTrue(any("missing depends_on_core" in e for e in errors), errors)

    def test_a_core_class_rename_needs_only_a_schema_edit(self):
        self.assertEqual(self.generate()[0], [])
        # Core 1.1.0 renames Library to Archive.
        renamed_core = _core()
        renamed_core.version = "1.1.0"
        renamed_core.get_klass("Library").name = "Archive"
        renamed_core.get_klass("Root").fields[0].type = "Archive"
        renamed_core.get_klass("Archive").fields[0].parent = "libraries"
        self.core_migrations.mkdir(parents=True)
        (self.core_migrations / "0001_archive.py").write_text(
            'from codegen.migration import *\nmigration = Migration(from_version="1.0.0", to_version="1.1.0", '
            'description="Library renamed to Archive", ops=[RenameClass("Library", "Archive")])\n'
        )
        renames = m.core_renames_since(m.load_migrations(self.core_migrations), None)

        # schema_ext.py still says Library: the build names the line to change.
        errors, _ = self.generate(renamed_core, renames)
        self.assertTrue(any("a core migration renamed Library to Archive - update schema_ext.py: HelloNote.library" in e for e in errors), errors)

        # Updated, at the same VERSION and with no migration: fine.
        self.set_schema(NOTE.replace('type="Library"', 'type="Archive"'))
        renamed_core = _core()
        renamed_core.version = "1.1.0"
        renamed_core.get_klass("Library").name = "Archive"
        renamed_core.get_klass("Root").fields[0].type = "Archive"
        errors, _ = self.generate(renamed_core, renames)
        self.assertEqual(errors, [])

    def test_extension_migrations_run_right_after_the_core_migration_they_depend_on(self):
        core = [
            m.Migration("1.0.0", "1.1.0", "core one", [m.RenameClass("A", "B")]),
            m.Migration("1.1.0", "1.2.0", "core two", [m.RenameClass("C", "D")]),
        ]
        acme = [m.Migration("0.1.0", "0.2.0", "acme", [m.RenameClass("AcmeX", "AcmeY")], extension="acme", depends_on_core="1.1.0")]
        early = [m.Migration("0.1.0", "0.2.0", "early", [m.RenameClass("EarlyX", "EarlyY")], extension="early", depends_on_core="1.0.0")]
        table = m.merged_runtime_table(core, "1.2.0", [("acme", acme), ("early", early)])
        self.assertEqual([(row[5], row[6]) for row in table], [("early", "early"), ("core one", ""), ("acme", "acme"), ("core two", "")])


class TestExtensionOwnedShapes(unittest.TestCase):
    """An extension class owning objects with polymorphic owners (Shape)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def core(self):
        from tests.test_owner import _schema  # Root/Holder own Items through owner=True fields

        return _schema()

    def extension(self, body):
        directory = self.root / "hello_ext"
        directory.mkdir(exist_ok=True)
        (directory / "le_extension.toml").write_text(
            '[extension]\nname = "hello_ext"\nversion = "1.0.0"\nprefix = "Hello"\n\n'
            '[compatibility]\nlayout_engine = ">=0.3"\nextension_api = 1\n\n[contents]\nschema = "schema_ext.py"\n'
        )
        (directory / "schema_ext.py").write_text(textwrap.dedent(body))
        return extension_schema.load(directory)

    BOX = """
    from codegen.schema import Field, Klass

    VERSION = "0.1.0"

    def extend(schema):
        schema.classes.append(Klass(name="HelloBox", description="A box", fields=[
            Field(name="root", description="Owner", type="Root", parent="hello_boxes"),
            Field(name="items", description="Its items", type="Item", is_list=True, is_child=True, owner=True),
        ]))
    """

    def test_the_owner_option_is_synthesized_and_kept_out_of_the_core_descriptor(self):
        core_fingerprint = fingerprint(build_descriptor(_linked(self.core())))
        schema = self.core()
        self.assertEqual(extension_schema.apply(schema, [self.extension(self.BOX)]), [])
        option = next(f for f in schema.get_klass("Item").fields if f.name == "hello_box")
        self.assertTrue(option.owner)
        self.assertEqual(option.parent, "items")
        self.assertEqual(option.synthesized_by, "hello_ext")
        self.assertFalse(next(f for f in schema.get_klass("HelloBox").fields if f.name == "items").owner)

        schema.link()
        self.assertEqual(fingerprint(build_descriptor(schema)), core_fingerprint)
        ext = build_extension_descriptor(schema, "hello_ext", "0.1.0")
        items = next(f for f in ext["classes"][0]["fields"] if f["name"] == "items")
        self.assertTrue(items["owner"], "the descriptor records the declaration on the list")
        without = json.loads(json.dumps(ext))
        del next(f for f in without["classes"][0]["fields"] if f["name"] == "items")["owner"]
        self.assertNotEqual(fingerprint(ext), fingerprint(without), "it's part of the extension's shape")

        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            item = (Path(out) / "item.hpp").read_text()
        self.assertIn("HelloBox,", item)
        self.assertIn("static constexpr ItemOwner hello_box(HelloBoxId id) noexcept", item)

    def test_a_render_purpose_joins_the_schema_and_carries_the_prefix(self):
        body = self.BOX.replace('from codegen.schema import Field, Klass', 'from codegen.schema import Field, Klass, Purpose, Render').replace(
            'schema.classes.append(Klass(name="HelloBox", description="A box", fields=[',
            'schema.classes.append(Klass(name="HelloBox", description="A box", render=Render(Purpose("HELLO_BOX", "helloBox", "Boxes")), fields=[',
        )
        schema = self.core()
        schema.purposes = []
        self.assertEqual(extension_schema.apply(schema, [self.extension(body)]), [])
        self.assertEqual([p.name for p in schema.purposes], ["HELLO_BOX"])

        bad = body.replace('Purpose("HELLO_BOX", "helloBox", "Boxes")', 'Purpose("BOX", "box", "Boxes")')
        errors = extension_schema.apply(self.core(), [self.extension(bad)])
        self.assertTrue(any("purpose BOX must start with HELLO_" in e for e in errors), errors)
        self.assertTrue(any("label 'box' must start with 'hello'" in e for e in errors), errors)

    def test_owning_a_class_without_owner_fields_is_refused(self):
        body = self.BOX.replace('type="Item", is_list=True, is_child=True, owner=True', 'type="Holder", is_list=True, is_child=True, owner=True')
        errors = extension_schema.apply(self.core(), [self.extension(body)])
        self.assertTrue(any("Holder has no owner fields to join" in e for e in errors), errors)


def _linked(schema):
    schema.link()
    return schema


if __name__ == "__main__":
    unittest.main()
