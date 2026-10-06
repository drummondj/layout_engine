# le: the Layout Engine package manager

`le` builds a Layout Engine **project**: Layout Engine plus the extensions a
project lists, installed into the project's `.le/` directory. Design:
[docs/PACKAGE_MANAGER_RESEARCH.md](../../docs/PACKAGE_MANAGER_RESEARCH.md).
Writing an extension: [docs/EXTENSION_SDK.md](../../docs/EXTENSION_SDK.md).

It needs Python 3.11+ (standard library only), git 2.34+, `ssh-keygen`
(OpenSSH's client tools, used by git to check signatures), and everything a
Layout Engine source build needs (`BUILD.md`). Run it as `tools/le/bin/le`,
or put `tools/le/bin` on your `PATH`.

Every project currently builds Layout Engine from source (#72 adds
prebuilt releases for script-only projects, and version ranges).

## Quick start

```
le init my_chip                        # le_project.toml + .gitignore (.le/)
cd my_chip
le trust acme @acme_release_key.pub    # whose signatures you accept
le add acme_router --github acme/acme_router --tag v1.4.0 --publisher acme
le add my_checks --path ../my_checks   # a local extension
le shell                               # builds if needed, then runs le_shell
```

## Commands

| Command | What it does |
|---|---|
| `le init [dir] [--name N] [--layout-engine-path P]` | Creates `le_project.toml`, using this `le`'s Layout Engine release tag, or a local checkout with `--layout-engine-path`. |
| `le add <name> --github OWNER/REPO (--tag T \| --rev R) --publisher P` | Adds a GitHub extension, then installs. `--allow-unsigned` accepts it unsigned (development only). |
| `le add <name> --path DIR` | Adds a local extension (never signed or locked), then installs. |
| `le remove <name>` | Removes one, refusing if another extension depends on it, then installs. |
| `le trust <publisher> <key \| @file>` | Trusts an SSH public key to sign that publisher's releases. |
| `le install` | Builds exactly what the lock says, or resolves and writes the lock if `le_project.toml` changed. |
| `le update [name...]` | Resolves the named entries again (all if none), ignoring the lock, then installs. |
| `le list` | What's installed: versions, sources, commits, signers. |
| `le shell [args...]` | Runs `.le/bundle/le_shell`, rebuilding first if anything changed (including files in a `--path` extension). |

`add` and `remove` take `--no-install` to edit `le_project.toml` only.
`add`, `remove` and `trust` keep the file's comments.

## Files

- `le_project.toml`: what you want. Commit it.
- `le_project.lock`: what was installed, i.e. each GitHub source's commit and
  the key that signed it. Commit it, so everyone builds the same thing. A
  locked install refuses a tag that now points elsewhere, or a different
  signing key; `le update` accepts the change.
- `.le/`: sources, the build tree and the bundle. Never commit it; it can be
  deleted and rebuilt.
- `~/.layout_engine/cache/` (or `$LE_CACHE_DIR`): git mirrors and dependency
  sources shared by every project.

## Signatures

A GitHub extension's tag must be SSH-signed by a key you trusted for its
publisher (`le trust`). A `--rev` pin must point at a signed commit. Layout
Engine's own tags are checked against its release key
(`assets/keys/allowed_signers`). Publishers sign tags with `git tag -s` and
`gpg.format=ssh`; see `docs/RELEASING.md` for how Layout Engine does it.

## Tests

`python3 -m unittest discover -s tests -t .` from `tools/le` (ctest
`le_package_manager_unit`), using fake GitHub repos and a stubbed build.
`tests/integration_test.py <source dir>` builds a real project (ctest
`le_package_manager_integration`, with `-DLE_TEST_PACKAGE_MANAGER=ON`).
