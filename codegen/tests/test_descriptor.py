import json
import logging
from pathlib import Path
import tempfile
import unittest

from codegen import descriptor as d
from codegen import generator
from codegen.schema import Field, Klass, Schema


def make_schema(version="1.0.0", extra_field=None, description="A net", net_type="str", enum_values=("A", "B")):
    """A small schema covering every field kind: parent, child, ref, enum, struct, scalar."""
    net_fields = [
        Field(name="design", description="Owner", type="Design", parent="nets"),
        Field(name="name", description=description, type=net_type, index=True, unique_per_parent=True),
        Field(name="bus", description="Bus", type="Bus", is_optional=True),
        Field(name="dir", description="Direction", type="Dir", is_optional=True),
        Field(name="boxes", description="Boxes", type="Box", is_list=True),
    ]
    if extra_field is not None:
        net_fields.append(extra_field)
    schema = Schema(
        name="t",
        description="test",
        namespace="t",
        version=version,
        classes=[
            Klass(
                name="Design",
                description="Root",
                fields=[Field(name="nets", description="Nets", type="Net", is_list=True, is_child=True)],
            ),
            Klass(name="Net", description="Net", fields=net_fields),
            Klass(name="Bus", description="Bus", fields=[Field(name="width", description="w", type="int", default=1)]),
            Klass(
                name="Dir",
                description="Direction",
                is_enum=True,
                has_pool=False,
                fields=[Field(name=n, description=n, type="int", value=i) for i, n in enumerate(enum_values)],
            ),
            Klass(
                name="Box",
                description="Box",
                has_pool=False,
                fields=[Field(name="x", description="x", type="dbu"), Field(name="y", description="y", type="dbu")],
            ),
        ],
    )
    schema.link()
    return schema


def fp(schema):
    return d.fingerprint(d.build_descriptor(schema))


class TestDescriptor(unittest.TestCase):
    def test_field_kinds(self):
        desc = d.build_descriptor(make_schema())
        classes = {k["name"]: k for k in desc["classes"]}
        kinds = {f["name"]: f["kind"] for f in classes["Net"]["fields"]}
        self.assertEqual(
            kinds, {"design": "parent", "name": "scalar", "bus": "ref", "dir": "enum", "boxes": "struct"}
        )
        self.assertEqual(classes["Design"]["fields"][0]["kind"], "child")
        self.assertEqual(classes["Dir"]["values"], [{"name": "A", "value": 0}, {"name": "B", "value": 1}])
        self.assertEqual(classes["Box"]["kind"], "struct")
        self.assertEqual(classes["Bus"]["fields"][0]["default"], 1)

    def test_fingerprint_ignores_descriptions_and_version(self):
        self.assertEqual(fp(make_schema()), fp(make_schema(description="Something else")))
        self.assertEqual(fp(make_schema()), fp(make_schema(version="9.9.9")))

    def test_fingerprint_ignores_declaration_order(self):
        a = make_schema()
        b = make_schema()
        b.classes.reverse()
        b.get_klass("Net").fields.reverse()
        self.assertEqual(fp(a), fp(b))

    def test_fingerprint_detects_shape_changes(self):
        base = fp(make_schema())
        self.assertNotEqual(base, fp(make_schema(extra_field=Field(name="w", description="w", type="int"))))
        self.assertNotEqual(base, fp(make_schema(net_type="int")))
        self.assertNotEqual(base, fp(make_schema(enum_values=("A", "B", "C"))))

    def test_descriptor_json_round_trips(self):
        desc = d.build_descriptor(make_schema())
        self.assertEqual(json.loads(d.descriptor_json(desc)), desc)


class TestHistory(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / "schema_history"

    def tearDown(self):
        self.tmp.cleanup()

    def record(self, schema, update_snapshot=False):
        desc = d.build_descriptor(schema)
        check = d.check_history(desc, self.dir, update_snapshot)
        if check.write:
            d.write_snapshot(self.dir, desc)
        return check

    def test_baseline_then_unchanged(self):
        check = self.record(make_schema())
        self.assertEqual(check.errors, [])
        self.assertTrue(check.write)
        self.assertTrue((self.dir / "1.0.0.json").exists())
        check = self.record(make_schema(description="docs only"))
        self.assertEqual(check.errors, [])
        self.assertFalse(check.write)

    def test_change_without_bump_fails(self):
        self.record(make_schema())
        changed = make_schema(extra_field=Field(name="w", description="w", type="int"))
        check = self.record(changed)
        self.assertEqual(len(check.errors), 1)
        self.assertIn("Bump version", check.errors[0])

    def test_update_snapshot_overwrites(self):
        self.record(make_schema())
        changed = make_schema(extra_field=Field(name="w", description="w", type="int"))
        check = self.record(changed, update_snapshot=True)
        self.assertEqual(check.errors, [])
        snapshot = json.loads((self.dir / "1.0.0.json").read_text())
        self.assertEqual(snapshot["fingerprint"], fp(changed))

    def test_bump_records_new_snapshot(self):
        self.record(make_schema())
        check = self.record(make_schema(version="1.1.0", extra_field=Field(name="w", description="w", type="int")))
        self.assertEqual(check.errors, [])
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["1.0.0.json", "1.1.0.json"])

    def test_version_must_increase(self):
        self.record(make_schema(version="1.1.0"))
        check = self.record(make_schema(version="1.0.5", extra_field=Field(name="w", description="w", type="int")))
        self.assertEqual(len(check.errors), 1)
        self.assertIn("not newer", check.errors[0])

    def test_bad_version_string(self):
        check = self.record(make_schema(version="0.0.1_test"))
        self.assertEqual(len(check.errors), 1)


class TestGenerate(unittest.TestCase):
    def test_writes_schema_version_header_and_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "generated"
            history = Path(tmp) / "schema_history"
            logger = logging.getLogger("test")
            schema = make_schema()
            self.assertEqual(generator.generate(schema, str(out), logger, history_dir=str(history)), 0)
            header = (out / "schema_version.hpp").read_text()
            self.assertIn('kVersion = "1.0.0"', header)
            self.assertIn(f'kFingerprint = "{fp(schema)}"', header)
            self.assertIn('R"LEDESC({"format":1', header)
            self.assertTrue((history / "1.0.0.json").exists())

            changed = make_schema(extra_field=Field(name="w", description="w", type="int"))
            (out / "marker").write_text("kept")
            self.assertEqual(generator.generate(changed, str(out), logger, history_dir=str(history)), 1)
            # A failed history check leaves the previous output untouched.
            self.assertTrue((out / "marker").exists())


if __name__ == "__main__":
    unittest.main()
