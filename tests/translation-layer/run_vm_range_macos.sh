#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 "$ROOT/tests/translation-layer/extract_vm_native.py" "$OUT/production_vm.inc"
clang -O1 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -ffunction-sections -fdata-sections \
  -I"$OUT" -I"$ROOT/src/translation-layer-next" -I"$ROOT/src/translation-layer" \
  "$ROOT/tests/translation-layer/vm_range_macos.c" -Wl,-dead_strip -Wl,-undefined,dynamic_lookup -o "$OUT/test"
"$OUT/test"
