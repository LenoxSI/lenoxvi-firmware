#!/usr/bin/env bash
# Host test for the NVS string copy (src/nvs_str.h): values that exactly fill
# their buffer must load. ASan + UBSan catch any overrun.
#   bash tests/run_nvs_str_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -Isrc tests/test_nvs_str.c -o "$OUT/t"
"$OUT/t"
