#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/husk-apk-path-build.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$ROOT/src/translation-layer-next" \
    "$ROOT/tests/translation-layer/apk_path.c" "$ROOT/src/translation-layer-next/husk-tl-apk-path.c" -o "$BUILD/test"
"$BUILD/test"
