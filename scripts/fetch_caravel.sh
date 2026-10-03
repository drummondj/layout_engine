#!/usr/bin/env bash
set -euo pipefail

# Downloads the caravel full-chip test case (tcl/caravel.tcl) into
# test_data/caravel/: the top-level, pad-ring and core DEFs and the macro
# LEFs from efabless/caravel, RAM128 from caravel_mgmt_soc_litex, and the
# sky130A tech, standard-cell and IO LEFs from the volare build of the
# open_pdks commit caravel's Makefile pins. Every source is pinned to a
# commit so the data doesn't drift.
#
# Usage: fetch_caravel.sh
# Needs curl, gzip, and either zstd or Python's zstandard module.

cd "$(dirname "$0")/.."

CARAVEL_SHA=27cbe49c90ba5362ad52c9968dd98e035c30c74f
MGMT_SOC_SHA=503eda0790085712ffef7f4ad8934c7daed3237f
OPEN_PDKS_SHA=12df12e2e74145e31c5a13de02f9a1e176b56e67

CARAVEL=https://raw.githubusercontent.com/efabless/caravel/$CARAVEL_SHA
MGMT_SOC=https://raw.githubusercontent.com/efabless/caravel_mgmt_soc_litex/$MGMT_SOC_SHA
VOLARE=https://github.com/chipfoundry/volare/releases/download/sky130-$OPEN_PDKS_SHA

# Every macro caravel.def, chip_io.def and caravel_core.def place that
# isn't a sky130 library cell.
MACRO_LEFS=(
  caravel_clocking caravel_core caravel_logo-stub caravel_motto-stub
  chip_io constant_block copyright_block-stub empty_macro
  gpio_defaults_block gpio_logic_high housekeeping
  manual_power_connections mgmt_protect_hv mprj2_logic_high
  mprj_io_buffer mprj_logic_high open_source-stub simple_por
  spare_logic_block user_id_programming user_id_textblock-stub
  user_project_wrapper xres_buf
)

OUT=test_data/caravel
mkdir -p "$OUT/def" "$OUT/lef" "$OUT/pdk"

fetch() { curl -sSfL -o "$2" "$1"; }

for def in caravel chip_io; do
  fetch "$CARAVEL/def/$def.def" "$OUT/def/$def.def"
done
fetch "$CARAVEL/def/caravel_core.def.gz" - | gzip -dc > "$OUT/def/caravel_core.def"

for lef in "${MACRO_LEFS[@]}"; do
  fetch "$CARAVEL/lef/$lef.lef" "$OUT/lef/$lef.lef"
done
fetch "$MGMT_SOC/lef/RAM128.lef" "$OUT/lef/RAM128.lef"

# Writes a .tar.zst's named members, flattened, into $OUT/pdk.
untar_zst() {
  local archive=$1; shift
  if command -v zstd >/dev/null; then
    zstd -dc "$archive" | tar -x -C "$OUT/pdk" --transform='s|.*/||' "$@"
  else
    python3 - "$archive" "$OUT/pdk" "$@" <<'EOF'
import os, sys, tarfile, zstandard
archive, dest, members = sys.argv[1], sys.argv[2], set(sys.argv[3:])
with open(archive, 'rb') as f, tarfile.open(
        fileobj=zstandard.ZstdDecompressor().stream_reader(f), mode='r|') as t:
    for m in t:
        if m.name in members:
            with open(os.path.join(dest, os.path.basename(m.name)), 'wb') as o:
                o.write(t.extractfile(m).read())
EOF
  fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fetch "$VOLARE/sky130_fd_sc_hd.tar.zst" "$tmp/sc_hd.tar.zst"
untar_zst "$tmp/sc_hd.tar.zst" \
  sky130A/libs.ref/sky130_fd_sc_hd/techlef/sky130_fd_sc_hd__nom.tlef \
  sky130A/libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef \
  sky130A/libs.ref/sky130_fd_sc_hd/lef/sky130_ef_sc_hd.lef

fetch "$VOLARE/sky130_fd_io.tar.zst" "$tmp/fd_io.tar.zst"
untar_zst "$tmp/fd_io.tar.zst" \
  sky130A/libs.ref/sky130_fd_io/lef/sky130_fd_io.lef \
  sky130A/libs.ref/sky130_fd_io/lef/sky130_ef_io.lef

echo "caravel test data written to $OUT"
