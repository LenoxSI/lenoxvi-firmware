#!/usr/bin/env bash
# Host test for the Wi-Fi change trial boot decision (src/wifi_trial.c).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isrc \
   src/wifi_trial.c tests/test_wifi_trial.c -o "$OUT/t"
"$OUT/t"
