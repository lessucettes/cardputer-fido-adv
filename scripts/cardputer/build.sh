#!/usr/bin/env bash
# Reproducible Cardputer ADV build entrypoint. Never flashes or erases.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${IDF_PATH:?IDF_PATH must point at the pinned ESP-IDF installation}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build/cardputer-adv}"

mkdir -p "${BUILD_DIR}"

exec python3 "${IDF_PATH}/tools/idf.py" \
    -B "${BUILD_DIR}" \
    -DIDF_TARGET=esp32s3 \
    -DCARDPUTER_ADV=ON \
    -DSDKCONFIG="${BUILD_DIR}/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="${ROOT}/config/cardputer_adv/sdkconfig.defaults" \
    build
