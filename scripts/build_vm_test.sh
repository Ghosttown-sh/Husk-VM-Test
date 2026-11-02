#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the isolated ARM64 VM test IPA on a Mac with Xcode 26+.
# No GitHub account, CI provider, Apple signing secret, or paid service required.
# Prerequisites: Xcode (selected), Swift 6.4+, Python 3.11+, XcodeGen, qemu-img, Rust/rustup.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/Husk-VM-Test.ipa}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
export DD="${DD:-$ROOT/build/derived-vm-test}"
export HUSK_TEST_BUILD=1

if [ "$(uname -s)" != Darwin ]; then
    echo "This build needs a Mac with Xcode 26 or later." >&2
    exit 1
fi
echo "Build host: $(uname -m); app target: ARM64 iOS device"
echo "Darwin tests report this host's page size; 4 KiB results do not verify a 16 KiB host."
for tool in xcodebuild xcrun clang swiftc python3 xcodegen qemu-img cargo rustup codesign ditto; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
XCODE_VERSION=$(xcodebuild -version)
echo "$XCODE_VERSION"
XCODE_MAJOR=$(printf '%s\n' "$XCODE_VERSION" | awk '/^Xcode / { split($2, v, "."); print v[1] }')
if [ -z "$XCODE_MAJOR" ] || [ "$XCODE_MAJOR" -lt 26 ]; then
    echo "Select Xcode 26+ with DEVELOPER_DIR or xcode-select before building." >&2
    exit 1
fi
xcrun --sdk iphoneos --show-sdk-version
SWIFT_VERSION=$(xcrun swiftc --version)
echo "$SWIFT_VERSION"
printf '%s\n' "$SWIFT_VERSION" | python3 -c '
import re, sys
m = re.search(r"Swift version (\d+)\.(\d+)", sys.stdin.read())
assert m and tuple(map(int, m.groups())) >= (6, 4), "Vendored StikJIT requires Swift 6.4+; select a compatible TOOLCHAINS value"
'
python3 -c 'import sys; assert sys.version_info >= (3, 11), "Python 3.11+ required"'

cd "$ROOT"
mkdir -p build/evidence "$DD" "$(dirname "$OUT")"
python3 tests/translation-layer/run_vm_pressure.py --cc clang 2>&1 | tee build/evidence/vm-policy.log
sh tests/translation-layer/run_vm_pressure_macos.sh 2>&1 | tee build/evidence/vm-darwin.log
sh tests/translation-layer/run_vm_data_macos.sh 2>&1 | tee build/evidence/vm-data-jit.log
sh tests/translation-layer/run_audio_producers.sh 2>&1 | tee build/evidence/audio-producers.log
sh tests/translation-layer/run_vm_gap_macos.sh 2>&1 | tee build/evidence/vm-gap-darwin.log
sh tests/translation-layer/run_vm_aligned_macos.sh 2>&1 | tee build/evidence/vm-aligned-darwin.log
sh tests/translation-layer/run_vm_range_macos.sh 2>&1 | tee build/evidence/vm-range-darwin.log
sh tests/translation-layer/run_compressed_heap.sh 2>&1 | tee build/evidence/compressed-heap.log
sh tests/translation-layer/run_guest_ram_plan.sh 2>&1 | tee build/evidence/qemu-ram.log
sh tests/translation-layer/run_apk_path.sh 2>&1 | tee build/evidence/apk-path.log
sh tests/translation-layer/run_gl_frame.sh 2>&1 | tee build/evidence/gl-frame.log
if [ "$(uname -m)" = arm64 ]; then
    python3 tests/translation-layer/run_gl_frame_angle_macos.py 2>&1 | tee build/evidence/gl-frame-metal.log
fi
clang -std=c11 -Wall -Wextra -Isrc/translation-layer tests/translation-layer/plan_test.c \
    src/translation-layer/husk-tl-elf.c -o build/plan-test
build/plan-test | tee build/evidence/page-plan.log

# Public release dependencies only. A local verified copy works offline too.
OFFICIAL_IPA="${HUSK_OFFICIAL_IPA:-$ROOT/build/official-3.3.ipa}"
if [ ! -f "$OFFICIAL_IPA" ]; then
    mkdir -p "$(dirname "$OFFICIAL_IPA")"
    curl --fail --location --retry 3 \
        https://github.com/Leviidev/Husk/releases/download/3.3/Husk.ipa -o "$OFFICIAL_IPA"
fi
python3 tools/stage_vm_test_dependencies.py "$OFFICIAL_IPA"
rustup target add aarch64-apple-ios
bash scripts/build_rppairing_ios.sh

# Publish the final local output only after all structural/signature checks pass.
CANDIDATE="$DD/Husk-VM-Test-candidate.ipa"
bash scripts/package_ipa.sh "$CANDIDATE"
COMMIT=$(git rev-parse --short HEAD)
git diff --quiet || COMMIT="$COMMIT-dirty"
python3 tools/validate_vm_test_ipa.py "$CANDIDATE" --commit "$COMMIT" | tee build/evidence/ipa-validation.log
VERIFY=$(mktemp -d "$DD/verify.XXXXXX")
trap 'rm -rf "$VERIFY"' EXIT
ditto -x -k "$CANDIDATE" "$VERIFY"
codesign --verify --deep --strict --verbose=2 "$VERIFY/Payload/Husk.app"
mv -f "$CANDIDATE" "$OUT"
echo "Validated iPadOS test IPA: $OUT"
echo "Physical iPad launch, editor, touch, and saved-chart checks are still required."
