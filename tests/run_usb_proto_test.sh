#!/usr/bin/env bash
# Host test for the USB setup protocol (src/usb_proto.c): line assembler,
# field rules and reply framing. Builds it with the system C compiler against
# ESP-IDF's own cJSON (and wifi_trial.c for the password rule).
#   IDF_PATH=~/esp-idf bash tests/run_usb_proto_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
CJSON="$IDF_PATH/components/json/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "FATAL: cJSON not found under $CJSON (set IDF_PATH)"; exit 2; }
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -Isrc -I"$CJSON" src/usb_proto.c src/wifi_trial.c "$CJSON/cJSON.c" tests/test_usb_proto.c -o "$OUT/t"
"$OUT/t"
