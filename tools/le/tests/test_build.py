"""The superbuild `le` generates, switching an existing build tree over to it,
and sharing dependency sources (not builds) through the cache."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from le import build


class TestSuperbuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = mock.patch.dict(os.environ, {"LE_CACHE_DIR": str(self.root / "cache")})  # never the user's cache
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("LE_DEPS_DIR", None)
        self.source = self.root / "layout engine"  # a space, as in any user's path
        self.source.mkdir()

    def test_adds_layout_engine_with_add_subdirectory(self):
        directory = build.write_superbuild(self.source, self.root / "superbuild")
        text = (directory / "CMakeLists.txt").read_text()
        self.assertIn(f"add_subdirectory([[{self.source.resolve()}]] layout_engine)", text)
        self.assertIn("enable_testing()", text)
        self.assertNotIn("LE_EXTENSION_DIRS ", text.split("\n", 2)[2], "le passes the list when configuring")

    def test_an_unchanged_file_isnt_rewritten(self):
        path = build.write_superbuild(self.source, self.root / "superbuild") / "CMakeLists.txt"
        before = path.stat().st_mtime_ns
        build.write_superbuild(self.source, self.root / "superbuild")
        self.assertEqual(path.stat().st_mtime_ns, before)

    def test_a_build_tree_configured_from_the_source_itself_is_replaced(self):
        state = self.root / ".le"
        build_dir = state / "build"
        build_dir.mkdir(parents=True)
        (build_dir / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n")
        (build_dir / "stale.o").write_text("")
        commands = []

        def run(command, what):
            commands.append(command)
            if command[1] == "--install":
                (state / "bundle").mkdir()

        with mock.patch.object(build, "_run", run):
            build.build(self.source, [], state, "Release", 1, None)
        self.assertFalse((build_dir / "stale.o").exists())
        self.assertEqual(commands[0][commands[0].index("-S") + 1], str(state / "superbuild"))



class TestSourceCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = mock.patch.dict(os.environ, {"LE_CACHE_DIR": str(self.root / "cache")})
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("LE_DEPS_DIR", None)

    def test_the_cache_holds_sources_only_and_le_deps_dir_overrides_it(self):
        self.assertEqual(build.source_cache_dir(), self.root / "cache" / "sources")
        with mock.patch.dict(os.environ, {"LE_DEPS_DIR": str(self.root / "elsewhere")}):
            self.assertEqual(build.source_cache_dir(), self.root / "elsewhere")

    def test_configure_builds_dependencies_in_the_projects_own_tree(self):
        command = build.configure_command(self.root / "superbuild", self.root / "build", "Debug", [self.root / "ext"], ["-DFOO=1"])
        self.assertIn("-UFETCHCONTENT_BASE_DIR", command)
        self.assertFalse(any(arg.startswith("-DFETCHCONTENT_BASE_DIR") for arg in command))
        self.assertIn(f"-DLE_SOURCE_CACHE_DIR={self.root / 'cache' / 'sources'}", command)
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", command)
        self.assertLess(command.index("-UFETCHCONTENT_BASE_DIR"), command.index("-DFOO=1"), "a project's cmake_args come last")

    def _listing(self, build_dir, *dependencies):
        listing = build_dir / "layout_engine" / "le_fetched_sources.json"
        listing.parent.mkdir(parents=True, exist_ok=True)
        listing.write_text(json.dumps([{"name": n, "entry": e, "source_dir": str(s)} for n, e, s in dependencies]))

    def test_newly_downloaded_sources_are_copied_into_the_cache(self):
        build_dir = self.root / "build"
        cache = self.root / "cache" / "sources"
        fmt = build_dir / "_deps" / "fmt-src"
        (fmt / "include").mkdir(parents=True)
        (fmt / "include" / "core.h").write_text("fmt")
        (fmt / "link").symlink_to("include")
        already = cache / "zstd-0123"
        already.mkdir(parents=True)
        self._listing(build_dir, ("fmt", "fmt-abcd", fmt), ("zstd", "zstd-0123", already))

        self.assertEqual(build.populate_source_cache(build_dir, cache), ["fmt-abcd"])
        self.assertEqual((cache / "fmt-abcd" / "include" / "core.h").read_text(), "fmt")
        self.assertTrue((cache / "fmt-abcd" / "link").is_symlink())
        self.assertEqual(sorted(p.name for p in cache.iterdir()), ["fmt-abcd", "zstd-0123"], "no staging directories left")
        self.assertTrue(fmt.is_dir(), "the project keeps its own copy")
        self.assertEqual(build.populate_source_cache(build_dir, cache), [], "a second run adds nothing")

    def test_another_le_finishing_first_wins(self):
        build_dir = self.root / "build"
        cache = self.root / "cache" / "sources"
        fmt = build_dir / "_deps" / "fmt-src"
        fmt.mkdir(parents=True)
        (fmt / "mine").write_text("")
        self._listing(build_dir, ("fmt", "fmt-abcd", fmt))

        real_copytree = build.shutil.copytree

        def racing_copytree(source, destination, symlinks):
            (cache / "fmt-abcd").mkdir()
            (cache / "fmt-abcd" / "theirs").write_text("")
            return real_copytree(source, destination, symlinks=symlinks)

        with mock.patch.object(build.shutil, "copytree", racing_copytree):
            self.assertEqual(build.populate_source_cache(build_dir, cache), [])
        self.assertEqual([p.name for p in (cache / "fmt-abcd").iterdir()], ["theirs"])
        self.assertEqual([p.name for p in cache.iterdir()], ["fmt-abcd"])

    def test_no_listing_means_nothing_to_add(self):
        self.assertEqual(build.populate_source_cache(self.root / "build", self.root / "cache"), [])

    def test_the_old_shared_dependency_builds_are_removed(self):
        old = self.root / "cache" / "deps" / "0.2.0" / "blend2d-build"
        old.mkdir(parents=True)
        build.remove_old_dependency_cache()
        self.assertFalse((self.root / "cache" / "deps").exists())
        build.remove_old_dependency_cache()  # nothing left: no-op


if __name__ == "__main__":
    unittest.main()
