/* Reply JSON builders — see cmd_json.h (M7, 1.2.20). */
#include "cmd_json.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void cmd_json_utf8_sanitize(const char *src, char *dst, size_t dst_len) {
  if (dst_len == 0) return;
  const unsigned char *s = (const unsigned char *)(src ? src : "");
  size_t o = 0;
  while (*s && o + 1 < dst_len) {
    unsigned char c = s[0];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
            : (c & 0xF8) == 0xF0 ? 4 : 0;
    bool ok = len > 0;
    for (int i = 1; ok && i < len; i++)
      if ((s[i] & 0xC0) != 0x80) ok = false;   /* also stops at NUL */
    if (ok && len == 2 && c < 0xC2) ok = false;                     /* overlong */
    if (ok && len == 3 && c == 0xE0 && s[1] < 0xA0) ok = false;     /* overlong */
    if (ok && len == 3 && c == 0xED && s[1] >= 0xA0) ok = false;    /* surrogate */
    if (ok && len == 4 && (c > 0xF4 || (c == 0xF0 && s[1] < 0x90)
                           || (c == 0xF4 && s[1] >= 0x90))) ok = false;
    if (!ok) {
      dst[o++] = '?';
      s++;
      continue;
    }
    if (o + (size_t)len + 1 > dst_len) break;   /* never split a character */
    memcpy(dst + o, s, (size_t)len);
    o += (size_t)len;
    s += len;
  }
  dst[o] = '\0';
}

/* The cmd_result envelope; data is attached by the caller. */
static cJSON *envelope(const char *cmd, bool ok) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  char safe_cmd[64];
  cmd_json_utf8_sanitize(cmd, safe_cmd, sizeof(safe_cmd));
  /* "type" is REQUIRED: pipecat's datachannel parser drops messages
   * without it (measured 2026-09-29). */
  if (!cJSON_AddStringToObject(root, "type", "cmd_result")
      || !cJSON_AddStringToObject(root, "evt", "cmd_result")
      || !cJSON_AddStringToObject(root, "cmd", safe_cmd)
      || !cJSON_AddBoolToObject(root, "ok", ok)) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

static char *finish(cJSON *root) {
  if (!root) return NULL;
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

char *cmd_json_simple(const char *cmd, bool ok, const char *note) {
  cJSON *root = envelope(cmd, ok);
  if (!root) return NULL;
  char safe_note[128];
  cmd_json_utf8_sanitize(note, safe_note, sizeof(safe_note));
  if (!cJSON_AddStringToObject(root, "data", safe_note)) {
    cJSON_Delete(root);
    return NULL;
  }
  return finish(root);
}

char *cmd_json_name(const char *name) {
  cJSON *root = envelope("set_name", true);
  cJSON *data = root ? cJSON_AddObjectToObject(root, "data") : NULL;
  char safe[64];
  cmd_json_utf8_sanitize(name, safe, sizeof(safe));
  if (!data || !cJSON_AddStringToObject(data, "name", safe)) {
    cJSON_Delete(root);
    return NULL;
  }
  return finish(root);
}

char *cmd_json_info(const char *name, const uint8_t mac[6], const char *fw,
                    const char *line, bool muted, int btn,
                    const cmd_audio_stats_t *audio,
                    const cmd_boot_info_t *boot) {
  cJSON *root = envelope("get_info", true);
  cJSON *data = root ? cJSON_AddObjectToObject(root, "data") : NULL;
  if (!data) { cJSON_Delete(root); return NULL; }
  char safe_name[64], safe_fw[40], safe_line[16], mac_hex[13];
  cmd_json_utf8_sanitize(name, safe_name, sizeof(safe_name));
  cmd_json_utf8_sanitize(fw, safe_fw, sizeof(safe_fw));
  cmd_json_utf8_sanitize(line, safe_line, sizeof(safe_line));
  snprintf(mac_hex, sizeof(mac_hex), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cJSON *hw = NULL;
  if (!cJSON_AddStringToObject(data, "name", safe_name)
      || !cJSON_AddStringToObject(data, "mac", mac_hex)
      || !cJSON_AddStringToObject(data, "fw", safe_fw)
      || !cJSON_AddStringToObject(data, "line", safe_line)
      || !cJSON_AddBoolToObject(data, "muted", muted)
      || !(hw = cJSON_AddObjectToObject(data, "mute_hw"))
      || !cJSON_AddNumberToObject(hw, "btn", btn)) {
    cJSON_Delete(root);
    return NULL;
  }
  if (audio) {
    /* ms with 0.1 resolution: frames are 20 ms, so sub-ms detail is noise */
    cJSON *au = cJSON_AddObjectToObject(data, "audio");
    if (!au
        || !cJSON_AddNumberToObject(au, "core", audio->core)
        || !cJSON_AddNumberToObject(au, "frames", (double)audio->frames)
        || !cJSON_AddNumberToObject(au, "ovf", (double)audio->ovf)
        || !cJSON_AddNumberToObject(au, "max_work_ms", (double)(audio->max_work_us / 100) / 10.0)
        || !cJSON_AddNumberToObject(au, "max_gap_ms", (double)(audio->max_gap_us / 100) / 10.0)
        || !cJSON_AddNumberToObject(au, "zero_s", (double)audio->zero_s)) {
      cJSON_Delete(root);
      return NULL;
    }
  }
  if (boot) {
    char safe_reason[16], safe_why[24];
    cmd_json_utf8_sanitize(boot->reason ? boot->reason : "", safe_reason, sizeof(safe_reason));
    cmd_json_utf8_sanitize(boot->why ? boot->why : "", safe_why, sizeof(safe_why));
    cJSON *bo = cJSON_AddObjectToObject(data, "boot");
    cJSON *wf = bo ? cJSON_AddObjectToObject(data, "wifi") : NULL;
    if (!wf
        || !cJSON_AddStringToObject(bo, "reason", safe_reason)
        || !cJSON_AddStringToObject(bo, "why", safe_why)
        || !cJSON_AddNumberToObject(bo, "prev_up_s", (double)boot->prev_up_s)
        || !cJSON_AddNumberToObject(bo, "up_s", (double)boot->up_s)
        || !(boot->rssi ? cJSON_AddNumberToObject(wf, "rssi", boot->rssi)
                        : cJSON_AddNullToObject(wf, "rssi"))) {
      cJSON_Delete(root);
      return NULL;
    }
    if (boot->ka) {
      cJSON *ka = cJSON_AddObjectToObject(wf, "ka");
      if (!ka
          || !cJSON_AddNumberToObject(ka, "sent", (double)boot->ka->sent)
          || !cJSON_AddNumberToObject(ka, "resent", (double)boot->ka->resent)
          || !cJSON_AddNumberToObject(ka, "rescued", (double)boot->ka->rescued)) {
        cJSON_Delete(root);
        return NULL;
      }
    }
  }
  return finish(root);
}

char *cmd_json_scan(const uint8_t (*ssids)[33], const int8_t *rssi, int n,
                    size_t max_bytes) {
  cJSON *root = envelope("scan", true);
  cJSON *data = root ? cJSON_AddObjectToObject(root, "data") : NULL;
  if (!data) { cJSON_Delete(root); return NULL; }
  /* Envelope ~70 B; each entry costs its ESCAPED name (cJSON: '"' and '\\'
   * -> 2 B, control chars -> 6 B, everything else as-is) + quotes, colon,
   * comma and a 4-digit RSSI. */
  size_t budget = 80;
  for (int i = 0; i < n; i++) {
    char raw[33], safe[33];
    memcpy(raw, ssids[i], 32);
    raw[32] = '\0';
    cmd_json_utf8_sanitize(raw, safe, sizeof(safe));
    size_t cost = 8;
    for (const unsigned char *p = (const unsigned char *)safe; *p; p++)
      cost += (*p == '"' || *p == '\\') ? 2 : (*p < 0x20) ? 6 : 1;
    if (budget + cost > max_bytes) break;
    if (!cJSON_AddNumberToObject(data, safe, rssi[i])) {
      cJSON_Delete(root);
      return NULL;
    }
    budget += cost;
  }
  return finish(root);
}

void cmd_json_free(char *s) {
  if (s) cJSON_free(s);
}
