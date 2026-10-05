import tempfile
import unittest
from pathlib import Path

from codegen import extension_manifest as em


def write_extension(root: Path, name: str, body: str, files=("le_extension.cmake",)) -> Path:
    directory = root / name
    directory.mkdir()
    for f in files:
        (directory / f).write_text("")
    (directory / em.MANIFEST_NAME).write_text(body)
    return directory


def manifest_text(name, prefix, version="1.0.0", deps="", api=1, layout_engine=">=0.2, <1", contents='cmake = "le_extension.cmake"'):
    return f"""
[extension]
name = "{name}"
version = "{version}"
prefix = "{prefix}"

[compatibility]
layout_engine = "{layout_engine}"
extension_api = {api}

[dependencies]
{deps}

[contents]
{contents}
"""


class TestConstraints(unittest.TestCase):
    def test_satisfies(self):
        self.assertTrue(em.satisfies("0.10.2", ">=0.9, <0.11"))
        self.assertFalse(em.satisfies("0.11.0", ">=0.9, <0.11"))
        self.assertTrue(em.satisfies("2.1.0", ">=2.0"))
        self.assertTrue(em.satisfies("1.4.0", "1.4"))
        self.assertFalse(em.satisfies("1.4.1", "==1.4.0"))
        self.assertTrue(em.satisfies("1.4.1", "!=1.4.0"))
        with self.assertRaises(em.ManifestError):
            em.satisfies("1.0.0", "~=1.0")


class TestManifests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_load_reads_every_section(self):
        d = write_extension(
            self.root,
            "acme_router",
            manifest_text("acme_router", "Acme", contents='cmake = "le_extension.cmake"\ntcl_procs = ["procs.tcl"]\ntcl_tests = ["t.tcl"]'),
            files=("le_extension.cmake", "procs.tcl", "t.tcl"),
        )
        m = em.load(d)
        self.assertEqual((m.name, m.version, m.prefix, m.extension_api), ("acme_router", "1.0.0", "Acme", 1))
        self.assertEqual(m.cmake, (d / "le_extension.cmake").resolve())
        self.assertEqual(m.tcl_procs, [(d / "procs.tcl").resolve()])
        self.assertEqual(m.tcl_tests, [(d / "t.tcl").resolve()])
        self.assertTrue(m.compiled)

    def test_script_only_extension_is_not_compiled(self):
        d = write_extension(self.root, "tools", manifest_text("tools", "Tools", contents='tcl_procs = ["t.tcl"]'), files=("t.tcl",))
        self.assertFalse(em.load(d).compiled)

    def test_bad_manifests_are_rejected(self):
        cases = {
            "BadName": manifest_text("BadName", "Bad"),
            "lowerprefix": manifest_text("lowerprefix", "acme"),
            "badversion": manifest_text("badversion", "Bv", version="1.0"),
            "missingfile": manifest_text("missingfile", "Mf", contents='cmake = "nope.cmake"'),
            "escapes": manifest_text("escapes", "Es", contents='cmake = "../x.cmake"'),
            "unknownkey": manifest_text("unknownkey", "Uk", contents='cmake = "le_extension.cmake"\nfoo = "bar"'),
        }
        for name, text in cases.items():
            d = write_extension(self.root, name, text)
            with self.assertRaises(em.ManifestError, msg=name):
                em.load(d)

    def test_order_puts_dependencies_first_then_names(self):
        a = em.load(write_extension(self.root, "a", manifest_text("a", "Aa", deps='c = ">=1.0"')))
        b = em.load(write_extension(self.root, "b", manifest_text("b", "Bb")))
        c = em.load(write_extension(self.root, "c", manifest_text("c", "Cc")))
        self.assertEqual([m.name for m in em.check_and_order([a, b, c], "0.2.0", 1)], ["b", "c", "a"])

    def test_all_problems_are_reported_together(self):
        a = em.load(write_extension(self.root, "a", manifest_text("a", "Same", api=2, deps='missing = ">=1"')))
        b = em.load(write_extension(self.root, "b", manifest_text("b", "Same", layout_engine=">=9")))
        with self.assertRaises(em.ManifestError) as raised:
            em.check_and_order([a, b], "0.2.0", 1)
        text = str(raised.exception)
        self.assertIn("share the prefix Same", text)
        self.assertIn("targets extension API 2", text)
        self.assertIn("needs layout_engine >=9", text)
        self.assertIn("depends on missing", text)

    def test_cycles_are_rejected(self):
        a = em.load(write_extension(self.root, "a", manifest_text("a", "Aa", deps='b = ">=1"')))
        b = em.load(write_extension(self.root, "b", manifest_text("b", "Bb", deps='a = ">=1"')))
        with self.assertRaises(em.ManifestError) as raised:
            em.check_and_order([a, b], "0.2.0", 1)
        self.assertIn("cycle", str(raised.exception))

    def test_main_writes_the_cmake_include(self):
        d = write_extension(self.root, "acme", manifest_text("acme", "Acme"))
        out = self.root / "le_extensions.cmake"
        self.assertEqual(em.main(["--layout-engine-version", "0.2.0", "--extension-api", "1", "--output", str(out), str(d)]), 0)
        text = out.read_text()
        self.assertIn('set(LE_EXTENSIONS "acme")', text)
        self.assertIn('set(LE_EXTENSION_acme_PREFIX "Acme")', text)
        self.assertIn("set(LE_EXTENSION_acme_COMPILED ON)", text)
        self.assertEqual(em.main(["--layout-engine-version", "9.0.0", "--extension-api", "1", "--output", str(out), str(d)]), 1)


if __name__ == "__main__":
    unittest.main()
