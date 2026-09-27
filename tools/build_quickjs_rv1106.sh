#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SDK_ROOT="${ECHO_SDK_ROOT:-/opt/rv1106-sdk}"
TOOLCHAIN_ROOT="${SDK_ROOT}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
TOOL_PREFIX="${TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf"
QUICKJS_DIR="${PROJECT_DIR}/third_party/quickjs"

cd "${QUICKJS_DIR}"
export CFLAGS="-I${PROJECT_DIR}/compat"

make clean
make -j2 \
    CROSS_PREFIX="${TOOL_PREFIX}-" \
    qjs libquickjs.a
