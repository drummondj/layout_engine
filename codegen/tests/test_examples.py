import copy
import logging
import tempfile
import unittest

from codegen.generator import generate
from examples import eda, solar_system
from tests.compile_check import compile_headers, compiler


class TestExamples(unittest.TestCase):
    """The example schemas generate code that compiles."""

    def check(self, schema):
        with tempfile.TemporaryDirectory() as out:
            # generate() links the schema in place; keep the shared example pristine.
            self.assertEqual(generate(copy.deepcopy(schema), out, logging.getLogger("test"), history_dir=None), 0)
            if compiler() is None:
                self.skipTest("no C++ compiler")
            result = compile_headers(out)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_eda(self):
        self.check(eda.schema)

    def test_solar_system(self):
        self.check(solar_system.schema)


if __name__ == "__main__":
    unittest.main()
