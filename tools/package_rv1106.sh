#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${PROJECT_DIR}/build/rv1106-package"
HOST="${PROJECT_DIR}/build/rv1106-release/pocket_host"
GUEST_JS="${PROJECT_DIR}/build/guest/gif_demo.js"
GUEST_PAK="${PROJECT_DIR}/build/guest/gif_demo.pak"

for file in "${HOST}" "${GUEST_JS}" "${GUEST_PAK}"; do
    if [ ! -f "${file}" ]; then
        echo "missing ${file}" >&2
        exit 1
    fi
done

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
cp "${HOST}" "${OUTPUT_DIR}/pocket_host"
cp "${GUEST_JS}" "${OUTPUT_DIR}/gif_demo.js"
cp "${GUEST_PAK}" "${OUTPUT_DIR}/gif_demo.pak"
chmod +x "${OUTPUT_DIR}/pocket_host"

printf 'RV1106 package: %s\n' "${OUTPUT_DIR}"
printf 'start command:  cd %s && ./pocket_host\n' "${OUTPUT_DIR}"
