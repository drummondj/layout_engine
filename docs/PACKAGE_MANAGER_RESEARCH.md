# Package Manager — Architecture Research

Companion to [EXTENSION_MECHANISM_RESEARCH.md](EXTENSION_MECHANISM_RESEARCH.md) (how extensions are written and built in) and [NATIVE_FILE_FORMAT_RESEARCH.md](NATIVE_FILE_FORMAT_RESEARCH.md) (how their data is saved). Installing and authoring extensions are designed together: this document fixes what an extension must *declare* so it can be installed, and the extension research builds on that.

## Goal

A user creates a **Layout Engine project**: a directory with a config file listing the extensions they want. One command fetches the extensions, checks they are compatible with each other and with the chosen layout_engine version, and produces a runnable `le_shell` that includes them. The model is a very simple pip, plus a lock file so every machine gets the same build.

Non-goals for the first version: a public package index, prebuilt binaries of compiled extensions, Windows.

## Summary of the recommendation

| Concern | Recommendation |
|---|---|
| Project | A directory with `le_project.toml` (what the user wants) and `le_project.lock` (exactly what was installed: commits and hashes). Both are committed; `.le/` holds everything generated. |
| Extension metadata | Each extension declares itself in `le_extension.toml`: name, version, prefix, compatible layout_engine range, extension API version, dependencies, contents. It is data, so it can be resolved without running extension code. |
| Sources | **git** (a URL plus a tag, a revision, or a semver range matched against `vX.Y.Z` tags) and **local path**. A package index comes later, as a git repo of TOML files. |
| Resolution | One version per extension per project; the merged schema makes two impossible. No backtracking solver: pick the highest matching tag for each extension, then *check* every declared range, and on a conflict name the pin to change. |
| Two tiers | **Script** extensions (Tcl procs and data files only) install into a prebuilt release bundle: no compiler needed. **Compiled** extensions (any C++, schema or GUI) trigger a project build of layout_engine from source at the locked tag, as a superbuild. |
| Runtime | Each bundle has an `extensions.json` index that `le_shell` reads at startup to source extension procs in dependency order. A bundle is self-contained and relocatable. |
| Tool | `le`, a small Python 3.11+ CLI (stdlib `tomllib`; no other dependencies), shipped in the release bundle as a single PyInstaller binary, as codegen already can be (`codegen/codegen.spec`). |

**Rejected:**
- **Runtime-loaded binary plugins**, the usual way to avoid rebuilding. There is no stable ABI for a schema-dependent, header-only database (EXTENSION_MECHANISM_RESEARCH.md, "Rejected"). A C-API-only module tier remains an open question there.
- **pip/conda/Spack as the package manager.** pip has no notion of "rebuild the host application with these sources". Spack fits source builds, but it is heavy, and asking every user to adopt it for one tool is too much. Spack or conda recipes can still wrap a project later.
- **CMake `FetchContent` alone,** with no tool. It can fetch, but not check compatibility ranges up front, write a lock file, or install script extensions without a compiler.
- **A full SAT/PubGrub resolver.** Real projects will have a handful of extensions, so a clear error message beats a clever solver.

---

## 1. Constraints that shape the design

- **No binary compatibility.** Extensions are compiled together with layout_engine, and a schema extension changes `Root` itself. Installing a compiled extension therefore means rebuilding `le_shell`. The package manager is a *source* package manager, closer to Cargo than to pip-with-wheels.
- **Users run prebuilt releases.** The release (`Dockerfile.linux-release`) is a flat bundle: `le_shell`, `le_tcl.so`, the procs files, `fonts/` and `libtbb`. It is found at run time with `find_resource` (`src/core/resource_path.hpp`). Many users will have no compiler toolchain, so anything that doesn't need compiling must not require one. Hence the script tier.
- **Linux servers, often without root.** Everything installs under the project directory and a per-user cache. Nothing goes in system paths.
- **Python is already present for builds.** codegen needs Python ≥ 3.11, so a Python tool adds no new requirement for compiled builds. For script-only users, the tool ships as a PyInstaller binary in the bundle.
- **Files outlive builds.** A `.led` file records the core schema version and each extension's versions (NATIVE_FILE_FORMAT_RESEARCH.md §4.8). The project must never quietly downgrade to versions that can't read files it has already written.

---

## 2. Concepts

| Term | Meaning |
|---|---|
| **Project** | A directory containing `le_project.toml`. Usually a chip or flow repository that also holds Tcl scripts and design files. |
| **Extension** | A directory containing `le_extension.toml`, normally its own git repo. Described in EXTENSION_MECHANISM_RESEARCH.md §2. |
| **Tier** | *script* if the manifest lists only `tcl_procs` and `resources`; *compiled* if it has `schema`, `cmake`, or anything else that builds. A project is compiled if any of its extensions is. |
| **Bundle** | The runnable result: a release-shaped directory (`le_shell`, `le_tcl.so`, procs, `fonts/`) plus `ext/<name>/` per extension and `extensions.json`. |
| **Lock** | `le_project.lock`: the resolved layout_engine and extension revisions, with content hashes. Commit it, as with `Cargo.lock` or `poetry.lock`. |

---

## 3. `le_project.toml`

```toml
[project]
name    = "my_chip"
startup = "init.tcl"            # optional; sourced after every extension's procs

[layout_engine]
version = "0.10"                # semver range; resolved against release tags
# git = "https://github.com/drummondj/layout_engine", rev = "main"   # unreleased builds

[build]                         # used only when the project is compiled
type = "Release"
jobs = 8

[extensions]
acme_router = { git = "ssh://git@git.acme.com/eda/acme_router.git", version = ">=1.4, <2" }
acme_common = { git = "ssh://git@git.acme.com/eda/acme_common.git", tag = "v2.1.0" }
my_checks   = { path = "../my_checks" }   # local: rebuilt when it changes, never hashed
```

An extension's own `[dependencies]` (from its manifest) need not be repeated here if they can be found. For now, that means listed here or reachable through a source the dependent's manifest gives; once an index exists, through the index.

---

## 4. `le_project.lock`

Written by `le install` and `le update`; never edited by hand.

```toml
format = 1

[layout_engine]
version = "0.10.2"
source  = "git+https://github.com/drummondj/layout_engine"
rev     = "3f9c2e1..."
kind    = "source"              # "release" when every extension is script-tier

[[extension]]
name           = "acme_router"
version        = "1.4.0"
schema_version = "1.2.0"        # from its schema_history; recorded for file-compatibility checks
tier           = "compiled"
source         = "git+ssh://git@git.acme.com/eda/acme_router.git"
rev            = "a1b2c3d..."
sha256         = "..."          # of the fetched tree, so a moved tag is detected
dependencies   = ["acme_common"]
```

`le install` with an up-to-date lock installs exactly what the lock says. It never re-resolves, and fails if a tag now points at a different tree.

---

## 5. Sources and resolution

**Sources.**
- `git` + `tag` or `rev`: an exact pin.
- `git` + `version` range: resolved against the repo's `vX.Y.Z` tags (`git ls-remote --tags`), choosing the highest match. The manifest's `version` must equal the tag, or the install fails.
- `path`: used in place, for authoring (§9). Never hashed in the lock.
- Authentication is git's own (SSH keys, credential helpers). `le` never handles credentials, so private corporate repos work unchanged.

**Resolution**, in order:
1. Resolve `[layout_engine]` to one version.
2. Resolve each extension to one revision, then read its manifest. Add any dependency not yet listed, using the same rules.
3. Check every constraint, and report all failures at once:
   - each extension's `[compatibility] layout_engine` range contains the chosen version
   - its `extension_api` equals that version's `LE_EXTENSION_API_VERSION`
   - each `[dependencies]` range contains the chosen dependency version
   - names and prefixes are unique across the project
4. Order extensions topologically by dependency, breaking ties by name. That order is used everywhere: CMake, `register_all()`, schema `extend()` calls and procs sourcing.

A conflict produces a message such as *"acme_router 1.4.0 needs layout_engine <0.10, but the project asks for 0.10; pin acme_router to a newer tag or layout_engine to 0.9"*. There is no search over older versions; the user chooses.

**Downgrades.** `le update` refuses to lower layout_engine's schema version, or any extension's schema version, unless given `--allow-downgrade`. Files written since couldn't be opened by the older build (NATIVE_FILE_FORMAT_RESEARCH.md §4.9).

---

## 6. What `le install` does

```
read le_project.toml ──► lock up to date? ──no──► resolve (§5) ──► write le_project.lock
                               │yes                                     │
                               ▼                                        ▼
                       fetch locked revisions into ~/.layout_engine/cache/ (verify sha256)
                               │
             ┌─────────────────┴─────────────────┐
     all script-tier                       any compiled
             │                                   │
   fetch the release bundle            configure a superbuild in .le/build:
   for the locked version              cmake -S <layout_engine@rev> -B .le/build
   into the cache                            -DLE_EXTENSION_DIRS=<ordered dirs>
             │                         build; install into .le/bundle
             ▼                                   │
   .le/bundle = link to the release    ◄─────────┘
   + ext/<name>/ copies
   + extensions.json
```

- **Script tier:** no compiler is needed. The release bundle is unpacked once per version into the shared cache, and the project bundle refers to it. Each extension's procs and resources are copied into `.le/bundle/ext/<name>/`, and `extensions.json` lists them.
- **Compiled tier:** layout_engine's own CMake does the work, so there is one build path whether the superbuild was generated by `le` or written by hand. Requirements:
  - CMake runs codegen (extension research phase 0).
  - An `install` target produces the release-shaped bundle, with `ext/<name>/` and `extensions.json`. Today that layout exists only inside `Dockerfile.linux-release`, so it moves into CMake and the Dockerfile calls it.
- **Build cost:** a from-source build fetches and compiles slang, Blend2D, Dear ImGui and the rest. `le` points `FETCHCONTENT_BASE_DIR` at the shared cache (keyed by layout_engine version and compiler), so dependency sources download once per machine, and it uses `ccache` when present. Core objects can't be shared between projects once a schema extension changes `Root`.
- **Toolchain:** `le doctor` checks for what `BUILD.md` requires (compiler, CMake, SWIG, Tcl, bison, Boost, oneTBB) before a long build fails halfway. `le install --container` (later) builds inside the `Dockerfile.linux-ci` image and copies the bundle out. That suits servers that have podman/docker but not gcc-toolset.

---

## 7. Project layout and caches

```
my_chip/
  le_project.toml        committed
  le_project.lock        committed
  init.tcl               optional startup script
  .le/                   generated - .gitignore it
    build/               CMake tree (compiled projects)
    bundle/              runnable: le_shell, le_tcl.so, procs, fonts/,
                         ext/<name>/..., extensions.json
~/.layout_engine/
  settings.json          existing GUI settings (shared by all projects)
  cache/
    git/                 bare mirrors of every fetched repo
    releases/<version>/  unpacked release bundles
    deps/<key>/          FetchContent sources for from-source builds
```

`.le/` can always be deleted and rebuilt from the lock.

---

## 8. Runtime

**Finding extensions.** `app_init` reads `extensions.json` next to the executable, found with `find_resource` like the core procs. A script-tier project runs the cached release `le_shell`, so `le shell` passes the project's index explicitly with a new `-extensions <path>` flag (env `LE_EXTENSIONS_PATH`), matching the existing `-module`/`-procs` flags and `LE_TCL_MODULE`/`LE_TCL_PROCS_PATH`.

```json
{
  "format": 1,
  "layout_engine": "0.10.2",
  "extension_api": 1,
  "extensions": [
    {"name": "acme_common", "version": "2.1.0", "tier": "script",   "dir": "ext/acme_common", "procs": ["tcl/common.tcl"]},
    {"name": "acme_router", "version": "1.4.0", "tier": "compiled", "dir": "ext/acme_router", "procs": ["tcl/acme_router_procs.tcl"]}
  ]
}
```

- List order is load order.
- `le_shell` refuses an index whose `layout_engine` or `extension_api` doesn't match the running build, naming both. A compiled entry must also be present in the binary's `register_all()` list, which catches running a stale bundle after the project changed.
- After all procs are sourced, `le_shell` sources the project's `startup` script.

**Files.** A `.led` file names the extensions that wrote it.
- `le check design.led` runs `db_info` through the project's `le_shell` and compares the file's extensions and versions with the lock. It reports each one as: matches, will migrate, missing (with the `le add` line to fix it), or too new for this project.
- `read_db` refuses a file that needs an extension the project doesn't have (NATIVE_FILE_FORMAT_RESEARCH.md §6), and the error gives the `le add` line. `le check` lets a user find this out before opening the file.

**Settings.** `~/.layout_engine/settings.json` stays per-user and shared by every project. Extension sections (`extensions.<name>`) are versioned per extension. Unknown keys are already preserved, so moving between projects with different extension sets loses nothing. Per-project settings overrides are an open question.

---

## 9. The `le` CLI, including authoring

| Command | What it does |
|---|---|
| `le init [dir]` | Writes a minimal `le_project.toml` and a `.gitignore` entry for `.le/`. |
| `le add <name> --git URL [--tag T \| --rev R \| --version RANGE]` / `--path DIR` | Adds an extension, then runs `install`. |
| `le remove <name>` | Removes it (refusing if another extension depends on it), then runs `install`. |
| `le install` | §6. Idempotent; does nothing if the bundle matches the lock. |
| `le update [name...]` | Re-resolves the named entries (all if none), ignoring the lock for them; §5 downgrade rule. |
| `le shell [args...]` | Runs the project's `le_shell`, first rebuilding if a `path` extension changed (like `cargo run`). |
| `le list` | Installed extensions, versions, tiers and sources. |
| `le check <file.led>` | §8. |
| `le doctor` | Toolchain and cache checks. |
| `le new-extension <name> [--script]` | Scaffolds an extension directory: manifest, procs file, and for compiled ones `le_extension.cmake`, `schema_ext.py`, a register function and a test. These are copied from `examples/extensions/hello_script`/`hello_ext`, so the templates are the examples CI already builds. |
| `le test [name...]` | Builds and runs the named extensions' tests in the project build. |
| `le makemigration <name> --name <slug>` | Wraps `codegen --target makemigration --extension ...` for an extension's schema change. |

**Authoring loop:** create a project, then `le add my_ext --path ../my_ext` and `le shell`. Edit, then `le shell` again, which rebuilds incrementally. `le test my_ext` runs the tests. To release, bump `version` in the manifest, commit, and tag `vX.Y.Z`. In v1 there is no publish step: consumers point `git` at the repo.

---

## 10. Effects on the extension mechanism

Changes made to EXTENSION_MECHANISM_RESEARCH.md because of this design:

1. **A declarative manifest, `le_extension.toml`,** carries identity, compatibility, dependencies and contents. `le_add_extension()` keeps only build details (sources, include dirs, `LINK`), and its `PREFIX`/`SCHEMA`/`TCL_PROCS` arguments move into the manifest (§2 there).
2. **Two tiers.** Script extensions never need a compiler.
3. **One runtime path for procs:** a bundle `extensions.json` index replaces procs paths baked into `le_shell` (§6 there).
4. **Relocatable extensions.** Resources live under `ext/<name>/` and are found with `find_resource`. An extension never refers to layout_engine's source tree.
5. **CMake runs codegen.** This is now phase 0, a prerequisite, rather than an optional follow-up.
6. **Multiple vendors coexist.** Unique names and the prefix rule are mandatory, and dependency ordering is built in (open question 3 there, now settled).
7. **Package version ≠ schema version.** SCHM records both for each extension (NATIVE_FILE_FORMAT_RESEARCH.md §4.8, §6).
8. **Versioned layout_engine releases.** `project(... VERSION 0.1.0)` must start moving, releases must be tagged `vX.Y.Z` (the repo has no tags today), and release bundles must be published (e.g. GitHub releases) so script-tier projects can download them.
9. **An `install` target** that builds the release-shaped bundle, so `Dockerfile.linux-release` and `le` share one layout.

---

## 11. Phased rollout

Extension-mechanism phases are in EXTENSION_MECHANISM_RESEARCH.md §10. Package-manager phase 1 needs its phases 0–2.

| Phase | Work | Unlocks |
|---|---|---|
| 1 | `le` skeleton: `init`/`add`/`remove`/`install`/`shell`/`list`; git (tag/rev) and path sources; lock file; compiled-tier superbuild; CMake `install` target with `extensions.json` | Projects with compiled extensions |
| 2 | Tagged, published releases; `-extensions` flag; script tier on release bundles; `version` ranges against tags; `update` with the downgrade rule; `le` as a PyInstaller binary in the bundle | Projects with no compiler |
| 3 | `new-extension`, `test`, `makemigration`; `check <file.led>` (needs SCHM `"extensions"`, native-format phase 5) | Extension authoring and file checks |
| later | Package index (a git repo of TOML entries, so it can be private), `--container` builds, `doctor` | Discovery; servers without a toolchain |

Each phase is tested against `hello_ext` and `hello_script`: install from path and from a local git repo, rebuild after an edit, and refuse an incompatible range.

---

## 12. Open questions

1. **Tool name and language.** Is `le` acceptable as a command name? Python (recommended: `tomllib`, git subprocesses, fast to write) or C++ inside `le_shell` (no PyInstaller, but much more code for git, TOML and process handling)?
2. **Release download host.** GitHub releases on `drummondj/layout_engine`, or a customer-hostable mirror, for sites without internet access? This affects how `[layout_engine]` names a source.
3. **Container builds by default?** Should `le install` build in the CI image whenever podman/docker is available, for reproducibility, or only when asked?
4. **Per-project settings.** Should a project be able to override parts of `settings.json`, e.g. layer colours for a technology?
5. **Index format and trust.** When an index arrives, should entries be signed, or is git transport security plus lock hashes enough?
6. **macOS.** `le_shell` builds there, but releases are Linux-only. Should the package manager support macOS from-source builds from day one?
