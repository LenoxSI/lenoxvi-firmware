#!/usr/bin/env bash
# Host test for the LED render decision (src/rgb.c): connect flash + WIFI_UP.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined -g \
   -Itests/stubs -Isrc tests/test_rgb.c -o "$OUT/t"
"$OUT/t"
