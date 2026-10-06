"""
End to end: `le` builds a real project from this checkout - hello_ext from a
local path, hello_script from a signed tag on a fake GitHub - runs both
extensions' commands through `le shell`, then edits the path extension and
checks `le shell` rebuilds. Then a script-only project gets a signed release
bundle (this build reconfigured with no extensions, signed with a test key)
and runs hello_script with no compiler or cmake on PATH. Slow (it builds Layout Engine), so it's a ctest
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


def le(project: Path, *args: str, path: str = None) -> str:
    env = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}
    if path is not None:
        env["PATH"] = path
    result = subprocess.run([sys.executable, "-m", "le", *args], cwd=project, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise SystemExit(f"le {' '.join(args)} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


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

        release_project(source, github, project / ".le" / "build", key)
        print("le integration test passed")
        return 0
    finally:
        github.close()


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
    listing = le(project, "list", path=str(tools))
    if "release" not in listing:
        raise SystemExit(f"le list doesn't show the release:\n{listing}")


if __name__ == "__main__":
    sys.exit(main())
