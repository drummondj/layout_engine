#!/bin/sh
# The bundle's le_shell: starts le_shell.bin beside it with the shared
# libraries this system lacks taken from runtime/<soname>/. A library the
# system provides is used as it is, except the C++ runtime (libstdc++,
# libgcc_s), which is taken from the bundle when the system's is older than
# the one Layout Engine was built with. The system's GL driver loads into
# the same process, so its own C++ runtime must not be replaced by an older
# one either: the newer of the two always wins. A bundled library brings the
# other bundled libraries it was built against (runtime/<soname>/needs).
#
# LE_BUNDLED_LIBRARIES=all uses every bundled library, =none none of them.
# le_shell.bin restores the caller's LD_LIBRARY_PATH (LE_SHELL_LD_LIBRARY_PATH)
# once it has started, so programs it runs don't inherit the bundle's.

# Its own commands (readlink, awk, ldconfig, ...) fall back to the system's
# directories when the caller's PATH lacks them; le_shell.bin gets the
# caller's PATH back.
caller_path=$PATH
PATH=${PATH:+$PATH:}/usr/bin:/bin:/usr/sbin:/sbin

here=$(dirname "$(readlink -f "$0")")

case $(uname -m) in
    x86_64) arch=x86-64 ;;
    aarch64) arch=AArch64 ;;
    *) arch= ;;
esac
system_libraries=$( (ldconfig -p || /sbin/ldconfig -p) 2>/dev/null)

# system_library <soname>: the path of the system's copy, if it has one.
system_library() {
    printf '%s\n' "$system_libraries" | awk -v soname="$1" -v arch="$arch" \
        '$1 == soname && index($0, arch) { print $NF; exit }'
}

# newest_version <prefix> <file>: the highest <prefix>N.N.N version a library defines.
newest_version() {
    grep -aoE "$1[0-9]+(\.[0-9]+)*" "$2" 2>/dev/null | sed "s/^$1//" |
        sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1
}

# is_older <a> <b>: whether version a is lower than version b.
is_older() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$1" ]
}

paths=
for directory in "$here"/runtime/lib*.so*; do
    [ -d "$directory" ] || continue
    soname=${directory##*/}
    use=
    case ${LE_BUNDLED_LIBRARIES:-auto} in
        all) use=1 ;;
        none) ;;
        *)
            system=$(system_library "$soname")
            if [ -z "$system" ]; then
                use=1
            else
                case $soname in
                    libstdc++.so.*) prefix=GLIBCXX_ ;;
                    libgcc_s.so.*) prefix=GCC_ ;;
                    *) prefix= ;;
                esac
                if [ -n "$prefix" ] && is_older "$(newest_version "$prefix" "$system")" "$(newest_version "$prefix" "$directory/$soname")"; then
                    use=1
                fi
            fi
            ;;
    esac
    if [ -n "$use" ]; then
        paths=$paths${paths:+:}$directory
        case $soname in
            libtcl*) [ -d "$here/runtime/tcl-library" ] && export LE_SHELL_TCL_LIBRARY="$here/runtime/tcl-library" ;;
        esac
    fi
done

# A bundled library takes its bundled dependencies along (runtime/<soname>/needs).
chosen=$paths
saved_ifs=$IFS
IFS=:
for directory in $chosen; do
    IFS=$saved_ifs
    if [ -f "$directory/needs" ]; then
        for needed in $(cat "$directory/needs"); do
            case ":$paths:" in
                *":$here/runtime/$needed:"*) ;;
                *) [ -d "$here/runtime/$needed" ] && paths=$paths:$here/runtime/$needed ;;
            esac
        done
    fi
    IFS=:
done
IFS=$saved_ifs

if [ -n "$paths" ]; then
    export LE_SHELL_LD_LIBRARY_PATH="${LD_LIBRARY_PATH-}"
    export LD_LIBRARY_PATH="$paths${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
PATH=$caller_path
exec "$here/le_shell.bin" "$@"
