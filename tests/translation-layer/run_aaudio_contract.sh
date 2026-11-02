#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 "$ROOT/tests/translation-layer/extract_aaudio.py" "$OUT/production_aaudio.inc"
clang -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Wno-unused-function -I"$OUT" "$ROOT/tests/translation-layer/aaudio_contract.c" -pthread -o "$OUT/test"
"$OUT/test"
