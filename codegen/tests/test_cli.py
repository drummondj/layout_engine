from click.testing import CliRunner
from codegen.cli import cli
import os
from pathlib import Path
import shutil
import unittest


class TestCli(unittest.TestCase):
    def setUp(self) -> None:
        if os.path.exists("temp"):
            shutil.rmtree("temp")
        Path("temp").mkdir()

        return super().setUp()

    def test_cli(self):
        runner = CliRunner()
        with runner.isolated_filesystem():
            with open("hello.txt", "w") as f:
                f.write("Hello World!")

        result = runner.invoke(
            cli,
            ["--schema", "examples/solar_system.py", "--output", "temp/solar_system"],
            catch_exceptions=False,
        )
        assert result.exit_code == 0
        assert os.path.exists("temp/solar_system")
        for klass in ["planet", "star", "solar_system"]:
            assert os.path.exists(f"temp/solar_system/{klass}.hpp")
            assert os.path.exists(f"temp/solar_system/{klass}.cpp")
        assert os.path.exists("temp/solar_system/CMakeLists.txt")
        assert os.path.exists("temp/solar_system/test_solar_system.cpp")

    def test_failure(self):
        runner = CliRunner()
        result = runner.invoke(
            cli,
            [
                "--schema",
                "tests/schemas/validation_failures.py",
                "--output",
                "temp/validation_failures",
            ],
            catch_exceptions=False,
        )
        assert result.exit_code == 1


if __name__ == "__main__":
    unittest.main()
