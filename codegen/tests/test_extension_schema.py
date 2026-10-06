import json
import logging
import tempfile
import textwrap
import unittest
from pathlib import Path

from codegen import extension_schema
from codegen.descriptor import build_descriptor, build_extension_descriptor, fingerprint
from codegen.generator import generate
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


def _linked(schema):
    schema.link()
    return schema


if __name__ == "__main__":
    unittest.main()
