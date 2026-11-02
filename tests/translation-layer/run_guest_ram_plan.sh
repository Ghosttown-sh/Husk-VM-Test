#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
swiftc "$ROOT/src/app/Husk/GuestRAMPlan.swift" "$ROOT/tests/translation-layer/guest_ram_plan.swift" -o "$OUT/test"
"$OUT/test"
