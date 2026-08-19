#!/usr/bin/env bash
# Package the Windows Host source into the ZIP served by the ReMCote website.
# Re-run after any change under windows-host/ so the download stays current.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT_DIR="artifacts/remcote/public/downloads"
OUT="$OUT_DIR/remcote-windows-host.zip"
STAGE="$(mktemp -d)/remcote-windows-host"

mkdir -p "$OUT_DIR" "$STAGE"

# Windows host source, scripts, and docs (exclude build outputs / vendored deps)
mkdir -p "$STAGE/windows-host"
for entry in windows-host/*; do
  base="$(basename "$entry")"
  case "$base" in build|dist|third_party) continue ;; esac
  cp -r "$entry" "$STAGE/windows-host/"
done

# Wire-protocol reference (the TS source of truth the C++ mirrors)
mkdir -p "$STAGE/reference"
cp lib/remcote-protocol/src/index.ts "$STAGE/reference/remcote-protocol.ts"

cp windows-host/README.md "$STAGE/README.md"

rm -f "$OUT"
(cd "$(dirname "$STAGE")" && zip -qr "$OLDPWD/$OUT" "$(basename "$STAGE")")
echo "Wrote $OUT ($(du -h "$OUT" | cut -f1))"
