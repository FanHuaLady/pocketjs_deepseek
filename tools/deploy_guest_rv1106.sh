#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
: "${RV1106_BOARD:?set RV1106_BOARD=root@your-board-ip}"
REMOTE_DIR="${RV1106_REMOTE_DIR:-/root/pocketjs-gif}"

HOST="${PROJECT_DIR}/build/rv1106-release/pocket_host"
GUEST_JS="${PROJECT_DIR}/build/guest/gif_demo.js"
GUEST_PAK="${PROJECT_DIR}/build/guest/gif_demo.pak"

for file in "${HOST}" "${GUEST_JS}" "${GUEST_PAK}"; do
    if [ ! -f "${file}" ]; then
        echo "missing ${file}" >&2
        exit 1
    fi
done

ssh "${RV1106_BOARD}" "mkdir -p '${REMOTE_DIR}'"
scp "${HOST}" "${GUEST_JS}" "${GUEST_PAK}" "${RV1106_BOARD}:${REMOTE_DIR}/"
ssh "${RV1106_BOARD}" "cd '${REMOTE_DIR}' && chmod +x pocket_host && ./pocket_host gif_demo.js gif_demo.pak 0"
