"""
End to end: `le` builds a real project from this checkout - hello_ext from a
local path, hello_script from a signed tag on a fake GitHub, my_ext from `le
new-extension` - runs their commands through `le shell`, then edits the path
extension and checks `le shell` rebuilds. `le test` runs my_ext's and
hello_script's tests, and `le check` reads a file of my_ext's objects before
and after a schema change drafted by `le makemigration`. Then a script-only
project gets a signed release bundle (this build reconfigured with no
extensions, signed with a test key), runs hello_script and its test with no
compiler or cmake on PATH, and `le check` finds hello_ext missing. Slow (it builds Layout Engine), so it's a ctest
only when LE_TEST_PACKAGE_MANAGER is ON. LE_DEPS_DIR can point at an existing
build's _deps to avoid downloading the dependencies again.

    python3 tools/le/tests/integration_test.py <layout_engine source dir>
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from le import build  # noqa: E402
from tests import helpers  # noqa: E402


def le(project: Path, *args: str, path: str = None, expect_failure: bool = False) -> str:
    env = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}
    if path is not None:
        env["PATH"] = path
    result = subprocess.run([sys.executable, "-m", "le", *args], cwd=project, capture_output=True, text=True, env=env)
    if (result.returncode != 0) != expect_failure:
        raise SystemExit(f"le {' '.join(args)} {'succeeded' if expect_failure else 'failed'}:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def has_line(out: str, *words: str) -> bool:
    return any(line.split()[: len(words)] == list(words) for line in out.splitlines())


def main() -> int:
    source = Path(sys.argv[1]).resolve()
    github = helpers.FakeGithub()
    try:
        root = github.root
        key = helpers.make_key(root, "acme")

        # hello_script, published as acme/hello_script with a signed tag.
        work = root / "work" / "hello_script"
        shutil.copytree(source / "examples" / "extensions" / "hello_script", work)
        helpers.run("git", "init", "-q", cwd=work)
        helpers.git_commit_all(work, "hello_script 0.1.0")
        helpers.git_tag(work, "v0.1.0", key)
        github.publish(work, "acme/hello_script")

        # hello_ext as a local path copy we can edit.
        hello_ext = root / "hello_ext"
        shutil.copytree(source / "examples" / "extensions" / "hello_ext", hello_ext)

        project = root / "project"
        le(root, "init", str(project), "--layout-engine-path", str(source))
        le(project, "trust", "acme", "@" + str(key) + ".pub")
        le(project, "add", "hello_script", "--github", "acme/hello_script", "--tag", "v0.1.0", "--publisher", "acme", "--no-install")
        my_ext = root / "my_ext"
        le(root, "new-extension", "my_ext", "--directory", str(my_ext))
        le(project, "add", "my_ext", "--path", str(my_ext), "--no-install")
        le(project, "add", "hello_ext", "--path", str(hello_ext))

        script = project / "check.tcl"
        script.write_text(
            'if {[hello_script_greet world] ne "Hello, world!"} { error "hello_script_greet" }\n'
            "hello_add_library world\n"
            'if {[hello_library_count] != 1} { error "hello_library_count" }\n'
            "puts [hello_ext_info]\n"
        )
        out = le(project, "shell", str(script))
        if "hello_ext 0.1.0" not in out:
            raise SystemExit(f"unexpected le shell output:\n{out}")

        # Edit the path extension's procs: `le shell` notices and rebuilds.
        procs = hello_ext / "tcl" / "hello_ext.tcl"
        procs.write_text(procs.read_text() + '\nproc hello_ext_edited {} { return "edited" }\n')
        script.write_text('puts [hello_ext_edited]\n')
        out = le(project, "shell", str(script))
        if "edited" not in out:
            raise SystemExit(f"the edit wasn't picked up:\n{out}")

        listing = le(project, "list")
        for expected in ("hello_ext 0.1.0", "hello_script 0.1.0", "github:acme/hello_script@", "signed SHA256:"):
            if expected not in listing:
                raise SystemExit(f"le list is missing {expected!r}:\n{listing}")

        authoring(project, my_ext)
        release_project(source, github, project / ".le" / "build", key)
        print("le integration test passed")
        return 0
    finally:
        github.close()


AUTHOR_FIELD = '                Field(name="author", description="Who wrote the note", type="str", is_optional=True),\n'


def authoring(project: Path, my_ext: Path) -> None:
    """The authoring loop on the new extension: test, write a file, change the schema, check the file."""
    out = le(project, "test", "my_ext", "hello_script")
    for expected in ("my_ext.MyExtExt.", "my_ext.my_ext_test", "hello_script.hello_script_test", "100% tests passed"):
        if expected not in out:
            raise SystemExit(f"le test output is missing {expected!r}:\n{out}")

    led = project / "notes.led"
    script = project / "write.tcl"
    script.write_text(f"create_my_ext_note -library [create_library -name lib1] -body hi\nwrite_db {{{led}}}\n")
    le(project, "shell", str(script))
    out = le(project, "check", str(led))
    if not has_line(out, "my_ext", "matches"):
        raise SystemExit(f"le check should say my_ext matches:\n{out}")

    # A new optional field on MyExtNote: a schema change needing a migration.
    schema = my_ext / "schema_ext.py"
    text = schema.read_text().replace('VERSION = "0.1.0"', 'VERSION = "0.2.0"')
    fields = text.index("            fields=[\n", text.index('name="MyExtNote"')) + len("            fields=[\n")
    schema.write_text(text[:fields] + AUTHOR_FIELD + text[fields:])
    le(project, "makemigration", "my_ext", "--name", "note_author", "--non-interactive")
    if len(list((my_ext / "migrations").glob("*_note_author.py"))) != 1:
        raise SystemExit(f"le makemigration wrote no migration: {sorted((my_ext / 'migrations').glob('*'))}")
    out = le(project, "check", str(led))  # rebuilds with the new schema first
    if not has_line(out, "my_ext", "will", "migrate"):
        raise SystemExit(f"le check should say my_ext will migrate:\n{out}")


def release_project(source: Path, github: helpers.FakeGithub, build_dir: Path, acme_key: Path) -> None:
    """A script-only project on a signed release bundle, with no build tools on PATH."""
    root = github.root
    version, _ = build.layout_engine_versions(source)
    helpers.run("cmake", "-S", str(source), "-B", str(build_dir), "-DLE_EXTENSION_DIRS=")
    helpers.run("cmake", "--build", str(build_dir), "--target", "le_shell", "le_tcl", "-j", str(os.cpu_count() or 2))
    bundle = root / "release_bundle"
    helpers.run("cmake", "--install", str(build_dir), "--component", "bundle", "--prefix", str(bundle))
    release_key = helpers.make_key(root, "release")
    helpers.fake_release(github, release_key, version, bundle=bundle)

    # Only what `le` needs for a release: git, ssh-keygen and tar.
    tools = root / "tools"
    tools.mkdir()
    for tool in ("git", "ssh-keygen", "tar"):
        (tools / tool).symlink_to(shutil.which(tool))
    major, minor, _ = version.split(".")
    project = root / "release_project"
    le(root, "init", str(project), path=str(tools))
    text = (project / "le_project.toml").read_text()
    if f'version = ">={major}.{minor}, <{major}.{int(minor) + 1}"' not in text:
        raise SystemExit(f"le init didn't default to this version's range:\n{text}")
    le(project, "trust", "acme", "@" + str(acme_key) + ".pub", path=str(tools))
    le(project, "add", "hello_script", "--github", "acme/hello_script", "--version", ">=0.1", "--publisher", "acme", path=str(tools))
    script = project / "check.tcl"
    script.write_text('puts [hello_script_greet release]\n')
    out = le(project, "shell", str(script), path=str(tools))
    if "Hello, release!" not in out:
        raise SystemExit(f"unexpected le shell output from the release:\n{out}")
    out = le(project, "test", path=str(tools))
    if "1 tests passed" not in out:
        raise SystemExit(f"unexpected le test output from the release:\n{out}")
    golden = source / "examples" / "extensions" / "hello_ext" / "tests" / "golden" / "0.1.0" / "notes.led"
    out = le(project, "check", str(golden), path=str(tools), expect_failure=True)
    if not has_line(out, "hello_ext", "missing"):
        raise SystemExit(f"le check should say hello_ext is missing:\n{out}")
    listing = le(project, "list", path=str(tools))
    if "release" not in listing:
        raise SystemExit(f"le list doesn't show the release:\n{listing}")


if __name__ == "__main__":
    sys.exit(main())
