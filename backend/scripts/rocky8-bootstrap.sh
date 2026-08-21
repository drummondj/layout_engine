#!/usr/bin/env bash
# Assembles a rootless build toolchain for this repo on Rocky Linux 8 (or
# any dnf/RPM-based RHEL-8-family distro) with no root access and no
# ability to install system packages. Idempotent/rerunnable - each stage
# checks whether its own output already exists and skips if so, so a
# failed run can just be re-invoked after fixing whatever broke.
#
# Neither this script nor the plan it came from has been run against a
# real Rocky 8 machine - every RPM name, URL, and GN flag below is
# best-effort reasoning grounded in this repo's own actual CMake/GN files
# and general RHEL8 package knowledge, not empirical verification. Expect
# real failures; each stage below prints exactly what it was trying to do
# so a failure is easy to report back and iterate on, the same way this
# project's own Docker/Ubuntu Linux CI path (Dockerfile.linux-ci) took
# several real iteration rounds to get working even on a much more
# forgiving (full apt access) base image.
#
# Usage:
#   backend/scripts/rocky8-bootstrap.sh [stage ...]
# With no arguments, runs every stage in order. Pass one or more stage
# names (see STAGE_NAMES below) to run only those, e.g. to retry a single
# failed stage:
#   backend/scripts/rocky8-bootstrap.sh skia
#
# After this completes, `source backend/scripts/rocky8-env.sh` before
# configuring the actual CMake/Flutter builds.
#
# Every run's full output is also written to a timestamped log file under
# $LE_TOOLCHAIN_ROOT/logs/ (see BUILD.md at the repo root) - if a stage
# fails and you can't work out why, that log file is exactly what's worth
# sending back for help; its path is printed at both the start and end of
# every run so it's easy to find either way.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LE_TOOLCHAIN_ROOT="${LE_TOOLCHAIN_ROOT:-$HOME/.local/lef_editor_toolchain}"
STAGE_DOWNLOADS="$LE_TOOLCHAIN_ROOT/.rpm-downloads"
LE_ROOT="$LE_TOOLCHAIN_ROOT/root"
LOG_DIR="$LE_TOOLCHAIN_ROOT/logs"

STAGE_NAMES=(check-tools rpms cmake ninja boost bison swig skia)

log()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok:\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m  FAIL:\033[0m %s\n' "$*" >&2; }

mkdir -p "$LE_TOOLCHAIN_ROOT" "$STAGE_DOWNLOADS" "$LE_ROOT" "$LOG_DIR"

# Every run gets its own timestamped log (never overwritten), plus
# 'latest.log' always pointing at the most recent run for convenience.
# tee, not a plain redirect, so output still appears on the terminal live
# - this isn't a silent background capture.
LOG_FILE="$LOG_DIR/bootstrap-$(date +%Y%m%d-%H%M%S).log"
ln -sf "$(basename "$LOG_FILE")" "$LOG_DIR/latest.log"
echo "Logging this run to: $LOG_FILE"
exec > >(tee -a "$LOG_FILE") 2>&1

# --- RPM extraction helper: downloads <pkg> (+ its full resolved
# dependency closure) without root via `dnf download`, then rpm2cpio|cpio
# every downloaded .rpm into $LE_ROOT without installing anything. Safe to
# call more than once for the same package - re-downloads are cheap and
# re-extraction is idempotent (cpio just overwrites the same files).
# Unconditional - always fetches/extracts regardless of what's already on
# the system. Only meant for packages where that's actually the right
# call (see extract_rpm_closure_if_missing below for the alternative, and
# stage_rpms's own comments for which packages need which): the compiler
# and CMake/Skia's own vendored-vs-system choices are pinned to a *known*
# version deliberately, because RHEL8's own stock versions of those are
# either too old to work at all (GCC 8.5 has no real C++20/23 support) or
# a real, specific version-skew risk already reasoned through elsewhere
# (RHEL8's harfbuzz/icu vs. a modern Skia commit) - "is something already
# installed" isn't a good enough question for those; the actual version
# matters, not just presence. ---
# RPM package names whose closure gets pulled in transitively by basically
# everything on a RHEL-family system (via "Requires: filesystem" et al) but
# that ship nothing this toolchain actually needs - only the base OS
# directory skeleton. Skipped entirely rather than extracted: `cpio -d`
# already creates whatever real parent directories the packages we *do*
# need require, on demand. `filesystem` specifically ships several legacy
# placeholder directories (/usr/lib{,64}/tls, sse2, debug, games, locale,
# modules, sysimage, X11, bpf, pm-utils) with an explicit, non-owner-writable
# mode baked into the RPM's own file list - cpio applies that literal mode
# on mkdir, so as a non-root user every subsequent attempt to create
# anything under those paths (even by the owning user) fails EACCES. Add
# another package name here if a future closure hits the same pattern.
RPM_SKIP_PACKAGES=()

extract_rpm_closure() {
    local pkg="$1"; shift
    local extra_repo_args=("$@")
    local dest="$STAGE_DOWNLOADS/$pkg"
    mkdir -p "$dest"
    local exclude_args=()
    local skip
    for skip in "${RPM_SKIP_PACKAGES[@]}"; do
        exclude_args+=(--exclude="$skip")
    done
    log "dnf download --resolve ${exclude_args[*]} ${extra_repo_args[*]:-} $pkg"
    if ! dnf download --resolve "${exclude_args[@]}" --destdir="$dest" "${extra_repo_args[@]}" "$pkg" 2>&1; then
        fail "dnf download failed for $pkg - is it reachable? See this script's fail-fast checks in the plan this came from (dnf repo reachability, crb/PowerTools enablement)."
        return 1
    fi
    local rpm_count
    rpm_count=$(find "$dest" -name '*.rpm' | wc -l | tr -d ' ')
    if [ "$rpm_count" = "0" ]; then
        fail "dnf download for $pkg produced no .rpm files"
        return 1
    fi
    log "extracting $rpm_count RPM(s) for $pkg into $LE_ROOT"
    local rpm
    for rpm in "$dest"/*.rpm; do
        local rpm_base
        rpm_base="$(basename "$rpm")"
        for skip in "${RPM_SKIP_PACKAGES[@]}"; do
            if [[ "$rpm_base" == "$skip"-* ]]; then
                continue 2
            fi
        done
        (cd "$LE_ROOT" && rpm2archive -n "$rpm" && tar -xf $rpm.tar --no-same-owner --no-same-permissions  ) || {
            fail "rpm2archive|tar failed for $rpm"
            return 1
        }
        chmod -R ug+rw $LE_ROOT
    done
    ok "$pkg (+ dependency closure)"
}

# --- Same as extract_rpm_closure, but skips entirely if $pkg is already
# installed on the system (`rpm -q`, works without root - just a query,
# not an install). For version-stable packages where basically any
# version RHEL8 would have packaged at all is good enough (GTK3 + its
# closure, Tcl/Tk, the Skia system-linked subset) - unlike
# extract_rpm_closure's own callers, there's no specific minimum version
# these need beyond "exists". Checking only the top-level package, not
# its transitive closure, is deliberately sufficient: dnf/rpm itself
# already enforces that a properly-installed package's own declared
# dependencies are satisfied, so if $pkg shows as installed at all, its
# closure is already present too - no need to re-derive and re-check it
# ourselves. If $pkg is missing OR too old for what's actually needed,
# falls through to a real extract_rpm_closure call. ---
extract_rpm_closure_if_missing() {
    local pkg="$1"
    if rpm -q "$pkg" >/dev/null 2>&1; then
        ok "$pkg already installed on this system ($(rpm -q "$pkg")) - skipping download/extract"
        return 0
    fi
    extract_rpm_closure "$@"
}

stage_check_tools() {
    log "checking for dnf/rpm2archive/tar/curl/git - required by this script itself"
    local missing=()
    for tool in dnf rpm2archive curl git tar; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        fail "missing required tool(s): ${missing[*]} - these are expected to already be present on a normal Rocky 8 install; if not, this bootstrap approach doesn't apply here"
        return 1
    fi
    if ! command -v dnf-plugins-core >/dev/null 2>&1; then
        dnf download --help >/dev/null 2>&1 || {
            fail "'dnf download' isn't available - the dnf-plugins-core package (or equivalent) may not be installed. Ask whoever manages this machine's base image whether it's present; this script can't install it (that would need root)."
            return 1
        }
    fi
    ok "required tools present"
}

stage_rpms() {
    log "Stage: RPM-extracted dependencies (compiler, Tcl/Tk, GTK3 + closure, Skia's system-linked subset)"

    # --- Compiler: gcc-toolset-13 (self-contained under /opt/rh/gcc-toolset-13).
    # Always extracted regardless of what's already installed - RHEL8's
    # stock GCC (8.5) can't do C++20/23 at all, so "is a compiler already
    # present" isn't the right question; it has to be *this* version. ---
    extract_rpm_closure gcc-toolset-13-gcc-c++ || return 1
    extract_rpm_closure gcc-toolset-13-libstdc++-devel || return 1
    extract_rpm_closure gcc-toolset-13-binutils || return 1
    # extract_rpm_closure gcc-toolset-13-make || return 1

    # --- Everything below this point is a version-stable package where
    # "is it already installed" is a good enough question - basically any
    # version RHEL8 would have packaged works fine for these, unlike the
    # compiler above or Skia's harfbuzz/icu/libwebp/expat choice (a real,
    # specific version-skew concern, handled separately via
    # LE_SKIA_VENDORS_THIRD_PARTY - see backend/CMakeLists.txt's own
    # comment). Checked via extract_rpm_closure_if_missing (see its own
    # comment) rather than unconditionally extracting a redundant private
    # copy on top of a perfectly good system one - a real, if harmless,
    # waste this stage used to always pay regardless of what the machine
    # already had, especially likely for GTK3's own large closure on a
    # machine that (per its confirmed real display) probably already has
    # a working desktop GTK3 stack. ---

    # --- Tcl/Tk (real runtime need - le_tcl_bridge.cpp #includes tcl.h
    # directly, and the le_tcl SWIG target links against it) - likely
    # behind the crb repo (Rocky's name for RHEL's
    # codeready-builder-for-rhel-8-*-rpms - verify the repo id on the real
    # machine if this fails; it varies by RHEL-family distro/variant). ---
    # extract_rpm_closure_if_missing tcl-devel --enablerepo=crb || return 1
    # extract_rpm_closure_if_missing tk-devel --enablerepo=crb || return 1
    extract_rpm_closure_if_missing tcl-devel || return 1
    extract_rpm_closure_if_missing tk-devel || return 1

    # --- GTK3 + transitive closure (Flutter's Linux embedder is
    # fundamentally GTK3-based). This hand list is a sanity check, not
    # authoritative - `dnf download --resolve --alldeps gtk3-devel` above
    # is what actually determines the real closure; the extra packages
    # below cover things gtk3-devel's own closure may not pull in
    # directly (Mesa GL/EGL for Flutter's own compositor, separate from
    # this project's GPU-free Skia raster path). ---
    extract_rpm_closure_if_missing gtk3-devel || return 1
    extract_rpm_closure_if_missing mesa-libGL-devel || return 1
    extract_rpm_closure_if_missing mesa-libEGL-devel || return 1
    extract_rpm_closure_if_missing mesa-dri-drivers || return 1
    extract_rpm_closure_if_missing mesa-libgbm || return 1

    # --- Skia's system-linked subset only (frozen/stable-ABI ones -
    # harfbuzz/icu/libwebp/expat are vendored from Skia's own third_party/
    # sources instead, see the skia stage below, since RHEL8's versions of
    # those are old enough relative to a modern Skia commit to be a real
    # risk - see backend/CMakeLists.txt's LE_SKIA_VENDORS_THIRD_PARTY
    # comment). freetype-devel is the riskiest of this subset (RHEL8's is
    # ~2.9.1) - if Skia link/API errors mention it specifically, that's
    # the first thing to also flip to vendored (skia_use_system_freetype2
    # isn't wired to LE_SKIA_VENDORS_THIRD_PARTY today - would need adding
    # if it comes to that). ---
    extract_rpm_closure_if_missing zlib-devel || return 1
    extract_rpm_closure_if_missing libpng-devel || return 1
    extract_rpm_closure_if_missing libjpeg-turbo-devel || return 1
    extract_rpm_closure_if_missing freetype-devel || return 1

    ok "all RPM-extracted dependencies staged under $LE_ROOT"
}

stage_cmake() {
    local version="3.28.3"
    local dir="$LE_TOOLCHAIN_ROOT/cmake"
    if [ -x "$dir/bin/cmake" ]; then
        ok "cmake already present at $dir (skipping - remove that directory to force a re-fetch)"
        return 0
    fi
    log "fetching CMake $version (upstream relocatable release, not the RHEL8 package - backend/CMakeLists.txt needs >=3.20, which RHEL8's own cmake can't be trusted to satisfy)"
    local tarball="$STAGE_DOWNLOADS/cmake-$version-linux-x86_64.tar.gz"
    curl -fL -o "$tarball" \
        "https://github.com/Kitware/CMake/releases/download/v$version/cmake-$version-linux-x86_64.tar.gz" \
        || { fail "download failed - is github.com reachable from this machine?"; return 1; }
    mkdir -p "$dir"
    tar -xzf "$tarball" -C "$dir" --strip-components=1 || { fail "extract failed"; return 1; }
    ok "cmake $version -> $dir"
}

stage_ninja() {
    local version="1.12.1"
    local dir="$LE_TOOLCHAIN_ROOT/ninja"
    if [ -x "$dir/ninja" ]; then
        ok "ninja already present at $dir (skipping)"
        return 0
    fi
    log "fetching Ninja $version (upstream relocatable release)"
    local zipfile="$STAGE_DOWNLOADS/ninja-linux.zip"
    curl -fL -o "$zipfile" \
        "https://github.com/ninja-build/ninja/releases/download/v$version/ninja-linux.zip" \
        || { fail "download failed"; return 1; }
    mkdir -p "$dir"
    (cd "$dir" && unzip -o "$zipfile" >/dev/null) || { fail "unzip failed - is 'unzip' installed?"; return 1; }
    chmod +x "$dir/ninja"
    ok "ninja $version -> $dir"
}

stage_boost() {
    local version="1.86.0"
    local version_us="${version//./_}"
    local dir="$LE_TOOLCHAIN_ROOT/boost"
    if [ -d "$dir/boost" ]; then
        ok "boost already present at $dir (skipping)"
        return 0
    fi
    log "fetching Boost $version headers (upstream tarball - used header-only here, Boost.Geometry only, so no compiled Boost libraries are needed)"
    local tarball="$STAGE_DOWNLOADS/boost_$version_us.tar.gz"
    curl -fL -o "$tarball" \
        "https://archives.boost.io/release/$version/source/boost_$version_us.tar.gz" \
        || { fail "download failed - if archives.boost.io is unreachable, try the GitHub mirror: https://github.com/boostorg/boost/releases"; return 1; }
    mkdir -p "$dir"
    tar -xzf "$tarball" -C "$dir" --strip-components=1 || { fail "extract failed"; return 1; }
    ok "boost $version headers -> $dir (BOOST_ROOT)"
}

stage_bison() {
    local version="3.8.2"
    if [ -x "$LE_ROOT/usr/bin/bison" ]; then
        ok "bison already present at $LE_ROOT/usr/bin/bison (skipping - remove that file to force a rebuild)"
        return 0
    fi
    log "building GNU Bison $version from source (RHEL8's own bison is 3.0.4; SWIG 4.2.1's grammar declares '%require \"3.5\"' in Source/CParse/parser.y, and no newer bison ships in RHEL8's default or CRB repos - confirmed by a real build failure: 'error: require bison 3.5, but have 3.0.4')"
    if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ]; then
        fail "CC/CXX not set - run the gcc-toolset stage (part of 'rpms') and 'source backend/scripts/rocky8-env.sh' before running this stage"
        return 1
    fi
    local src_dir="$STAGE_DOWNLOADS/bison-$version"
    if [ ! -d "$src_dir" ]; then
        local tarball="$STAGE_DOWNLOADS/bison-$version.tar.gz"
        curl -fL -o "$tarball" \
            "https://ftp.gnu.org/gnu/bison/bison-$version.tar.gz" \
            || { fail "download failed - is ftp.gnu.org reachable?"; return 1; }
        mkdir -p "$src_dir"
        tar -xzf "$tarball" -C "$src_dir" --strip-components=1 || { fail "extract failed"; return 1; }
    fi
    (
        cd "$src_dir" &&
        ./configure --prefix="$LE_ROOT/usr" &&
        make -j "$(nproc)" &&
        make install
    ) || { fail "bison build failed - see output above"; return 1; }
    ok "bison $version -> $LE_ROOT/usr/bin/bison"
}

stage_swig() {
    local version="4.2.1"
    if command -v "$LE_ROOT/usr/bin/swig" >/dev/null 2>&1; then
        ok "swig already present at $LE_ROOT/usr/bin/swig (skipping)"
        return 0
    fi
    log "building SWIG $version from source (not gambling on EPEL reachability - see this script's own header comment. --without-pcre2 avoids a whole extra library dependency and SWIG's core functionality doesn't need it)"
    if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ]; then
        fail "CC/CXX not set - run the gcc-toolset stage (part of 'rpms') and 'source backend/scripts/rocky8-env.sh' before running this stage, so SWIG itself builds with the same modern compiler as everything else"
        return 1
    fi
    if ! command -v bison >/dev/null 2>&1; then
        fail "bison not found on PATH - run the bison stage first (backend/scripts/rocky8-bootstrap.sh bison), then re-source rocky8-env.sh"
        return 1
    fi
    local bison_version
    bison_version=$(bison --version | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)
    if [ -z "$bison_version" ] || ! printf '%s\n%s\n' "3.5" "$bison_version" | sort -C -V; then
        fail "bison on PATH is $bison_version, but SWIG's grammar requires >=3.5 (RHEL8's stock bison is 3.0.4, too old). Run the bison stage (backend/scripts/rocky8-bootstrap.sh bison), then re-source rocky8-env.sh so \$LE_ROOT/usr/bin's newer bison shadows the system one on PATH."
        return 1
    fi
    local src_dir="$STAGE_DOWNLOADS/swig-$version"
    if [ ! -d "$src_dir" ]; then
        local tarball="$STAGE_DOWNLOADS/swig-$version.tar.gz"
        curl -fL -o "$tarball" \
            "https://github.com/swig/swig/archive/refs/tags/v$version.tar.gz" \
            || { fail "download failed"; return 1; }
        mkdir -p "$src_dir"
        tar -xzf "$tarball" -C "$src_dir" --strip-components=1 || { fail "extract failed"; return 1; }
    fi
    (
        cd "$src_dir" &&
        ./autogen.sh &&
        ./configure --without-pcre2 --prefix="$LE_ROOT/usr" &&
        make -j "$(nproc)" &&
        make install
    ) || { fail "SWIG build failed - see output above"; return 1; }
    ok "swig $version -> $LE_ROOT/usr/bin/swig"
}

stage_skia() {
    local dir="$LE_TOOLCHAIN_ROOT/skia"
    local skia_dir="$dir/skia"
    if [ -f "$skia_dir/out/Linux/libskia.a" ]; then
        ok "libskia.a already built at $skia_dir/out/Linux (skipping - remove that file to force a rebuild)"
        return 0
    fi
    if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ]; then
        fail "CC/CXX not set - source backend/scripts/rocky8-env.sh (after the rpms/cmake/ninja/swig stages) before running this stage"
        return 1
    fi
    log "cloning/building Skia (raster-only, no GPU backend - matches this project's macOS/Docker Skia builds exactly, see Dockerfile.linux-ci)"
    mkdir -p "$dir"
    if [ ! -d "$skia_dir/.git" ]; then
        git clone https://skia.googlesource.com/skia.git "$skia_dir" || { fail "clone failed - is skia.googlesource.com reachable?"; return 1; }
    fi
    (
        cd "$skia_dir" &&
        # Same pinned commit as Dockerfile.linux-ci's own Skia stage - keep
        # these in sync by hand if that commit is ever bumped there.
        git checkout 9f330f1704305686dafa9eeef11de77caa5314b1 &&
        python3 tools/git-sync-deps &&
        python3 bin/fetch-gn &&
        bin/gn gen out/Linux --args="
            is_official_build=true
            is_component_build=false
            is_debug=false
            target_cpu=\"x64\"
            cc=\"$CC\"
            cxx=\"$CXX\"
            skia_enable_gpu=false
            skia_use_gl=false
            skia_use_vulkan=false
            skia_use_egl=false
            skia_enable_fontmgr_custom_directory=true
            skia_enable_fontmgr_custom_embedded=false
            skia_enable_fontmgr_custom_empty=false
            skia_use_fontconfig=false
            skia_use_freetype=true
            skia_use_system_freetype2=true
            skia_use_system_harfbuzz=false
            skia_use_system_libjpeg_turbo=true
            skia_use_system_libpng=true
            skia_use_system_libwebp=false
            skia_use_system_zlib=true
            skia_use_system_icu=false
            skia_use_system_expat=false
        " &&
        ninja -C out/Linux -j "$(nproc)" skia
    ) || {
        fail "Skia GN configure or ninja build failed - see output above. Common cause: a system-linked header (freetype/jpeg/png/zlib, extracted into \$LE_TOOLCHAIN_ROOT/root/usr) not found at its expected path - may need extra_cflags/extra_ldflags added to the bin/gn gen args above pointing at \$LE_ROOT/usr/include and \$LE_ROOT/usr/lib64, the same way Dockerfile.linux-ci already needed one such flag for Debian's own nonstandard harfbuzz header layout."
        return 1
    }
    ok "Skia built -> $skia_dir (SKIA_DIR)"
}

run_stage() {
    case "$1" in
        check-tools) stage_check_tools ;;
        rpms)        stage_rpms ;;
        cmake)       stage_cmake ;;
        ninja)       stage_ninja ;;
        boost)       stage_boost ;;
        bison)       stage_bison ;;
        swig)        stage_swig ;;
        skia)        stage_skia ;;
        *)
            fail "unknown stage '$1' - valid stages: ${STAGE_NAMES[*]}"
            return 1
            ;;
    esac
}

main() {
    local stages=("$@")
    if [ "${#stages[@]}" -eq 0 ]; then
        stages=("${STAGE_NAMES[@]}")
    fi

    local failed=()
    for stage in "${stages[@]}"; do
        if ! run_stage "$stage"; then
            failed+=("$stage")
            # cmake/ninja/swig/skia all need CC/CXX/PATH from a sourced
            # rocky8-env.sh - if rpms just landed in this same invocation,
            # source it now so later stages in this same run see the
            # gcc-toolset compiler without requiring a second manual step.
        fi
        if [ "$stage" = "rpms" ] && [ -x "$LE_TOOLCHAIN_ROOT/root/opt/rh/gcc-toolset-13/root/usr/bin/gcc" ]; then
            # shellcheck disable=SC1091
            source "$SCRIPT_DIR/rocky8-env.sh" >/dev/null 2>&1 || true
        fi
    done

    echo
    if [ "${#failed[@]}" -eq 0 ]; then
        log "All requested stages succeeded. Next: source backend/scripts/rocky8-env.sh"
        echo "Full log: $LOG_FILE"
    else
        fail "Stage(s) failed: ${failed[*]}"
        echo "Re-run just the failed stage(s) after fixing whatever broke, e.g.:" >&2
        echo "  $0 ${failed[*]}" >&2
        echo "Full log (this is what's worth sending back for help): $LOG_FILE" >&2
        exit 1
    fi
}

main "$@"
