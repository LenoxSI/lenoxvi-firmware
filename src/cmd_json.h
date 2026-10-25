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

/* Audio-loop health (1.2.27): lifetime counters + worst cases since the
 * previous get_info. ovf is the ground truth — each count is an I2S DMA
 * buffer the mic produced and the loop never read (audio LOST). */
typedef struct cmd_audio_stats_t {
  int core;              /* core the audio loop last ran on (-1 = not started) */
  uint32_t frames;       /* 20 ms frames encoded + handed to the RTP sender */
  uint32_t ovf;          /* I2S RX DMA queue overflows since boot */
  uint32_t max_work_us;  /* worst decimate + encode + send, since last read */
  uint32_t max_gap_us;   /* worst gap between frame reads, since last read */
  uint32_t zero_s;       /* seconds of all-zero mic audio while not muted, since boot */
  const char *init;      /* audio init result: "ok" or the step that failed; NULL = omitted */
} cmd_audio_stats_t;

/* STUN keepalive counters since boot (1.2.36, libpeer patch 0004):
 * sent = fresh keepalive requests, resent = retransmits, rescued = replies
 * that came only after a retransmit (sessions one lost packet used to close). */
typedef struct cmd_ka_stats_t {
  uint32_t sent;
  uint32_t resent;
  uint32_t rescued;
} cmd_ka_stats_t;

/* Wi-Fi link history since boot: the last STA disconnect reason (ESP-IDF
 * wifi_err_reason_t, 0 = none yet) and how many disconnects happened. */
typedef struct cmd_link_stats_t {
  int last_reason;
  uint32_t disconnects;
} cmd_link_stats_t;
/* Last OTA outcome this boot. last is a short fixed tag ("none" = no check
 * yet; see ota.cpp for the set), http the last HTTP status seen by the check
 * or download (0 = none), at_s the uptime when it was recorded. */
typedef struct cmd_ota_status_t {
  const char *last;
  int http;
  uint32_t at_s;
} cmd_ota_status_t;
/* Reboot black box (1.2.29): why the previous boot ended, plus link
 * quality — see boot_diag.h for the meaning of each field. */
typedef struct cmd_boot_info_t {
  const char *reason;   /* chip reset reason name ("sw", "panic", ...) */
  const char *why;      /* our own reboot path, "" when none */
  uint32_t prev_up_s;   /* uptime of the previous boot, seconds */
  uint32_t up_s;        /* uptime of this boot, seconds */
  int rssi;             /* Wi-Fi signal, dBm; 0 = not associated (-> null) */
  const cmd_ka_stats_t *ka;  /* keepalive counters -> "wifi":{"ka"}; NULL = omitted */
  const cmd_link_stats_t *link;  /* -> "wifi":{"last_reason","disconnects"}; NULL = omitted */
  const cmd_ota_status_t *ota;   /* -> "ota":{"last","http","at_s"}; NULL = omitted */
} cmd_boot_info_t;

/* {...,"cmd":"get_info","ok":true,"data":{"name","mac","fw","line","muted",
 *  "mute_hw":{"btn"},"audio":{"core","frames","ovf","max_work_ms",
 *  "max_gap_ms"},"boot":{"reason","why","prev_up_s","up_s"},
 *  "wifi":{"rssi","ka":{"sent","resent","rescued"},"last_reason","disconnects"},
 *  "ota":{"last","http","at_s"}}} — audio.init rides in "audio"; mac is the 6 raw
 *  bytes, printed as 12 hex chars; "ka" is omitted when boot->ka is NULL;
 *  "audio" is omitted when audio is NULL, "boot" + "wifi" when boot is. */
char *cmd_json_info(const char *name, const uint8_t mac[6], const char *fw,
                    const char *line, bool muted, int btn,
                    const cmd_audio_stats_t *audio,
                    const cmd_boot_info_t *boot);

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
