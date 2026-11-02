#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
  -I"$ROOT/tests/translation-layer/fake-audio" "$ROOT/tests/translation-layer/audio_producers.c" -pthread -o "$OUT/test"
"$OUT/test"
