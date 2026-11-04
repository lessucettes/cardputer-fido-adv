#!/usr/bin/env bash
# Release gate for the Cardputer ADV prototype. Hardware validation is separate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

cmake -S tests/cardputer -B build/cardputer-host -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCARDPUTER_SANITIZERS=ON
cmake --build build/cardputer-host
ctest --test-dir build/cardputer-host --output-on-failure -R '^cardputer_'

python3 -m unittest discover -s tests/cardputer -p '*_test.py'

: "${IDF_PATH:?IDF_PATH must point at the pinned ESP-IDF installation}"
bash scripts/cardputer/build.sh
python3 scripts/cardputer/check-build.py build/cardputer-adv
python3 scripts/cardputer/package.py --build build/cardputer-adv \
    --out dist/cardputer-adv --force

echo "PASS: release checks (hardware gates remain separate)"
