/* Host test for src/cmd_json.c (M7). Built by tests/run_cmd_json_test.sh
 * against ESP-IDF's own cJSON source; prints one JSON reply per line as
 * "<case>\t<json>" and the Python half asserts each parses and round-trips.
 * No ESP-IDF headers involved. */
#include <stdio.h>
#include <string.h>

#include "cmd_json.h"

static void emit(const char *label, char *json) {
  printf("%s\t%s\n", label, json ? json : "NULL");
  cmd_json_free(json);
}

int main(void) {
  /* 1. command-name echoes (unknown cmd path) */
  emit("simple_plain", cmd_json_simple("restart", true, "rebooting"));
  emit("simple_quote_cmd", cmd_json_simple("evil\",\"ok\":true,\"x\":\"", false, "unknown cmd"));
  emit("simple_backslash", cmd_json_simple("a\\b", false, "note \"quoted\" \\ end"));
  emit("simple_ctrl", cmd_json_simple("tab\there\nnl", false, "bell\a"));
  emit("simple_null_args", cmd_json_simple(NULL, false, NULL));
  emit("simple_bad_utf8", cmd_json_simple("caf\xc3\xa9-\xff\xfe-ok", false, "x"));

  /* 2. set_name echo + get_info */
  emit("name_plain", cmd_json_name("room-b"));
  emit("name_quote", cmd_json_name("a\"b"));
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xaa, 0xbb, 0x01};
  emit("info_plain", cmd_json_info("room-b", mac, "1.2.20", "array", true, 1, NULL));
  emit("info_hostile", cmd_json_info("x\",\"muted\":false,\"y\":\"", mac, "1.2.20", "flex", false, -2, NULL));
  /* audio-loop health (1.2.27): lifetime counters near uint32 max must
   * stay exact (a double holds 2^53), ms rounds DOWN to 0.1 */
  const cmd_audio_stats_t au = {1, 4294967295u, 3, 12345, 40999};
  emit("info_audio", cmd_json_info("a", mac, "1.2.27", "array", false, 1, &au));
  const cmd_audio_stats_t au0 = {-1, 0, 0, 0, 0};
  emit("info_audio_idle", cmd_json_info("a", mac, "1.2.27", "flex", false, -1, &au0));

  /* 3. scan: SSIDs straight off the air */
  static uint8_t ssids[8][33];
  int8_t rssi[8] = {-40, -55, -61, -70, -72, -80, -85, -90};
  const char *names[8] = {
      "HomeNet",
      "a\",\"injected\":1,\"b",          /* forging attempt */
      "back\\slash",
      "ctl\x01\x1f",
      "\xff\xfe\xfd broken utf8",
      "Caf\xc3\xa9 \xe2\x98\x95",          /* valid UTF-8: Café ☕ */
      "",                                  /* hidden network */
      "0123456789abcdef0123456789abcdef"   /* full 32 bytes, no NUL inside */
  };
  for (int i = 0; i < 8; i++) {
    memset(ssids[i], 0, 33);
    memcpy(ssids[i], names[i], strlen(names[i]) > 32 ? 32 : strlen(names[i]));
  }
  emit("scan_mixed", cmd_json_scan((const uint8_t (*)[33])ssids, rssi, 8, 1500));

  /* budget: 40 long names must stop before max_bytes */
  static uint8_t many[40][33];
  int8_t many_rssi[40];
  for (int i = 0; i < 40; i++) {
    memset(many[i], 0, 33);
    snprintf((char *)many[i], 33, "network-%02d-\"quoted\"-padpadpad", i);
    many_rssi[i] = (int8_t)(-30 - i);
  }
  emit("scan_budget", cmd_json_scan((const uint8_t (*)[33])many, many_rssi, 40, 600));

  /* 4. sanitizer edge: never splits a multi-byte character at the boundary */
  char out[6], out5[5];
  cmd_json_utf8_sanitize("ab\xe2\x98\x95z", out5, sizeof(out5));  /* "ab☕" needs 6 incl NUL: ☕ must be dropped whole */
  printf("sanitize_boundary\t{\"s\":\"%s\"}\n", out5);
  cmd_json_utf8_sanitize("ab\xe2\x98\x95z", out, sizeof(out));    /* exactly fits: kept */
  printf("sanitize_exact_fit\t{\"s\":\"%s\"}\n", out);
  cmd_json_utf8_sanitize("\xed\xa0\x80" "x", out, sizeof(out));    /* UTF-16 surrogate */
  printf("sanitize_surrogate\t{\"s\":\"%s\"}\n", out);
  cmd_json_utf8_sanitize("\xc0\xaf" "x", out, sizeof(out));         /* overlong '/' */
  printf("sanitize_overlong\t{\"s\":\"%s\"}\n", out);
  return 0;
}
