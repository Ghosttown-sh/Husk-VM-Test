#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 "$ROOT/tests/translation-layer/extract_jni_alloc.py" "$OUT/production_jni_alloc.inc"
clang -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
  -I"$OUT" -I"$ROOT/src/translation-layer-next" "$ROOT/tests/translation-layer/jni_alloc.c" -o "$OUT/test"
"$OUT/test"
