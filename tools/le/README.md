# le: the Layout Engine package manager

`le` builds a Layout Engine **project**: Layout Engine plus the extensions a
project lists, installed into the project's `.le/` directory. Design:
[docs/PACKAGE_MANAGER_RESEARCH.md](../../docs/PACKAGE_MANAGER_RESEARCH.md).
Writing an extension: [docs/EXTENSION_SDK.md](../../docs/EXTENSION_SDK.md).

It needs git 2.34+, `tar` and `ssh-keygen` (OpenSSH's client tools, used
to check signatures). Each release bundle carries `le` as a single
executable; from a checkout, run `tools/le/bin/le` (Python 3.11+, standard
library only).

How Layout Engine itself is installed:

- **Release bundle**: when every extension is Tcl-only and Layout Engine
  comes from GitHub by tag or version range, `le` downloads that release's
  bundle and its signature, checks it against Layout Engine's release key,
  and runs its `le_shell`. No compiler or cmake needed.
- **Built from source**: otherwise (a compiled extension, a `--rev` pin, a
  local `path`), it builds Layout Engine with the extensions, which needs
  everything a source build needs (`BUILD.md`).

## Quick start

```
le init my_chip                        # le_project.toml + .gitignore (.le/)
cd my_chip
le trust acme @acme_release_key.pub    # whose signatures you accept
le add acme_router --github acme/acme_router --version ">=1.4, <2" --publisher acme
le add my_checks --path ../my_checks   # a local extension
le shell                               # builds if needed, then runs le_shell
```

## Commands

| Command | What it does |
|---|---|
| `le init [dir] [--name N] [--layout-engine-path P]` | Creates `le_project.toml`, asking for this `le`'s Layout Engine minor series (e.g. `">=0.3, <0.4"`), or a local checkout with `--layout-engine-path`. |
| `le add <name> --github OWNER/REPO (--tag T \| --rev R \| --version RANGE) --publisher P` | Adds a GitHub extension, then installs. A range picks the highest matching signed `vX.Y.Z` tag. `--allow-unsigned` accepts it unsigned (development only). |
| `le add <name> --path DIR` | Adds a local extension (never signed or locked), then installs. |
| `le remove <name>` | Removes one, refusing if another extension depends on it, then installs. |
| `le trust <publisher> <key \| @file>` | Trusts an SSH public key to sign that publisher's releases. |
| `le install` | Installs exactly what the lock says, or resolves and writes the lock if `le_project.toml` changed. |
| `le update [name...]` | Resolves the named entries again (all if none), ignoring the lock, then installs. |
| `le list` | What's installed: versions, sources, commits, signers, release or source build. |
| `le shell [args...]` | Runs Layout Engine's `le_shell` with the project's extensions, reinstalling first if anything changed (including files in a `--path` extension). |

A tag must match the version in its `le_extension.toml`. `install` and
`update` refuse to move anything to a lower version than the lock has
unless given `--allow-downgrade`.

`add` and `remove` take `--no-install` to edit `le_project.toml` only.
`add`, `remove` and `trust` keep the file's comments.

## Files

- `le_project.toml`: what you want. Commit it.
- `le_project.lock`: what was installed, i.e. each GitHub source's tag,
  commit and signing key, and the release bundle's sha256. Commit it, so everyone builds the same thing. A
  locked install refuses a tag that now points elsewhere, or a different
  signing key; `le update` accepts the change.
- `.le/`: sources, the build tree and the bundle. Never commit it; it can be
  deleted and rebuilt.
- `~/.layout_engine/cache/` (or `$LE_CACHE_DIR`): git mirrors, unpacked
  release bundles and dependency sources shared by every project.

## Signatures

A GitHub extension's tag must be SSH-signed by a key you trusted for its
publisher (`le trust`). A `--rev` pin must point at a signed commit. Layout
Engine's own tags and release bundles are checked against its release key
(`assets/keys/allowed_signers`, built into the `le` executable). Publishers sign tags with `git tag -s` and
`gpg.format=ssh`; see `docs/RELEASING.md` for how Layout Engine does it.

## Tests

`python3 -m unittest discover -s tests -t .` from `tools/le` (ctest
`le_package_manager_unit`), using fake GitHub repos and a stubbed build.
`tests/integration_test.py <source dir>` builds a real project, then runs a
script-only project on a signed bundle of that build (ctest
`le_package_manager_integration`, with `-DLE_TEST_PACKAGE_MANAGER=ON`). CI
runs it in its own job (`le-integration`), only when a change touches what
`le` depends on, plus nightly and on demand.

The `le` executable is built with `-DLE_BUILD_LE_BINARY=ON` (needs
PyInstaller for CMake's Python), which installs it into the bundle; the
release build does this.
