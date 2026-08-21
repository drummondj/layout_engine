# Activates the rootless toolchain rocky8-bootstrap.sh assembles - source
# this once per shell session before configuring/building anything on a
# locked-down Rocky Linux 8 machine with no root and no system package
# installs (see backend/CLAUDE.md's Build section and the plan this pair
# of scripts came from):
#
#   source backend/scripts/rocky8-env.sh
#
# Not a standalone executable (no shebang, not chmod +x) - `source`, not
# `./rocky8-env.sh`, or none of these exports survive into your actual
# shell.
#
# LE_TOOLCHAIN_ROOT can be overridden before sourcing to use a prefix other
# than the default - rocky8-bootstrap.sh must have been run against the
# same root first.

if [ "${BASH_SOURCE:-}" = "${0:-}" ] || [ -n "${ZSH_EVAL_CONTEXT:-}" ] && [[ "$ZSH_EVAL_CONTEXT" != *:file:* ]]; then
    echo "rocky8-env.sh must be sourced, not executed: 'source ${0}'" >&2
    return 1 2>/dev/null || exit 1
fi

: "${LE_TOOLCHAIN_ROOT:=$HOME/.local/lef_editor_toolchain}"
export LE_TOOLCHAIN_ROOT

if [ ! -d "$LE_TOOLCHAIN_ROOT/root" ]; then
    echo "rocky8-env.sh: $LE_TOOLCHAIN_ROOT/root not found - run rocky8-bootstrap.sh first" >&2
    return 1 2>/dev/null || exit 1
fi

_le_root="$LE_TOOLCHAIN_ROOT/root"
_le_gcc_root="$_le_root/opt/rh/gcc-toolset-13/root/usr"

# --- Compiler (gcc-toolset-13, rootlessly extracted via rpm2cpio) ---
export CC="$_le_gcc_root/bin/gcc"
export CXX="$_le_gcc_root/bin/g++"
# Belt-and-braces alongside CC/CXX above - some tools (Skia's GN, Flutter's
# own build) invoke `gcc`/`g++` by bare name rather than respecting CC/CXX,
# so PATH needs the real one found first too. Reconfigure (fresh build
# directory) after changing CC/CXX on an existing build - CMake caches the
# compiler path at first configure and won't pick up a change to these
# variables on a stale build tree, the same class of gotcha as this
# project's own ENABLE_COVERAGE note in backend/CLAUDE.md.
export PATH="$_le_gcc_root/bin:$LE_TOOLCHAIN_ROOT/cmake/bin:$LE_TOOLCHAIN_ROOT/ninja:$_le_root/usr/bin:$PATH"

# --- Headers / libraries extracted from RPMs into $_le_root/usr, plus
# gcc-toolset's own runtime libs ---
export CPATH="$_le_root/usr/include${CPATH:+:$CPATH}"
export C_INCLUDE_PATH="$_le_root/usr/include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
export CPLUS_INCLUDE_PATH="$_le_root/usr/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
export LIBRARY_PATH="$_le_root/usr/lib64:$_le_gcc_root/lib64${LIBRARY_PATH:+:$LIBRARY_PATH}"
# Runtime, not just link-time: binaries built with gcc-toolset-13 link
# against *its own* newer libstdc++.so.6/libgcc_s.so.1, not RHEL8's base
# one - since build and run happen on the same machine here, this is
# simpler than static-linking libstdc++. If the eventual launch mechanism
# doesn't inherit a sourced shell environment (a desktop launcher, a
# systemd unit, ...), that specific launcher needs this same
# LD_LIBRARY_PATH set some other way, or switch to -static-libstdc++
# -static-libgcc instead.
export LD_LIBRARY_PATH="$_le_root/usr/lib64:$_le_gcc_root/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$_le_root/usr/lib64/pkgconfig:$_le_root/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- Vendored (upstream tarball) deps ---
export BOOST_ROOT="$LE_TOOLCHAIN_ROOT/boost"
export SKIA_DIR="$LE_TOOLCHAIN_ROOT/skia/skia"
# Tells backend/CMakeLists.txt's skia target (and both flutter_plugin
# CMakeLists.txt files, forwarded via frontend/linux/CMakeLists.txt) that
# this SKIA_DIR checkout was built with skia_use_system_harfbuzz/icu/
# libwebp/expat=false - see backend/CMakeLists.txt's own comment on
# LE_SKIA_VENDORS_THIRD_PARTY for why (RHEL8's harfbuzz/icu are too old
# relative to a modern Skia commit to trust as system libs).
export LE_SKIA_VENDORS_THIRD_PARTY=ON

unset _le_root _le_gcc_root

cat <<EOF
rocky8-env.sh: activated toolchain at $LE_TOOLCHAIN_ROOT
  CC             = $CC
  CXX            = $CXX
  BOOST_ROOT     = $BOOST_ROOT
  SKIA_DIR       = $SKIA_DIR
Configure the backend with:
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug -DSKIA_DIR=\${SKIA_DIR}
EOF
