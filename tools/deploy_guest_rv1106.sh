#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
: "${RV1106_BOARD:?set RV1106_BOARD=root@your-board-ip}"
REMOTE_DIR="${RV1106_REMOTE_DIR:-/root/pocketjs-gif}"

PACKAGE_DIR="${PROJECT_DIR}/build/rv1106-package"

"${SCRIPT_DIR}/package_rv1106.sh"

ssh "${RV1106_BOARD}" "mkdir -p '${REMOTE_DIR}'"
scp "${PACKAGE_DIR}/pocket_host" \
    "${RV1106_BOARD}:${REMOTE_DIR}/.pocket_host.new"
scp "${PACKAGE_DIR}/gif_demo.js" \
    "${PACKAGE_DIR}/gif_demo.pak" \
    "${RV1106_BOARD}:${REMOTE_DIR}/"

REMOTE_VOLUME="${RV1106_AUDIO_VOLUME:-1}"
ssh "${RV1106_BOARD}" \
    "cd '${REMOTE_DIR}' && \
     start-stop-daemon -K -q -p /tmp/pocket_host.pid 2>/dev/null || true; \
     mv .pocket_host.new pocket_host; \
     chmod +x pocket_host; \
     RV1106_AUDIO_VOLUME='${REMOTE_VOLUME}' \
       start-stop-daemon -S -b -m -p /tmp/pocket_host.pid \
       -x '${REMOTE_DIR}/pocket_host' --"
