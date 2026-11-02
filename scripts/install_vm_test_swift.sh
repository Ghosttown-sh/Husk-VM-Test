#!/bin/bash
# Install the signed upstream compiler required by the vendored StikJIT module.
# Run on an ephemeral Mac CI host; no Apple account or signing credential used.
set -euo pipefail
VERSION=6.4.2
PACKAGE="${RUNNER_TEMP:?CI temporary directory required}/swift-$VERSION.pkg"
curl --fail --location --retry 3 \
    "https://download.swift.org/swift-$VERSION-release/xcode/swift-$VERSION-RELEASE/swift-$VERSION-RELEASE-osx.pkg" \
    -o "$PACKAGE"
SIGNATURE=$(pkgutil --check-signature "$PACKAGE")
printf '%s\n' "$SIGNATURE"
printf '%s\n' "$SIGNATURE" | grep -F 'Developer ID Installer: Swift Open Source (V9AUD2URP3)' >/dev/null
installer -target CurrentUserHomeDirectory -pkg "$PACKAGE"
TOOLCHAIN="$HOME/Library/Developer/Toolchains/swift-$VERSION-RELEASE.xctoolchain"
export TOOLCHAINS=$(plutil -extract CFBundleIdentifier raw "$TOOLCHAIN/Info.plist")
xcrun swiftc --version
printf 'TOOLCHAINS=%s\n' "$TOOLCHAINS" >> "${GITHUB_ENV:?}"
