#!/usr/bin/env bash
set -euo pipefail

# Runs a .tcl script against the real running Flutter app (via
# integration_test/tcl_script_profile_test.dart) with Tracy capturing the
# whole session, then exports the resulting trace to CSV - see that test
# file's own comment for why this has to go through the real app rather
# than le_shell (no GUI there, so le_render_pixel_buffer never runs).
#
# Requires the Homebrew `tracy` formula (`brew install tracy`) for
# tracy-capture/tracy-csvexport - pinned to the same 0.13.1
# backend/CMakeLists.txt's own vendored Tracy uses, so trace files are
# protocol-compatible.
#
# Usage: scripts/profile_tcl_script.sh <script.tcl> [output-name]
#   <script.tcl>   Tcl script to run against the live app.
#   [output-name]  Basename (no extension) for the .tracy/.csv files under
#                  build/tracy_profiles/ - defaults to the script's own
#                  stem plus a timestamp.

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <script.tcl> [output-name]" >&2
    exit 1
fi

SCRIPT_PATH="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
if [[ ! -f "$SCRIPT_PATH" ]]; then
    echo "Tcl script not found: $SCRIPT_PATH" >&2
    exit 1
fi

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND_DIR="$(dirname "$SELF_DIR")"

OUTPUT_NAME="${2:-$(basename "$SCRIPT_PATH" .tcl)-$(date +%Y%m%d-%H%M%S)}"
OUT_DIR="$FRONTEND_DIR/build/tracy_profiles"
mkdir -p "$OUT_DIR"
TRACE_FILE="$OUT_DIR/$OUTPUT_NAME.tracy"
CSV_FILE="$OUT_DIR/$OUTPUT_NAME.csv"

TRACY_CAPTURE="$(command -v tracy-capture || true)"
TRACY_CSVEXPORT="$(command -v tracy-csvexport || true)"
if [[ -z "$TRACY_CAPTURE" || -z "$TRACY_CSVEXPORT" ]]; then
    echo "tracy-capture/tracy-csvexport not found on PATH - install via 'brew install tracy'" >&2
    exit 1
fi

echo "Starting tracy-capture -> $TRACE_FILE"
"$TRACY_CAPTURE" -o "$TRACE_FILE" -f &
CAPTURE_PID=$!

# tracy-capture's own tracy::Worker retries the connection internally
# (see capture/src/capture.cpp) - safe to start before the app exists to
# connect to; it just waits. Idempotent-safe cleanup (checks the process
# is still alive first) stays armed for the whole script, not just around
# the `wait` below, in case something above fails unexpectedly first.
cleanup() {
    if kill -0 "$CAPTURE_PID" 2>/dev/null; then
        kill "$CAPTURE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Running integration test against the real app (TCL_SCRIPT_PATH=$SCRIPT_PATH)"
set +e
(
    cd "$FRONTEND_DIR"
    flutter test integration_test/tcl_script_profile_test.dart -d macos \
        --dart-define=TCL_SCRIPT_PATH="$SCRIPT_PATH"
)
TEST_STATUS=$?
set -e

# tracy-capture's own main loop is `while (worker.IsConnected())` - it
# exits on its own once the app process (its Tracy client) disconnects,
# which the `flutter test` run above already caused, one way or another.
# Wait for it to actually finish writing $TRACE_FILE before exporting.
wait "$CAPTURE_PID"

echo "Exporting $TRACE_FILE -> $CSV_FILE"
"$TRACY_CSVEXPORT" "$TRACE_FILE" > "$CSV_FILE"

echo "Done."
echo "  Trace: $TRACE_FILE"
echo "  CSV:   $CSV_FILE"

tracy $TRACE_FILE

exit "$TEST_STATUS"
