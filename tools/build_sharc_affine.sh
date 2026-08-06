#!/bin/sh
# Build the minimal ADSP-21469 affine DLM with an activated CCES toolchain.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: CCES_ROOT=/path/to/cces tools/build_sharc_affine.sh OUTPUT_DIR" >&2
    exit 2
fi
if [ -z "${CCES_ROOT:-}" ]; then
    echo "refusing: CCES_ROOT is required" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUTPUT_DIR=$1
CC21K="$CCES_ROOT/cc21k.exe"
ELF2DYN="$CCES_ROOT/elf2dyn.exe"

if [ ! -f "$CC21K" ] || [ ! -f "$ELF2DYN" ]; then
    echo "refusing: CCES_ROOT does not contain cc21k.exe and elf2dyn.exe" >&2
    exit 2
fi

RUNNER=${CCES_RUNNER:-}
if [ -z "$RUNNER" ] && command -v wine >/dev/null 2>&1; then
    RUNNER=wine
fi

run_tool()
{
    tool=$1
    shift
    if [ -n "$RUNNER" ]; then
        "$RUNNER" "$tool" "$@"
    else
        "$tool" "$@"
    fi
}

mkdir -p "$OUTPUT_DIR"

run_tool "$CC21K" \
    -proc ADSP-21469 \
    -si-revision 0.2 \
    -O \
    -nwc \
    -no-fp-associative \
    -no-fx-contract \
    -no-std-lib \
    -T "$ROOT/kernels/affine/dynmod.ldf" \
    -o "$OUTPUT_DIR/affine.dxe" \
    "$ROOT/kernels/affine/affine.c"

python3 "$ROOT/tools/inspect_sharc_image.py" \
    "$OUTPUT_DIR/affine.dxe" \
    --format elf \
    --require-symbol _uad_affine_entry \
    --require-no-undefined \
    --max-code-bytes 0x600 > "$OUTPUT_DIR/affine-dxe.json"

run_tool "$ELF2DYN" \
    -o "$OUTPUT_DIR/affine.dyn" \
    -e _uad_affine_entry \
    "$OUTPUT_DIR/affine.dxe"

python3 "$ROOT/tools/inspect_sharc_image.py" \
    "$OUTPUT_DIR/affine.dyn" \
    --format dlm \
    --require-export-name _uad_affine_entry \
    --max-code-bytes 0x600 > "$OUTPUT_DIR/affine-dyn.json"

echo "built $OUTPUT_DIR/affine.dxe and $OUTPUT_DIR/affine.dyn"
