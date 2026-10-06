"""Shared test fixtures: throwaway SSH keys, signed git repos served as fake GitHub."""

import os
import subprocess
import tempfile
import textwrap
from pathlib import Path


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)


def make_key(directory: Path, name: str) -> Path:
    """An ed25519 key pair; returns the private key's path (public: + .pub)."""
    key = directory / name
    run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", name, "-f", str(key))
    return key


def public_key(key: Path) -> str:
    return (key.parent / (key.name + ".pub")).read_text().strip()


def git_commit_all(repo: Path, message: str, signing_key: Path = None) -> None:
    run("git", "add", "-A", cwd=repo)
    args = ["git", "-c", "user.name=t", "-c", "user.email=t@example.com"]
    if signing_key is not None:
        args += ["-c", "gpg.format=ssh", "-c", f"user.signingkey={signing_key}.pub", "commit", "-S"]
    else:
        args += ["commit"]
    run(*args, "-q", "-m", message, cwd=repo)


def git_tag(repo: Path, tag: str, signing_key: Path = None) -> None:
    base = ["git", "-c", "user.name=t", "-c", "user.email=t@example.com"]
    if signing_key is None:
        run(*base, "tag", tag, cwd=repo)
    else:
        run(*base, "-c", "gpg.format=ssh", "-c", f"user.signingkey={signing_key}.pub", "tag", "-s", tag, "-m", tag, cwd=repo)


def manifest(name: str, prefix: str, version: str = "1.0.0", layout_engine: str = ">=0.2, <0.3", extra_contents: str = "", deps: str = "") -> str:
    return textwrap.dedent(
        f"""\
        [extension]
        name = "{name}"
        version = "{version}"
        prefix = "{prefix}"

        [compatibility]
        layout_engine = "{layout_engine}"
        extension_api = 1

        [dependencies]
        {deps}

        [contents]
        tcl_procs = ["tcl/{name}.tcl"]
        {extra_contents}
        """
    )


def write_script_extension(directory: Path, name: str, prefix: str, **kwargs) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "le_extension.toml").write_text(manifest(name, prefix, **kwargs))
    (directory / "tcl").mkdir(exist_ok=True)
    (directory / "tcl" / f"{name}.tcl").write_text(f"proc {name}_hello {{}} {{ return hello }}\n")
    return directory


class FakeGithub:
    """Bare repos under a temp dir, served to `le` through LE_GITHUB_URL; the cache goes there too."""

    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.saved = {k: os.environ.get(k) for k in ("LE_GITHUB_URL", "LE_CACHE_DIR", "LE_RELEASES_URL", "LE_RELEASE_ALLOWED_SIGNERS")}
        os.environ["LE_GITHUB_URL"] = f"file://{self.root}/github/{{repo}}.git"
        os.environ["LE_CACHE_DIR"] = str(self.root / "cache")

    def publish(self, work: Path, repo: str) -> None:
        """Pushes the work tree's branches and tags to github/<repo>.git."""
        bare = self.root / "github" / f"{repo}.git"
        if not bare.exists():
            bare.parent.mkdir(parents=True, exist_ok=True)
            run("git", "init", "-q", "--bare", str(bare))
        run("git", "push", "-q", "--force", "--tags", str(bare), "HEAD:refs/heads/main", cwd=work)

    def close(self):
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()


def fake_layout_engine(directory: Path, version: str = "0.2.0", api: int = 1) -> Path:
    """Just enough of a Layout Engine source tree for `le` to read its versions."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "CMakeLists.txt").write_text(
        f"project(layout_engine_backend VERSION {version} LANGUAGES C CXX)\nset(LE_EXTENSION_API_VERSION {api})\n"
    )
    return directory


def fake_release(fake_github: "FakeGithub", key: Path, version: str = "0.3.0", api: int = 1, sign: bool = True, bundle: Path = None) -> Path:
    """
    A signed release of a fake Layout Engine: a tagged repo published as
    drummondj/layout_engine (signed by `key`, the stand-in release key) and
    a release tarball with its .sig, served through LE_RELEASES_URL. The
    tarball holds `bundle`, or else a stub le_shell that prints its arguments.
    """
    import tarfile

    os.environ["LE_RELEASES_URL"] = f"file://{fake_github.root}/releases/{{tag}}/{{asset}}"
    signers = fake_github.root / "release_allowed_signers"
    signers.write_text(f'release@layout-engine namespaces="git,layout_engine-release" {" ".join(public_key(key).split()[:2])}\n')
    os.environ["LE_RELEASE_ALLOWED_SIGNERS"] = str(signers)

    work = fake_github.root / "work" / "layout_engine"
    if not work.exists():
        work.mkdir(parents=True)
        run("git", "init", "-q", cwd=work)
    fake_layout_engine(work, version, api)
    git_commit_all(work, f"v{version}")
    git_tag(work, f"v{version}", key)
    fake_github.publish(work, "drummondj/layout_engine")

    if bundle is None:
        bundle = fake_github.root / "bundle_src" / version
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "le_shell").write_text('#!/bin/sh\necho "le_shell $*"\n')
        (bundle / "le_shell").chmod(0o755)
        (bundle / "extensions.json").write_text(
            f'{{"format": 1, "layout_engine": "{version}", "extension_api": {api}, "extensions": []}}\n'
        )
    tag = f"v{version}"
    out = fake_github.root / "releases" / tag
    out.mkdir(parents=True, exist_ok=True)
    tarball = out / f"layout_engine-linux-x86_64-{tag}.tar.gz"
    with tarfile.open(tarball, "w:gz") as archive:
        for p in bundle.iterdir():
            archive.add(p, arcname=p.name)
    if sign:
        run("ssh-keygen", "-q", "-Y", "sign", "-f", str(key), "-n", "layout_engine-release", str(tarball))
    return tarball
