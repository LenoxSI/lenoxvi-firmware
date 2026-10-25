/* USB setup protocol v1 — see usb_proto.h (1.2.47). */
#include "usb_proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "wifi_trial.h"

#define PREFIX_REQ "OH>"
#define PREFIX_REPLY "OH<"

/* ── line assembler ─────────────────────────────────────────────────── */

void usb_line_init(usb_line_t *st) {
  st->len = 0;
  st->overflow = false;
  st->buf[0] = '\0';
}

void usb_line_feed(usb_line_t *st, const uint8_t *data, size_t n,
                   void (*on_line)(const char *line, void *ctx),
                   void (*on_overflow)(void *ctx), void *ctx) {
  for (size_t i = 0; i < n; i++) {
    char c = (char)data[i];
    if (st->overflow) {                 /* drop the rest of an overlong line */
      if (c == '\n') st->overflow = false;
      continue;
    }
    if (c == '\n') {
      if (st->len > 0 && st->buf[st->len - 1] == '\r') st->len--;
      st->buf[st->len] = '\0';
      st->len = 0;
      if (on_line) on_line(st->buf, ctx);
      continue;
    }
    if (st->len >= USB_LINE_MAX) {
      st->overflow = true;
      st->len = 0;
      if (on_overflow) on_overflow(ctx);
      continue;
    }
    st->buf[st->len++] = c;
  }
}

/* ── rules ──────────────────────────────────────────────────────────── */

bool usb_proto_name_ok(const char *name) {
  if (!name) return false;
  size_t n = strlen(name);
  if (n < 1 || n > 31) return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
          || c == '_' || c == '-')) return false;
  }
  return true;
}

bool usb_proto_bot_url_ok(const char *url) {
  static const char scheme[] = "https://", path[] = "/api/offer";
  if (!url) return false;
  size_t n = strlen(url);
  if (n == 0) return true;                       /* clear: back to the baked URL */
  if (n > USB_BOT_URL_MAX - 1) return false;
  for (size_t i = 0; i < n; i++) {
    char c = url[i];
    if (c <= ' ' || c > '~' || c == '"' || c == '\\') return false;
  }
  size_t ls = sizeof scheme - 1, lp = sizeof path - 1;
  if (n <= ls + lp) return false;                /* no host */
  if (strncmp(url, scheme, ls) != 0 || strcmp(url + n - lp, path) != 0) return false;
  /* What's between is host[:port]: a host, no further path. */
  if (url[ls] == ':') return false;
  for (size_t i = ls; i < n - lp; i++) {
    if (url[i] == '/') return false;
  }
  return true;
}

/* ── requests ───────────────────────────────────────────────────────── */

static void set_err(char *err, size_t errlen, const char *msg) {
  if (errlen) snprintf(err, errlen, "%s", msg);
}

/* A present field must be a string that fits dst. Absent: *present = false. */
static bool take_str(const cJSON *root, const char *key, char *dst, size_t dst_size,
                     bool *present, char *err, size_t errlen) {
  const cJSON *j = cJSON_GetObjectItemCaseSensitive(root, key);
  *present = j != NULL;
  if (!j) return true;
  if (!cJSON_IsString(j) || strlen(j->valuestring) >= dst_size) {
    if (errlen) snprintf(err, errlen, "%s: must be a string of at most %u bytes",
                         key, (unsigned)(dst_size - 1));
    return false;
  }
  memcpy(dst, j->valuestring, strlen(j->valuestring) + 1);
  return true;
}

bool usb_proto_parse(const char *line, usb_req_t *out, char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  out->id = -1;
  if (errlen) err[0] = '\0';
  if (!line || strncmp(line, PREFIX_REQ, sizeof PREFIX_REQ - 1) != 0) return false;

  cJSON *root = cJSON_Parse(line + sizeof PREFIX_REQ - 1);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    set_err(err, errlen, "bad json");
    return true;
  }
  /* id first, so every error reply below can echo it. */
  const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "id");
  if (j_id) {
    if (!cJSON_IsNumber(j_id) || !(j_id->valuedouble >= 0 && j_id->valuedouble <= 2147483647.0)) {
      set_err(err, errlen, "id: must be a number from 0 to 2147483647");
      goto done;
    }
    out->id = (long)j_id->valuedouble;
  }
  const cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
  if (!cJSON_IsString(j_cmd)) {
    set_err(err, errlen, "cmd: must be a string");
    goto done;
  }
  if (strcmp(j_cmd->valuestring, "info") == 0) {
    out->cmd = USB_CMD_INFO;
  } else if (strcmp(j_cmd->valuestring, "restart") == 0) {
    out->cmd = USB_CMD_RESTART;
  } else if (strcmp(j_cmd->valuestring, "set") == 0) {
    out->cmd = USB_CMD_SET;
    bool has_ssid = false, has_pass = false;
    if (!take_str(root, "wifi_ssid", out->wifi_ssid, sizeof out->wifi_ssid, &has_ssid, err, errlen)
        || !take_str(root, "wifi_pass", out->wifi_pass, sizeof out->wifi_pass, &has_pass, err, errlen)
        || !take_str(root, "name", out->name, sizeof out->name, &out->has_name, err, errlen)
        || !take_str(root, "bot_url", out->bot_url, sizeof out->bot_url, &out->has_bot_url, err, errlen)) {
      goto done;
    }
    out->has_wifi = has_ssid || has_pass;
    if (has_ssid != has_pass) {
      set_err(err, errlen, has_ssid ? "wifi_pass: must be sent with wifi_ssid"
                                    : "wifi_ssid: must be sent with wifi_pass");
      goto done;
    }
  } else {
    set_err(err, errlen, "unknown cmd");
  }
done:
  {
    /* Don't leave the password in freed heap. */
    cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "wifi_pass");
    if (cJSON_IsString(j_pass)) memset(j_pass->valuestring, 0, strlen(j_pass->valuestring));
  }
  cJSON_Delete(root);
  return true;
}

int usb_proto_validate_set(const usb_req_t *r, char *err, size_t errlen) {
  if (errlen) err[0] = '\0';
  if (r->has_bot_url && !usb_proto_bot_url_ok(r->bot_url)) {
    set_err(err, errlen, "bot_url: must be empty or https://host[:port]/api/offer, at most 127 bytes");
    return -1;
  }
  if (r->has_name && !usb_proto_name_ok(r->name)) {
    set_err(err, errlen, "name: must be 1 to 31 of A-Z a-z 0-9 _ -");
    return -1;
  }
  if (r->has_wifi) {
    size_t n = strlen(r->wifi_ssid);
    if (n < 1 || n > 32) {
      set_err(err, errlen, "wifi_ssid: must be 1 to 32 bytes");
      return -1;
    }
    if (!wifi_pass_acceptable(r->wifi_pass)) {
      set_err(err, errlen, "wifi_pass: must be up to 63 characters, or 64 hex digits");
      return -1;
    }
  }
  return 0;
}

/* ── replies ────────────────────────────────────────────────────────── */

static const char *s_or_empty(const char *s) { return s ? s : ""; }

static cJSON *reply_head(const char *cmd, long id, bool ok) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddStringToObject(root, "cmd", s_or_empty(cmd));
  if (id >= 0) cJSON_AddNumberToObject(root, "id", (double)id);
  cJSON_AddBoolToObject(root, "ok", ok);
  return root;
}

static void add_saved(cJSON *root, const char *const *saved, int n_saved) {
  if (!saved) return;
  cJSON *arr = cJSON_AddArrayToObject(root, "saved");
  for (int i = 0; arr && i < n_saved; i++) {
    cJSON_AddItemToArray(arr, cJSON_CreateString(s_or_empty(saved[i])));
  }
}

/* Print, frame as "\nOH<{...}\n" into buf, free root. -1 when it doesn't fit
 * (the line itself is held to USB_LINE_MAX) or on allocation failure. */
static int frame(cJSON *root, char *buf, size_t len) {
  if (!root) return -1;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return -1;
  size_t jl = strlen(json);
  size_t line = sizeof PREFIX_REPLY - 1 + jl;
  size_t total = 1 + line + 1;
  int rc = -1;
  if (line <= USB_LINE_MAX && total + 1 <= len) {
    buf[0] = '\n';
    memcpy(buf + 1, PREFIX_REPLY, sizeof PREFIX_REPLY - 1);
    memcpy(buf + 1 + sizeof PREFIX_REPLY - 1, json, jl);
    buf[total - 1] = '\n';
    buf[total] = '\0';
    rc = (int)total;
  }
  cJSON_free(json);
  return rc;
}

int usb_proto_reply_info(const usb_info_t *in, long id, char *buf, size_t len) {
  cJSON *root = reply_head("info", id, true);
  if (!root) return -1;
  char mac[18];
  snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
           in->mac[0], in->mac[1], in->mac[2], in->mac[3], in->mac[4], in->mac[5]);
  cJSON_AddNumberToObject(root, "proto", USB_PROTO_VERSION);
  cJSON_AddStringToObject(root, "track", s_or_empty(in->track));
  cJSON_AddStringToObject(root, "fw", s_or_empty(in->fw));
  cJSON_AddStringToObject(root, "mac", mac);
  cJSON_AddStringToObject(root, "name", s_or_empty(in->name));
  cJSON_AddStringToObject(root, "wifi_ssid", s_or_empty(in->wifi_ssid));
  cJSON_AddStringToObject(root, "bot_url", s_or_empty(in->bot_url));
  cJSON_AddBoolToObject(root, "bot_url_saved", in->bot_url_saved);
  cJSON_AddStringToObject(root, "link", s_or_empty(in->link));
  cJSON_AddStringToObject(root, "ip", s_or_empty(in->ip));
  cJSON_AddNumberToObject(root, "up_s", (double)in->up_s);
  return frame(root, buf, len);
}

int usb_proto_reply_ok(const char *cmd, long id, const char *const *saved, int n_saved,
                       char *buf, size_t len) {
  cJSON *root = reply_head(cmd, id, true);
  if (!root) return -1;
  add_saved(root, saved, n_saved);
  return frame(root, buf, len);
}

int usb_proto_reply_err(const char *cmd, long id, const char *error,
                        const char *const *saved, int n_saved, char *buf, size_t len) {
  cJSON *root = reply_head(cmd, id, false);
  if (!root) return -1;
  cJSON_AddStringToObject(root, "error", s_or_empty(error));
  add_saved(root, saved, n_saved);
  return frame(root, buf, len);
}
