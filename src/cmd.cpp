/* cmd.cpp — bot→node command schema over the data channel (firmware 1.1.0).
 *
 * Envelope: {"cmd": "<name>", "id": <n>, ...args}
 * Replies:  {"evt": "cmd_result", "cmd": "<name>", "id": <n>, "ok": bool,
 *            "data": {...}}
 *
 * Existing RTVI "type" messages are untouched; anything with a "cmd"
 * field routes here (rtvi.cpp tries cmd first). One channel, versioned
 * by the schema doc — design note protocol note: scan, set_wifi,
 * set_name, restart, get_info, ping.
 */

#include "cmd.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "main.h"
#include "rtvi.h"
#include "settings.h"

static const char *TAG = "cmd";

/* Reply helper shared by every handler: wraps ok + note in the
 * cmd_result envelope. (Declared in cmd.h; defined before use.) */
void cmd_send_simple(const char *cmd, bool ok, const char *note) {
  char reply[256];
  snprintf(reply, sizeof(reply),
           "{\"evt\":\"cmd_result\",\"cmd\":\"%s\",\"ok\":%s,"
           "\"data\":\"%s\"}",
           cmd, ok ? "true" : "false", note);
  pipecat_rtvi_send_raw(reply);
}

/* Pending Wi-Fi change: apply → verify → rollback (design note flow —
 * a typo from the dashboard must never cost a trip to the node). */
static bool s_wifi_pending = false;
static char s_old_ssid[33] = "";
static char s_old_pass[64] = "";
static int s_wifi_failures = 0;

void cmd_wifi_on_connected(void) {
  if (s_wifi_pending) {
    s_wifi_pending = false;
    ESP_LOGI(TAG, "new wifi creds VERIFIED (got ip)");
    cmd_send_simple("set_wifi", true, "connected");
  }
}

void cmd_wifi_on_disconnected(void) {
  if (!s_wifi_pending) return;
  if (++s_wifi_failures >= 3) {
    ESP_LOGW(TAG, "new wifi creds FAILED 3x — rolling back");
    settings_set_wifi(s_old_ssid, s_old_pass);
    s_wifi_pending = false;
    s_wifi_failures = 0;
    cmd_send_simple("set_wifi", false, "rollback: could not connect");
  }
}

static void do_scan(void) {
  /* Scan from the RTVI task wedges the data channel ~2s — acceptable
   * for a dashboard-triggered scan (the bot knows to wait). Blocking
   * scan while associated is supported by the WiFi driver. */
  wifi_scan_config_t cfg = {};
  esp_wifi_scan_start(&cfg, true);
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 24) n = 24;  /* cap the reply size */
  wifi_ap_record_t *aps =
      (wifi_ap_record_t *)malloc(n * sizeof(wifi_ap_record_t));
  char *out = (char *)malloc(1536);
  if (!aps || !out) {
    cmd_send_simple("scan", false, "oom");
    free(aps); free(out);
    return;
  }
  uint16_t got = n;
  esp_wifi_scan_get_ap_records(&got, aps);
  int off = 0;
  for (int i = 0; i < got; i++) {
    /* dedupe: driver returns one entry per channel per SSID */
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (strncmp((char *)aps[j].ssid, (char *)aps[i].ssid, 32) == 0) {
        dup = true; break;
      }
    }
    if (dup) continue;
    off += snprintf(out + off, 1536 - off, "%s\"%.32s\":%d",
                    off > 1 ? "," : "", aps[i].ssid, aps[i].rssi);
    if (off > 1400) break;
  }
  char reply[1600];
  snprintf(reply, sizeof(reply),
           "{\"evt\":\"cmd_result\",\"cmd\":\"scan\",\"ok\":true,"
           "\"data\":{%s}}",
           out);
  pipecat_rtvi_send_raw(reply);
  ESP_LOGI(TAG, "scan: replied %d SSIDs", got);
  free(aps);
  free(out);
}

bool pipecat_cmd_handle(const cJSON *msg) {
  const cJSON *j_cmd = cJSON_GetObjectItem(msg, "cmd");
  if (j_cmd == NULL || !cJSON_IsString(j_cmd)) return false;
  const char *cmd = j_cmd->valuestring;
  const cJSON *j_id = cJSON_GetObjectItem(msg, "id");

  if (strcmp(cmd, "ping") == 0) {
    cmd_send_simple("ping", true, "pong");
  } else if (strcmp(cmd, "restart") == 0) {
    cmd_send_simple("restart", true, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));  /* let the ack leave the wire */
    esp_restart();
  } else if (strcmp(cmd, "get_info") == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char reply[512];
    snprintf(reply, sizeof(reply),
             "{\"evt\":\"cmd_result\",\"cmd\":\"get_info\",\"ok\":true,"
             "\"data\":{\"name\":\"%s\",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
             "\"fw\":\"%s\"}}",
             settings_get_node_name(), mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5], esp_app_get_description()->version);
    pipecat_rtvi_send_raw(reply);
  } else if (strcmp(cmd, "set_name") == 0) {
    const cJSON *j_name = cJSON_GetObjectItem(msg, "name");
    if (!cJSON_IsString(j_name)) {
      cmd_send_simple("set_name", false, "missing name");
    } else {
      int rc = settings_set_node_name(j_name->valuestring);
      cmd_send_simple("set_name", rc == 0, rc == 0 ? "ok" : "nvs error");
    }
  } else if (strcmp(cmd, "scan") == 0) {
    do_scan();
  } else if (strcmp(cmd, "set_wifi") == 0) {
    const cJSON *j_ssid = cJSON_GetObjectItem(msg, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(msg, "pass");
    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass)) {
      cmd_send_simple("set_wifi", false, "missing ssid/pass");
    } else {
      strlcpy(s_old_ssid, settings_get_wifi_ssid(), sizeof(s_old_ssid));
      strlcpy(s_old_pass, settings_get_wifi_pass(), sizeof(s_old_pass));
      int rc = settings_set_wifi(j_ssid->valuestring, j_pass->valuestring);
      if (rc != 0) {
        cmd_send_simple("set_wifi", false, "nvs error");
      } else {
        s_wifi_pending = true;
        s_wifi_failures = 0;
        cmd_send_simple("set_wifi", true, "stored, connecting");
        esp_wifi_disconnect();  /* event handler reconnects with new creds */
      }
    }
  } else {
    ESP_LOGW(TAG, "unknown cmd: %s", cmd);
    cmd_send_simple(cmd, false, "unknown cmd");
  }
  return true;
}

/* ── Session liveness (023 zombie fix) ─────────────────────────────
 * Every 5 min the node pings over the data channel; the bot always
 * answers. A silent session (bot gone, or SCTP zombie) gets 90s to
 * prove itself, then a reboot re-rolls the handshake. */
static bool s_pong_received = true;

void cmd_note_activity(void) { s_pong_received = true; }

static void liveness_task(void *arg) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(300000));
    if (!s_pong_received) {
      ESP_LOGE(TAG, "no pong for 90s — session zombie, reboot re-roll");
      esp_restart();
    }
    s_pong_received = false;
    pipecat_rtvi_send_raw(
        "{\"evt\":\"cmd_result\",\"cmd\":\"ping\",\"ok\":true,"
        "\"data\":\"probe\"}");
    /* give the round-trip 90s inside the next 5-min window */
  }
}

void cmd_liveness_start(void) {
  static StaticTask_t buf;
  static StackType_t stack[4096 / sizeof(StackType_t)];
  xTaskCreateStaticPinnedToCore(liveness_task, "liveness",
                                4096 / sizeof(StackType_t), NULL, 3,
                                stack, &buf, 0);
}