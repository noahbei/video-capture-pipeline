#!/usr/bin/env bash
# gen_key.sh — generate a 32-byte (256-bit) AES key file for vcpcapture
#
# Usage: ./scripts/gen_key.sh [output_path]
# Default output: ./enc.key

set -euo pipefail

OUTPUT="${1:-enc.key}"

if [[ -e "$OUTPUT" ]]; then
    echo "Key file already exists: $OUTPUT"
    echo "Delete it first or specify a different path."
    exit 1
fi

openssl rand -out "$OUTPUT" 32
chmod 600 "$OUTPUT"

echo "Generated 32-byte AES-256 key: $OUTPUT"
echo "Keep this file secure — anyone with the key can decrypt your recordings."
echo ""
echo "To use in config.toml:"
echo "  [encryption]"
echo "  key_file = \"$(realpath "$OUTPUT")\""
