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
check("info: no audio stats -> no audio key", "audio" not in J("info_plain")["data"], J("info_plain"))
d = J("info_audio")["data"]
check("info audio: all fields, counters exact, ms floored to 0.1",
      d.get("audio") == {"core": 1, "frames": 4294967295, "ovf": 3,
                         "max_work_ms": 12.3, "max_gap_ms": 40.9}, d.get("audio"))
check("info audio: existing fields untouched", d["fw"] == "1.2.27" and d["mute_hw"] == {"btn": 1}, d)
d = J("info_audio_idle")["data"]
check("info audio: not-started loop reports core -1 and zeros",
      d.get("audio") == {"core": -1, "frames": 0, "ovf": 0, "max_work_ms": 0, "max_gap_ms": 0}, d.get("audio"))
d = J("info_plain")["data"]
check("info boot: block absent when no boot info (older callers)", "boot" not in d and "wifi" not in d, d)
d = J("info_boot_panic")["data"]
check("info boot: crash reports reason, empty why, both uptimes",
      d.get("boot") == {"reason": "panic", "why": "", "prev_up_s": 139, "up_s": 42}, d.get("boot"))
check("info boot: rssi in its own wifi block", d.get("wifi") == {"rssi": -61}, d.get("wifi"))
check("info boot: audio + existing fields untouched beside it",
      d.get("audio", {}).get("frames") == 4294967295 and d["muted"] is True and d["fw"] == "1.2.29", d)
d = J("info_boot_sw")["data"]
check("info boot: deliberate reboot names its path; uint32 uptime exact",
      d.get("boot") == {"reason": "sw", "why": "peer-dropped", "prev_up_s": 4294967295, "up_s": 7}
      and d.get("wifi") == {"rssi": -88} and "audio" not in d, d)
d = J("info_boot_cold")["data"]
check("info boot: not associated -> rssi null (not 0 dBm)", d.get("wifi") == {"rssi": None}, d.get("wifi"))
check("info boot: cold power-on has no previous record", d["boot"]["reason"] == "power_on" and d["boot"]["prev_up_s"] == 0, d)
d = J("info_boot_hostile")["data"]
check("info boot: hostile reason/why are DATA (no injected keys), cut to buffer",
      set(d["boot"]) == {"reason", "why", "prev_up_s", "up_s"} and "x" not in d
      and d["boot"]["reason"] == 'sw","x":1' and d["boot"]["why"].startswith('a"}\\b\x01')
      and len(d["boot"]["why"]) == 23, d.get("boot"))
d = J("info_boot_nullstr")["data"]
check("info boot: NULL strings become empty, never a crash",
      d["boot"]["reason"] == "" and d["boot"]["why"] == "", d.get("boot"))
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
