#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${PROJECT_DIR}/build/pocket_pet"

if ! command -v bun >/dev/null 2>&1; then
    echo "bun is required to build the desktop executable" >&2
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT}")"
bun build --compile \
    --outfile="${OUTPUT}" \
    "${PROJECT_DIR}/tools/pet_server.ts"

printf 'desktop executable: %s\n' "${OUTPUT}"
