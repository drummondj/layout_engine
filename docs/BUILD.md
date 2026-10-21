# Building from source

**Just want to run it?** Signed, portable Linux x86_64 releases (glibc 2.28+)
are on the [GitHub Releases](../../../releases) page; see
`docs/RELEASING.md` to verify one. They need no build tools.

A source build needs a C++23 compiler, CMake, the libraries Layout Engine
links and Python with codegen's packages. There are three ways to get them:

- **An `le` project:** `le toolchain` provides all of it, without root
  (`tools/le/README.md`). This is the way to build extensions.
- **Without `le`, on any x86_64 Linux, without root:** the same toolchain,
  from conda-forge via pixi (below). The machine needs only CA certificates,
  git and tar.
- **The machine's own packages:** `tools/le/bin/le doctor --source` lists
  what's missing and the apt or dnf package for each (see "With the
  machine's own packages").

Docker users can also run `docker compose run --rm ci` (`Dockerfile.linux-ci`).

## 1. Get the toolchain (once)

[pixi](https://pixi.sh) is a single static binary. Use the version `le`
pins (`PIXI_VERSION` and `PIXI_SHA256` in `tools/le/le/toolchain.py`):

```
curl -fsSL -o pixi.tar.gz https://github.com/prefix-dev/pixi/releases/download/v0.81.0/pixi-x86_64-unknown-linux-musl.tar.gz
echo "7aa3ec39aecceff9062fa2ed4d42cbaa0bdc25ddea727d048e061cf188d434f6  pixi.tar.gz" | sha256sum -c -
tar xzf pixi.tar.gz && mkdir -p ~/.local/bin && mv pixi ~/.local/bin/
cd toolchain && pixi install --frozen
```

`toolchain/pixi.toml` lists the toolchain (GCC 14, CMake, make, bison,
SWIG, Boost, oneTBB, Tcl, readline, X11, Mesa, ccache, Python with jinja2
and click, ...); `toolchain/pixi.lock` pins every package. The environment
(about 1.7 GB) goes in `toolchain/.pixi/`.

## 2. Configure, build and test

Inside the environment (`pixi shell` in `toolchain/`, then back to the
repository root), point CMake at the environment's own compilers and
libraries. The target-prefixed compiler names matter: CMake then also finds
the environment's `ar`/`ranlib`/`objdump` instead of the host's.

```
TC=$CONDA_PREFIX
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=$TC/bin/x86_64-conda-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=$TC/bin/x86_64-conda-linux-gnu-g++ \
    -DCMAKE_PREFIX_PATH=$TC -DCMAKE_FIND_ROOT_PATH=$TC \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DPython3_EXECUTABLE=$TC/bin/python3
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

For a Release tree (what benchmarks and the shipped `le_shell` use), the
same with `-B build_release -DCMAKE_BUILD_TYPE=Release`. A Profile tree
(`-DCMAKE_BUILD_TYPE=Profile`: Release plus debug info) turns on Tracy
instrumentation; any tree can set `-DLE_ENABLE_TRACY=ON` or `OFF`.

## With the machine's own packages

`le doctor --source` checks each item: a C++23 compiler (GCC 13+ or
Clang 17+), CMake 3.25+, make, bison, SWIG 4+, Python 3.11-3.13 and the
Boost, zlib, oneTBB, Tcl 8.6, readline, X11 and OpenGL development
packages. Install codegen's packages for the Python CMake will use, then
configure as usual:

```
python3 -m pip install ./codegen
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPython3_EXECUTABLE=$(command -v python3)
```

## Run it

```
./build/le_shell
```

finds `le_tcl.so` and the procs in its build tree. To make a
self-contained, relocatable copy (the same layout as a release), install
the `bundle` component:

```
cmake --install build_release --component bundle --prefix ~/le_bundle
~/le_bundle/le_shell
```

The bundle's own files always win over the build tree's, so it can be
copied to another machine as-is. It carries the shared libraries a machine
may lack or have too old (libstdc++, libgcc_s, readline, Tcl, zlib, and
libglvnd's GL dispatch and the X11 client libraries) in `runtime/`; its
`le_shell` is a small launcher that uses each only when the machine doesn't
provide it, or provides an older C++ runtime, then starts `le_shell.bin`.
`LE_BUNDLED_LIBRARIES=all` (or `none`) overrides the choice. The machine
needs a glibc at least as new as the one the bundle was built against (2.28
with the toolchain above), and, for the GUI, its own GL driver (Mesa, or a
GPU vendor's). To build extensions in, see `docs/EXTENSION_SDK.md`.

## After a source change

Rebuild and test with step 2's `cmake --build`/`ctest` lines; CMake
reconfigures itself when `CMakeLists.txt` changes. Editing
`src/database/schema.py` needs nothing extra: the build regenerates the code
(the `regen-database` skill covers the version bump, snapshot and golden
files a schema change needs).

The vendored LEF/DEF parsers build in-source (`src/lefdef/{lef,def}/`) and
trust timestamps, so objects from another toolchain (a different compiler,
a container) break the build with confusing errors. Delete them, and the
build tree, the way `Dockerfile.linux-ci`'s CMD does:

```
find src/lefdef/lef src/lefdef/def \( -name '*.o' -o -name '*.a' -o -name 'lef.tab.*' -o -name 'def.tab.*' -o -name '*.output' \) -delete
rm -rf src/lefdef/{lef,def}/{lib,include,bin}
```
