#!/usr/bin/env bash
# Host test for the command-reply JSON builders (src/cmd_json.c).
# Builds them with the system C compiler against ESP-IDF's own cJSON, runs
# every case, and checks each reply parses as JSON with the hostile strings
# carried as DATA (escaped), never as structure.
#   IDF_PATH=~/esp-idf bash tests/run_cmd_json_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
CJSON="$IDF_PATH/components/json/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "FATAL: cJSON not found under $CJSON (set IDF_PATH)"; exit 2; }
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -Isrc -I"$CJSON" src/cmd_json.c "$CJSON/cJSON.c" tests/test_cmd_json.c -o "$OUT/t"
"$OUT/t" > "$OUT/out.tsv"
python3 - "$OUT/out.tsv" <<'PY'
import json, sys
rows = dict(line.rstrip("\n").split("\t", 1) for line in open(sys.argv[1], encoding="utf-8"))
fails = 0
def check(name, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  -- {detail}"))
    fails += 0 if cond else 1
def J(k):
    return json.loads(rows[k])
ENV = {"type": "cmd_result", "evt": "cmd_result"}
for k in rows:
    try:
        json.loads(rows[k]); ok = True
    except Exception as e:
        ok = False; err = e
    check(f"{k}: parses as JSON", ok, rows[k] if ok else f"{err}: {rows[k]}")
d = J("simple_plain")
check("simple: envelope + fields", d == {**ENV, "cmd": "restart", "ok": True, "data": "rebooting"}, d)
d = J("simple_quote_cmd")
check("simple: quoted cmd stays ONE string, ok stays false, no injected key",
      d["cmd"] == 'evil","ok":true,"x":"' and d["ok"] is False and "x" not in d, d)
d = J("simple_backslash")
check("simple: backslashes + quotes round-trip", d["cmd"] == "a\\b" and d["data"] == 'note "quoted" \\ end', d)
d = J("simple_ctrl")
check("simple: control chars escaped and round-trip", d["cmd"] == "tab\there\nnl" and d["data"] == "bell\a", d)
d = J("simple_null_args")
check("simple: NULL cmd/note -> empty strings", d["cmd"] == "" and d["data"] == "", d)
d = J("simple_bad_utf8")
check("simple: invalid UTF-8 -> '?', valid kept", d["cmd"] == "café-??-ok", d)
check("name: plain", J("name_plain") == {**ENV, "cmd": "set_name", "ok": True, "data": {"name": "room-b"}})
check("name: quote escaped", J("name_quote")["data"]["name"] == 'a"b')
d = J("info_plain")
check("info: all fields", d["cmd"] == "get_info" and d["data"] == {
    "name": "room-b", "mac": "020000aabb01", "fw": "1.2.20", "line": "array",
    "muted": True, "mute_hw": {"btn": 1}}, d)
d = J("info_hostile")
check("info: hostile name can't override muted", d["data"]["muted"] is False
      and d["data"]["name"] == 'x","muted":false,"y":"' and "y" not in d["data"]
      and d["data"]["mute_hw"] == {"btn": -2} and d["data"]["line"] == "flex", d)
d = J("scan_mixed")["data"]
check("scan: forging SSID is one key, no injected key",
      'a","injected":1,"b' in d and "injected" not in d and d['a","injected":1,"b'] == -55, d)
check("scan: backslash + control chars round-trip", d.get("back\\slash") == -61 and d.get("ctl\x01\x1f") == -70, d)
check("scan: broken UTF-8 replaced with '?'", d.get("??? broken utf8") == -72, d)
check("scan: valid UTF-8 kept (Café ☕)", d.get("Café ☕") == -80, d)
check("scan: hidden network (empty name) kept", d.get("") == -85, d)
check("scan: full 32-byte SSID intact", d.get("0123456789abcdef0123456789abcdef") == -90, d)
check("scan: all 8 networks present", len(d) == 8, list(d))
b = rows["scan_budget"]
bd = json.loads(b)["data"]
check("scan budget: output stays within max_bytes (600)", len(b.encode()) <= 600, len(b.encode()))
check("scan budget: kept a prefix in order, dropped the rest", 0 < len(bd) < 40
      and list(bd) == [f'network-{i:02d}-"quoted"-padpadpad' for i in range(len(bd))], list(bd)[:3])
check("sanitize: never splits a multi-byte char at the buffer edge", J("sanitize_boundary")["s"] == "ab", rows["sanitize_boundary"])
check("sanitize: a multi-byte char that exactly fits is kept", J("sanitize_exact_fit")["s"] == "ab☕", rows["sanitize_exact_fit"])
check("sanitize: UTF-16 surrogate rejected", J("sanitize_surrogate")["s"] == "???x", rows["sanitize_surrogate"])
check("sanitize: overlong encoding rejected", J("sanitize_overlong")["s"] == "??x", rows["sanitize_overlong"])
print(f"\n{'ALL PASSED' if not fails else str(fails) + ' FAILED'} ({len(rows)} cases)")
sys.exit(1 if fails else 0)
PY
