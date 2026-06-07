#!/usr/bin/env bash
# Copies build artifacts into firmware/ ready for hosting or GitHub Release.
# Usage: ./scripts/package-firmware.sh [version]
set -euo pipefail

VERSION=${1:-$(git describe --tags --always 2>/dev/null || echo "dev")}
BUILD_DIR="$(dirname "$0")/../build"
OUT_DIR="$(dirname "$0")/../firmware"

if [ ! -f "$BUILD_DIR/gscrl-obs-remote.bin" ]; then
  echo "ERROR: run 'idf.py build' first"
  exit 1
fi

mkdir -p "$OUT_DIR"

cp "$BUILD_DIR/gscrl-obs-remote.bin"                    "$OUT_DIR/obs-remote.bin"
cp "$BUILD_DIR/bootloader/bootloader.bin"               "$OUT_DIR/bootloader.bin"
cp "$BUILD_DIR/partition_table/partition-table.bin"     "$OUT_DIR/partition-table.bin"

cat > "$OUT_DIR/manifest.json" <<EOF
{
  "version": "$VERSION",
  "built":   "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "files": {
    "bootloader":       { "offset": "0x0",     "file": "bootloader.bin" },
    "partition_table":  { "offset": "0x8000",  "file": "partition-table.bin" },
    "app":              { "offset": "0x10000", "file": "obs-remote.bin" }
  }
}
EOF

echo "Packaged firmware $VERSION -> $OUT_DIR/"
ls -lh "$OUT_DIR/"
