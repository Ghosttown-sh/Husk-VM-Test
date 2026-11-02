#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 "$ROOT/tests/translation-layer/extract_vm_native.py" "$OUT/production_vm.inc"
clang -O1 -ffunction-sections -fdata-sections -I"$OUT" -I"$ROOT/src/translation-layer-next" -I"$ROOT/src/translation-layer" \
  "$ROOT/tests/translation-layer/vm_data_macos.c" "$ROOT/src/translation-layer-next/husk-tl-vm.c" \
  -Wl,-dead_strip -Wl,-undefined,dynamic_lookup -o "$OUT/test"
"$OUT/test"
