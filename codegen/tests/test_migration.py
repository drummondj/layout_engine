import json
import logging
from pathlib import Path
import tempfile
import unittest

from codegen import descriptor as d
from codegen import generator
from codegen import migration as m
from codegen.schema import Field

from tests.test_descriptor import make_schema


def desc(**kwargs):
    return d.build_descriptor(make_schema(**kwargs))


class TestOps(unittest.TestCase):
    def test_rename_field_and_class_follow_references(self):
        base = desc()
        after = json.loads(json.dumps(base))
        m.RenameField("Net", "name", "label").apply(after)
        m.RenameClass("Bus", "Bundle").apply(after)
        net = next(k for k in after["classes"] if k["name"] == "Net")
        self.assertIn("label", [f["name"] for f in net["fields"]])
        self.assertEqual(next(f for f in net["fields"] if f["name"] == "bus")["type"], "Bundle")

    def test_rename_class_retargets_owner_options(self):
        from tests.test_owner import _schema

        schema = _schema()
        schema.link()
        after = d.build_descriptor(schema)
        m.RenameClass("Holder", "Box").apply(after)
        item = next(k for k in after["classes"] if k["name"] == "Item")
        owner = next(f for f in item["fields"] if f["name"] == "owner")
        self.assertEqual([o["type"] for o in owner["options"]], ["Root", "Box"])
        m._check_references(after, m.Migration(from_version="1.0.0", to_version="1.0.1", description="t", ops=[]))

    def test_illegal_ops_are_reported(self):
        for op in [
            m.RenameField("Net", "missing", "x"),
            m.RemoveField("Nope", "x"),
            m.AddField("Net", {"name": "name", "kind": "scalar", "type": "int"}),
            m.RenameEnumValue("Dir", "A", "B"),
            m.Todo("decide"),
        ]:
            with self.assertRaises(m.MigrationError, msg=op):
                op.apply(json.loads(json.dumps(desc())))

    def test_runtime_entries(self):
        migration = m.Migration(
            "1.0.0",
            "1.1.0",
            "renames",
            [
                m.RenameClass("Bus", "Bundle"),
                m.RenameField("Net", "name", "label"),
                m.RenameEnumValue("Dir", "A", "AA"),
                m.RemoveEnumValue("Dir", "B", map_to="AA"),
                m.AddField("Net", {"name": "w", "kind": "scalar", "type": "int"}),
                m.RunCode("fixup", "recompute w"),
            ],
        )
        self.assertEqual(
            [(kind, klass, old, new) for _, kind, klass, old, new, _ in m.runtime_table([migration])],
            [
                ("rename_class", "", "Bus", "Bundle"),
                ("rename_field", "Net", "name", "label"),
                ("rename_enum_value", "Dir", "A", "AA"),
                ("rename_enum_value", "Dir", "B", "AA"),
                ("unsupported", "", "fixup", "recompute w"),
            ],
        )


class TestDraft(unittest.TestCase):
    def test_added_removed_and_altered_fields(self):
        old = desc()
        new = desc(extra_field=Field(name="w", description="w", type="int"), net_type="int", enum_values=("A", "B", "C"))
        ops = m.draft_ops(old, new, ask_rename=lambda *a: False)
        kinds = sorted(type(op).__name__ for op in ops)
        self.assertEqual(kinds, ["AddEnumValue", "AddField", "AlterField"])
        after = json.loads(json.dumps(old))
        for op in ops:
            op.apply(after)
        self.assertEqual(d.fingerprint(after), d.fingerprint(new))

    def test_possible_rename_asks_or_leaves_a_todo(self):
        old = desc(extra_field=Field(name="w", description="w", type="int"))
        new = desc(extra_field=Field(name="width", description="w", type="int"))
        ops = m.draft_ops(old, new, ask_rename=lambda kind, where, a, b: True)
        self.assertEqual([op.render() for op in ops], ["RenameField('Net', 'w', 'width')"])
        todo = m.draft_ops(old, new)
        self.assertEqual(len(todo), 1)
        self.assertIsInstance(todo[0], m.Todo)
        declined = m.draft_ops(old, new, ask_rename=lambda *a: False)
        self.assertEqual(sorted(type(op).__name__ for op in declined), ["AddField", "RemoveField"])

    def test_rendered_migration_loads_and_replays(self):
        old = desc()
        new = desc(extra_field=Field(name="w", description="w", type="int"), enum_values=("A", "B", "C"))
        ops = m.draft_ops(old, new)
        with tempfile.TemporaryDirectory() as tmp:
            path = m.next_migration_path(Path(tmp), "add w")
            self.assertEqual(path.name, "0001_add_w.py")
            path.write_text(m.render_migration("1.0.0", "1.1.0", "add w", ops))
            loaded = m.load_migrations(Path(tmp))
            self.assertEqual(len(loaded), 1)
            final = m.replay(old, loaded)[-1][1]
            self.assertEqual(d.fingerprint(final), d.fingerprint(new))
            self.assertEqual(m.next_migration_path(Path(tmp), "next").name, "0002_next.py")


class TestCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.history = Path(self.tmp.name) / "schema_history"
        self.migrations = Path(self.tmp.name) / "migrations"
        d.write_snapshot(self.history, desc(version="1.0.0"))
        self.v11 = desc(version="1.1.0", extra_field=Field(name="w", description="w", type="int"))
        d.write_snapshot(self.history, self.v11)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        self.migrations.mkdir(exist_ok=True)
        (self.migrations / name).write_text(text)

    def test_missing_migration(self):
        errors = m.check_migrations(self.v11, self.history, self.migrations)
        self.assertEqual(len(errors), 1)
        self.assertIn("1.1.0 has no migration", errors[0])

    def test_matching_migration_passes(self):
        ops = m.draft_ops(desc(version="1.0.0"), self.v11)
        self.write("0001_w.py", m.render_migration("1.0.0", "1.1.0", "w", ops))
        self.assertEqual(m.check_migrations(self.v11, self.history, self.migrations), [])

    def test_incomplete_migration_names_what_is_missing(self):
        self.write("0001_w.py", m.render_migration("1.0.0", "1.1.0", "w", []))
        errors = m.check_migrations(self.v11, self.history, self.migrations)
        self.assertEqual(len(errors), 1)
        self.assertIn("+ Net.w", errors[0])

    def test_todo_fails(self):
        self.write("0001_w.py", m.render_migration("1.0.0", "1.1.0", "w", [m.Todo("rename?")]))
        errors = m.check_migrations(self.v11, self.history, self.migrations)
        self.assertIn("unresolved TODO", errors[0])

    def test_chain_must_start_at_the_oldest_snapshot(self):
        self.write("0001_w.py", m.render_migration("0.9.0", "1.1.0", "w", []))
        errors = m.check_migrations(self.v11, self.history, self.migrations)
        self.assertIn("chain is at 1.0.0", errors[0])

    def test_schema_changed_beyond_the_last_migration(self):
        ops = m.draft_ops(desc(version="1.0.0"), self.v11)
        self.write("0001_w.py", m.render_migration("1.0.0", "1.1.0", "w", ops))
        newer = desc(version="1.1.0", extra_field=Field(name="w", description="w", type="double"))
        errors = m.check_migrations(newer, self.history, self.migrations)
        self.assertIn("doesn't reach the current schema", errors[0])


class TestGeneratorIntegration(unittest.TestCase):
    def test_generate_requires_a_migration_and_emits_the_runtime_table(self):
        logger = logging.getLogger("test")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "generated"
            history = Path(tmp) / "schema_history"
            migrations = Path(tmp) / "migrations"
            self.assertEqual(generator.generate(make_schema(), str(out), logger, history_dir=str(history)), 0)
            self.assertIn("std::array<Op, 0>", (out / "migrations.hpp").read_text())

            renamed = make_schema(version="1.1.0", extra_field=Field(name="width", description="w", type="int"))
            # 1.0.0 -> 1.1.0 needs a migration first.
            self.assertEqual(generator.generate(renamed, str(out), logger, history_dir=str(history)), 1)
            self.assertEqual(
                generator.make_migration(make_schema(version="1.1.0", extra_field=Field(name="width", description="w", type="int")),
                                         str(history), str(migrations), "add_width", logger, interactive=False),
                0,
            )
            written = next(migrations.iterdir())
            self.assertIn("AddField('Net'", written.read_text())
            renamed = make_schema(version="1.1.0", extra_field=Field(name="width", description="w", type="int"))
            self.assertEqual(generator.generate(renamed, str(out), logger, history_dir=str(history)), 0)

            # A rename shows up in the loader's table.
            written.write_text(written.read_text().replace("from codegen.migration import *", "from codegen.migration import *\n# edited"))
            (migrations / "0002_rename.py").write_text(
                m.render_migration("1.1.0", "1.2.0", "width renamed", [m.RenameField("Net", "width", "span")])
            )
            v12 = make_schema(version="1.2.0", extra_field=Field(name="span", description="w", type="int"))
            self.assertEqual(generator.generate(v12, str(out), logger, history_dir=str(history)), 0)
            header = (out / "migrations.hpp").read_text()
            self.assertIn('Op{ "1.2.0", OpKind::RenameField, "Net", "width", "span", "width renamed", "", "" }', header)
            # Every migration is listed for db_info, additive ones too, though only the rename has an op.
            self.assertIn("std::array<Migration, 2>", header)
            self.assertIn('Migration{ "1.1.0", "add width", "", false }', header)
            self.assertIn('Migration{ "1.2.0", "width renamed", "", false }', header)


if __name__ == "__main__":
    unittest.main()
