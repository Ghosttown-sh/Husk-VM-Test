#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -D_DARWIN_C_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=undefined \
  "$ROOT/tests/translation-layer/jit_pool_macos.c" -o "$OUT/test"
"$OUT/test"
