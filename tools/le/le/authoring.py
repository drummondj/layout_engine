"""
Writing extensions: `le new-extension` scaffolds one from the examples CI
builds, `le test` runs extensions' tests in the project build, `le
makemigration` drafts a schema migration, and `le check` compares a .led
file's extensions with the project.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from le import install as installer, lock as lockfile, project as projectfile
from codegen import extension_manifest  # on sys.path via le.install


class AuthoringError(Exception):
    pass


# --- new-extension ---------------------------------------------------------------

COMPILED_TEMPLATE = "hello_ext"
SCRIPT_TEMPLATE = "hello_script"
# A new extension starts at schema 0.1.0 with no history of its own.
_NOT_COPIED = {"__pycache__", "schema_history", "migrations", "golden"}


def template_dir(template: str) -> Path:
    """The example extension a new one is copied from."""
    if installer._FROZEN:
        return installer._data_dir() / "templates" / template
    return installer._REPO_ROOT / "examples" / "extensions" / template


def to_snake_case(name: str) -> str:
    """PascalCase to snake_case, as codegen names a class's functions (AcmeRouter -> acme_router)."""
    return "".join("_" + c.lower() if c.isupper() else c for c in name).lstrip("_")


def default_prefix(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def _renamer(template: str, name: str, prefix: str):
    """Rewrites the template's identifiers in one pass, so a new name containing
    the template's words isn't rewritten twice."""
    snake = to_snake_case(prefix)
    if template == SCRIPT_TEMPLATE:
        words = {"hello_script": name, "HelloScript": prefix, "HELLO_SCRIPT": snake.upper()}
        pattern = re.compile("hello_script|HelloScript|HELLO_SCRIPT")
        return lambda text: pattern.sub(lambda m: words[m.group(0)], text)
    lower_camel = prefix[0].lower() + prefix[1:]
    pattern = re.compile(r"hello_ext|HELLO|Hello|hello(?=[A-Z])|hello")

    def replace(m: re.Match) -> str:
        word = m.group(0)
        if word == "hello_ext":
            return name
        if word == "HELLO":
            return snake.upper()
        if word == "Hello":
            return prefix
        return lower_camel if m.end() < len(m.string) and m.string[m.end()].isupper() else snake

    return lambda text: pattern.sub(replace, text)


def _manifest_text(text: str, description: str, layout_engine: str) -> str:
    lines = text.splitlines(keepends=True)
    while lines and lines[0].startswith("#"):  # the example's own header comment
        lines.pop(0)
    text = "".join(lines)
    text = re.sub(r'^description = ".*"$', lambda _: f"description = {projectfile._quote(description)}", text, flags=re.M)
    return re.sub(r'^layout_engine = ".*"$', lambda _: f"layout_engine = {projectfile._quote(layout_engine)}", text, flags=re.M)


def new_extension(name: str, directory: Path, script: bool, prefix: Optional[str], description: Optional[str]) -> Path:
    """Copies the compiled or script example into `directory` as extension `name`; returns the directory."""
    if not extension_manifest._NAME.match(name):
        raise AuthoringError(f"name {name!r} must be snake_case (lower case, digits, underscores)")
    prefix = prefix or default_prefix(name)
    if not extension_manifest._PREFIX.match(prefix):
        raise AuthoringError(f"prefix {prefix!r} must be PascalCase, e.g. Acme")
    if directory.exists():
        raise AuthoringError(f"{directory} already exists")
    template = SCRIPT_TEMPLATE if script else COMPILED_TEMPLATE
    source = template_dir(template)
    if not source.is_dir():
        raise AuthoringError(f"the {template} template isn't at {source}")
    major, minor, _ = installer.own_layout_engine_version().split(".")
    layout_engine = f">={major}.{minor}, <{major}.{int(minor) + 1}"
    rename = _renamer(template, name, prefix)

    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if any(part in _NOT_COPIED for part in relative.parts) or path.is_dir():
            continue
        target = directory / rename(str(relative))
        target.parent.mkdir(parents=True, exist_ok=True)
        text = rename(path.read_text())
        if relative.name == extension_manifest.MANIFEST_NAME:
            text = _manifest_text(text, description or f"{name}: a Layout Engine extension", layout_engine)
        elif relative.name == "schema_ext.py":
            text = re.sub(r'^VERSION = ".*"$', 'VERSION = "0.1.0"', text, flags=re.M)
        elif relative.suffix == ".cpp":
            text = re.sub(r'kSchemaVersion = ".*";', 'kSchemaVersion = "0.1.0";', text)
        target.write_text(text)
        shutil.copymode(path, target)
    extension_manifest.load(directory)  # what was written must be a valid extension
    return directory


# --- The installed project -------------------------------------------------------


def extension_directory(project: projectfile.Project, name: str) -> Path:
    """Where an installed extension's files are: in place for a path source, else under .le/src."""
    source = project.extensions[name]
    return source.path if source.path is not None else project.root / installer.STATE_DIR / "src" / name


def _manifests(project: projectfile.Project, lock: lockfile.Lock, names: List[str]) -> List[extension_manifest.Manifest]:
    known = [e.name for e in lock.extensions]
    unknown = [n for n in names if n not in known]
    if unknown:
        raise AuthoringError(f"not in this project: {', '.join(unknown)} (it has {', '.join(known) or 'no extensions'})")
    return [extension_manifest.load(extension_directory(project, n)) for n in (names or known)]


def _run(command: List[str], what: str) -> int:
    print(f"le: {what}", flush=True)
    return subprocess.run(command).returncode


# --- test ------------------------------------------------------------------------


def run_tests(root: Path, names: List[str], shell: List[str]) -> int:
    """Builds and runs the named extensions' tests (all if none): ctest in a
    source build; for a release, each tcl_tests script through le_shell."""
    project = projectfile.load(root)
    lock = lockfile.load(root)
    manifests = _manifests(project, lock, names)
    if lock.layout_engine_bundle == "source":
        build_dir = project.root / installer.STATE_DIR / "build"
        targets = [f"{m.name}_test_deps" for m in manifests]
        jobs = str(project.jobs or os.cpu_count() or 2)
        if _run(["cmake", "--build", str(build_dir), "--target", *targets, "-j", jobs], "building the tests") != 0:
            raise AuthoringError("building the tests failed")
        regex = "^(" + "|".join(m.name for m in manifests) + r")\."
        return _run(["ctest", "--test-dir", str(build_dir), "--output-on-failure", "--no-tests=error", "-R", regex], "running the tests")
    tests = [t for m in manifests for t in m.tcl_tests]
    if not tests:
        raise AuthoringError("no tcl_tests to run")
    failed = [t for t in tests if _run([*shell, str(t)], f"running {t.name}") != 0]
    if failed:
        print(f"le: {len(failed)} of {len(tests)} tests failed: {', '.join(t.name for t in failed)}", flush=True)
        return 1
    print(f"le: {len(tests)} tests passed", flush=True)
    return 0


# --- makemigration ---------------------------------------------------------------


def _codegen_python(build_dir: Path) -> str:
    """The Python the project build runs codegen with (it has codegen's packages)."""
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        found = re.search(r"^Python3_EXECUTABLE:\w+=(.+)$", cache.read_text(), re.M)
        if found:
            return found.group(1)
    if installer._FROZEN:
        raise AuthoringError(f"no project build at {build_dir} to take codegen's Python from - run `le install`")
    return sys.executable


def make_migration(root: Path, name: str, slug: str, non_interactive: bool) -> int:
    """Drafts extension `name`'s next migration with codegen, against the project's Layout Engine source.
    Doesn't rebuild first: a bumped schema version without its migration doesn't build."""
    project = projectfile.load(root)
    lock = lockfile.load(root)
    if lock is None:
        raise AuthoringError("nothing installed yet - run `le install` (it writes the schema snapshot a migration starts from)")
    if name not in project.extensions:
        raise AuthoringError(f"{name} isn't in this project")
    if project.extensions[name].path is None:
        raise AuthoringError(f"{name} isn't a path extension - migrations are drafted in the extension's own checkout (`le add {name} --path DIR`)")
    if lock.layout_engine_bundle != "source":
        raise AuthoringError(f"{name} has no schema: this project runs a release, so every extension is script-only")
    target = extension_manifest.load(extension_directory(project, name))
    if target.schema is None:
        raise AuthoringError(f"{name} has no schema ([contents] schema) to migrate")
    le_source = project.layout_engine.path if project.layout_engine.path is not None else project.root / installer.STATE_DIR / "src" / "layout_engine"
    command = [_codegen_python(project.root / installer.STATE_DIR / "build"), "-m", "codegen.cli",
               "--schema", str(le_source / "src" / "database" / "schema.py"),
               "--target", "makemigration", "--migrate-extension", name, "--name", slug]
    for m in _manifests(project, lock, []):
        if m.schema is not None:
            command += ["--extension", str(m.directory)]
    if non_interactive:
        command.append("--non-interactive")
    print(f"le: drafting {name}'s migration", flush=True)
    return subprocess.run(command, env={**os.environ, "PYTHONPATH": str(le_source / "codegen")}).returncode


# --- check -----------------------------------------------------------------------

_EXTENSION_LINE = re.compile(r"^extension (\S+) (\S+) \(schema (\S+)\): (.*)$", re.M)
_SCHEMA_LINE = re.compile(r"^schema version: (\S+)", re.M)
_BUILD_LINE = re.compile(r"^this build: (\S+)", re.M)


@dataclass
class Finding:
    name: str
    status: str  # "matches", "will migrate", "missing", "too new"
    detail: str

    @property
    def ok(self) -> bool:
        return self.status in ("matches", "will migrate")


def _compare(name: str, file_version: str, built_version: str, update_hint: str) -> Finding:
    file_v, built_v = extension_manifest.parse_version(file_version), extension_manifest.parse_version(built_version)
    if file_v == built_v:
        return Finding(name, "matches", f"schema {file_version}")
    if file_v < built_v:
        return Finding(name, "will migrate", f"schema {file_version} -> {built_version}")
    return Finding(name, "too new", f"schema {file_version}, newer than this project's {built_version} - {update_hint}")


def findings(db_info: str) -> List[Finding]:
    """What `db_info` (run by the project's le_shell) says about loading the file in this project."""
    file_schema, built_schema = _SCHEMA_LINE.search(db_info), _BUILD_LINE.search(db_info)
    if not file_schema or not built_schema:
        raise AuthoringError(f"unexpected db_info output:\n{db_info}")
    result = [_compare("layout_engine", file_schema.group(1), built_schema.group(1), "update layout_engine (`le update layout_engine`)")]
    for name, package_version, schema_version, state in _EXTENSION_LINE.findall(db_info):
        built = re.match(r"this build has schema (\S+)", state)
        if built is None:
            result.append(Finding(name, "missing", f"{package_version} (schema {schema_version}) - add it: `le add {name} --github OWNER/REPO "
                                                   f"--version \">={package_version}\" --publisher P` or `le add {name} --path DIR`"))
        else:
            result.append(_compare(name, schema_version, built.group(1), f"update it (`le update {name}`)"))
    return result


def check(shell: List[str], led_file: Path) -> int:
    """Reports how each of the file's extensions (and core) would load in the project; 1 if any can't."""
    if not led_file.is_file():
        raise AuthoringError(f"{led_file} doesn't exist")
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "check.tcl"
        path = str(led_file.resolve()).replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}")
        script.write_text(f"puts [db_info {{{path}}}]\n")
        result = subprocess.run([*shell, str(script)], capture_output=True, text=True)
    if result.returncode != 0:
        raise AuthoringError(f"db_info failed:\n{result.stdout}{result.stderr}")
    report = findings(result.stdout)
    width = max(len(f.name) for f in report)
    for f in report:
        print(f"{f.name:<{width}}  {f.status:<12}  {f.detail}")
    return 0 if all(f.ok for f in report) else 1
