#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
POCKETJS_DIR="${PROJECT_DIR}/third_party/pocketjs"

cd "${POCKETJS_DIR}"

env -u MAKEFLAGS -u MFLAGS cargo +nightly build --release --locked \
    --manifest-path engine/ui-cabi/Cargo.toml \
    --target armv7-unknown-linux-uclibceabihf \
    -Z build-std=std,panic_abort \
    --features software-only

printf 'core: %s\n' \
    "${POCKETJS_DIR}/engine/ui-cabi/target/armv7-unknown-linux-uclibceabihf/release/libpocketjs_symbian_core.a"
