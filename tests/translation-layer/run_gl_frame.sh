#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror "$ROOT/tests/translation-layer/gl_frame.c" -o "$OUT/test"
"$OUT/test"
