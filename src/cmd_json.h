/* Reply JSON builders for the node's command channel (M7, 1.2.20).
 *
 * Every reply that carries a string from OUTSIDE the firmware — a command
 * name echoed back, Wi-Fi network names heard over the air, the node name —
 * is built here with cJSON, so quotes, backslashes and control characters
 * are escaped instead of pasted into the JSON (a network named
 * `a","x":1` could forge or break the old snprintf replies). Network names
 * are raw 802.11 bytes, not guaranteed UTF-8: invalid sequences are replaced
 * with '?' so the bot's JSON parser never rejects a whole scan.
 *
 * Pure C + cJSON, no ESP-IDF headers: tests/test_cmd_json.c builds this on
 * the host against the same cJSON source the firmware compiles.
 *
 * Every builder returns a heap string the caller releases with
 * cmd_json_free(), or NULL on allocation failure. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* {"type":"cmd_result","evt":"cmd_result","cmd":<cmd>,"ok":<ok>,"data":<note>} */
char *cmd_json_simple(const char *cmd, bool ok, const char *note);

/* {...,"cmd":"set_name","ok":true,"data":{"name":<name>}} */
char *cmd_json_name(const char *name);

/* {...,"cmd":"get_info","ok":true,"data":{"name","mac","fw","line","muted",
 *  "mute_hw":{"btn"}}} — mac is the 6 raw bytes, printed as 12 hex chars. */
char *cmd_json_info(const char *name, const uint8_t mac[6], const char *fw,
                    const char *line, bool muted, int btn);

/* {...,"cmd":"scan","ok":true,"data":{<ssid>:<rssi>,...}} — ssids is n
 * entries of 33 bytes (NUL-terminated, like wifi_ap_record_t.ssid). Callers
 * dedupe; entries past max_bytes of output are dropped (the data channel
 * message stays bounded). */
char *cmd_json_scan(const uint8_t (*ssids)[33], const int8_t *rssi, int n,
                    size_t max_bytes);

/* Copy src into dst (dst_len incl. NUL) replacing invalid UTF-8 with '?'. */
void cmd_json_utf8_sanitize(const char *src, char *dst, size_t dst_len);

void cmd_json_free(char *s);

#ifdef __cplusplus
}
#endif
