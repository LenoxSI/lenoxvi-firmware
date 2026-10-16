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
#include <esp_wifi.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdio.h>

#include "main.h"
#include "settings.h"

static const char *TAG = "cmd";

/* Reply helper shared by every handler: wraps ok + note in the
 * cmd_result envelope. (Declared in cmd.h; defined before use.) */
void cmd_send_simple(const char *cmd, bool ok, const char *note) {
  char reply[256];
  /* "type":"cmd_result" is REQUIRED: pipecat's datachannel parser
   * does json["type"] on every inbound message and drops the message
   * on KeyError — measured live 2026-09-29 18:04 ("Error parsing JSON
   * message …, 'type'"). The LED path uses the same trick. */
  snprintf(reply, sizeof(reply),
           "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"%s\","
           "\"ok\":%s,\"data\":\"%s\"}",
           cmd, ok ? "true" : "false", note);
  pipecat_rtvi_send_raw(reply);
}

/* ── NVS writes must run on an internal-RAM stack ──────────────────
 * The RTVI task's stack is PSRAM ("RTVI task spawned (PSRAM stack)",
 * webrtc.cpp). NVS commit suspends the flash cache — a stack access
 * to PSRAM inside that window double-faults. Measured 2026-09-29
 * ~19:20: every set_name → "Guru Meditation: Core 0 panic'ed
 * (Double exception)", A1=0x60100000 (PSRAM SP), backtrace CORRUPTED,
 * node rebooted before acking. Reads are safe (no cache suspension —
 * settings_load at boot is fine); WRITES go through this runner: a
 * short-lived task on a static internal stack. One slot is safe —
 * the RTVI task serializes commands (same pattern as liveness). */
typedef struct {
  int kind;  /* 0 = set_name, 1 = set_wifi */
  const char *a, *b;
  int rc;
} nvs_job_t;

static nvs_job_t s_job;
static SemaphoreHandle_t s_job_done;

static void nvs_job_task(void *arg) {
  nvs_job_t *job = (nvs_job_t *)arg;
  job->rc = job->kind == 0 ? settings_set_node_name(job->a)
                           : settings_set_wifi(job->a, job->b);
  xSemaphoreGive(s_job_done);
  vTaskDelete(NULL);
}

static int nvs_run(int kind, const char *a, const char *b) {
  static StaticTask_t buf;
  static StackType_t stack[6144 / sizeof(StackType_t)];
  if (s_job_done == NULL) {
    s_job_done = xSemaphoreCreateBinary();
    if (s_job_done == NULL) return -100;
  }
  s_job.kind = kind; s_job.a = a; s_job.b = b; s_job.rc = -101;
  TaskHandle_t th = xTaskCreateStaticPinnedToCore(
      nvs_job_task, "nvs_write", 6144 / sizeof(StackType_t), &s_job,
      5, stack, &buf, 0);
  if (th == NULL) return -102;
  xSemaphoreTake(s_job_done, portMAX_DELAY);
  return s_job.rc;
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
    /* 1.1.1: restore the OLD creds into the driver too, or the rollback
     * has the same stale-config bug in reverse — the NVS restore would
     * be invisible to the next esp_wifi_connect(). */
    pipecat_wifi_apply_runtime_config();
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
           "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"scan\","
           "\"ok\":true,\"data\":{%s}}",
           out);
  pipecat_rtvi_send_raw(reply);
  ESP_LOGI(TAG, "scan: replied %d SSIDs", got);
  free(aps);
  free(out);
}

/* ── connect-push: firmware version → bot, once per data-channel open.
 * (1.1.2) The bot's Nodes table shows each node's ACTUALLY-RUNNING fw
 * (the OTA manifest can only say what's AVAILABLE). Rides the existing
 * cmd_result envelope — nothing new invented on the wire. Pushed on the
 * data-channel-open path so it survives every reconnect + OTA reboot;
 * the bot needs no request lane and can never ask mid-audio.
 */
void cmd_push_firmware_version(void) {
  const esp_app_desc_t *app_desc = esp_app_get_description();
  char reply[256];
  /* if the bootloader rolled back from a version, say which —
   * "rejected" rides alongside "fw" (the bot ignores unknown keys today;
   * it's there so a silent rollback can be surfaced). Version strings come
   * from esp_app_desc (digits + dots), never user input. */
  char rejected[32];
  if (pipecat_ota_rejected_version(rejected, sizeof(rejected))) {
    snprintf(reply, sizeof(reply),
             "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"fw_push\","
             "\"ok\":true,\"data\":{\"fw\":\"%s\",\"rejected\":\"%s\",\"muted\":%s}}",
             app_desc->version, rejected, pipecat_mic_muted() ? "true" : "false");
  } else {
    snprintf(reply, sizeof(reply),
             "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"fw_push\","
             "\"ok\":true,\"data\":{\"fw\":\"%s\",\"muted\":%s}}",
             app_desc->version, pipecat_mic_muted() ? "true" : "false");
  }
  pipecat_rtvi_send_raw(reply);
  ESP_LOGI(TAG, "fw_push: sent running version %s", app_desc->version);
}

/* H4 (2026-10-16): a physical-button mute change (array) is told to the bot
 * unprompted, same envelope as fw_push. Called from the WebRTC loop — the
 * task that owns sending — never from the LED task that detected it. */
void cmd_service_mute_push(void) {
  if (!pipecat_mute_push_take()) return;
  char reply[128];
  snprintf(reply, sizeof(reply),
           "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"mute_push\","
           "\"ok\":true,\"data\":{\"muted\":%s}}",
           pipecat_mic_muted() ? "true" : "false");
  pipecat_rtvi_send_raw(reply);
  ESP_LOGI(TAG, "mute_push: sent muted=%s", pipecat_mic_muted() ? "true" : "false");
}

bool pipecat_cmd_handle(const cJSON *msg) {
  const cJSON *j_cmd = cJSON_GetObjectItem(msg, "cmd");
  if (j_cmd == NULL || !cJSON_IsString(j_cmd)) return false;
  const char *cmd = j_cmd->valuestring;
  const cJSON *j_id = cJSON_GetObjectItem(msg, "id");

  if (strcmp(cmd, "ping") == 0) {
    cmd_send_simple("ping", true, "pong");
  } else if (strcmp(cmd, "pong") == 0) {
    /* Liveness answer from the bot (cmd "probe" above). Activity is
     * noted by rtvi.cpp's dispatch — deliberately NO reply here:
     * asymmetry is what makes the loop structurally impossible. */
  } else if (strcmp(cmd, "restart") == 0) {
    cmd_send_simple("restart", true, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));  /* let the ack leave the wire */
    esp_restart();
  } else if (strcmp(cmd, "get_info") == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char reply[512];
    snprintf(reply, sizeof(reply),
             "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"get_info\","
             "\"ok\":true,\"data\":{\"name\":\"%s\",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
             "\"fw\":\"%s\",\"muted\":%s}}",
             settings_get_node_name(), mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5], esp_app_get_description()->version,
             pipecat_mic_muted() ? "true" : "false");
    pipecat_rtvi_send_raw(reply);
  } else if (strcmp(cmd, "set_mute") == 0) {
    /* Mic mute (flex 1.1.0; both boards since 1.2.11, H4): "mute":
     * true|false, or omit for toggle. Persisted; on array it drives the
     * same XMOS pin as the physical button (red LED). Echoes the resulting
     * state so the dashboard renders truth from the ack, not optimistic UI. */
    const cJSON *j_mute = cJSON_GetObjectItem(msg, "mute");
    bool now;
    if (j_mute == NULL || !cJSON_IsBool(j_mute)) {
      now = pipecat_mic_mute_toggle();
    } else {
      now = pipecat_mic_mute_set(cJSON_IsTrue(j_mute));
    }
    char reply[160];
    snprintf(reply, sizeof(reply),
             "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\","
             "\"cmd\":\"set_mute\",\"ok\":true,"
             "\"data\":{\"muted\":%s}}",
             now ? "true" : "false");
    pipecat_rtvi_send_raw(reply);
  } else if (strcmp(cmd, "set_name") == 0) {
    const cJSON *j_name = cJSON_GetObjectItem(msg, "name");
    if (!cJSON_IsString(j_name)) {
      cmd_send_simple("set_name", false, "missing name");
    } else {
      int rc = nvs_run(0, j_name->valuestring, NULL);
      if (rc == 0) {
        /* Echo the stored name: the bot's registry relabels the room
         * from THIS ack (immediately, no reboot needed). Plain "ok"
         * left the dashboard showing the old label until re-offer
         * (measured 2026-09-29 19:38). */
        char reply[160];
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\","
                 "\"cmd\":\"set_name\",\"ok\":true,"
                 "\"data\":{\"name\":\"%s\"}}",
                 settings_get_node_name());
        pipecat_rtvi_send_raw(reply);
      } else {
        cmd_send_simple("set_name", false, "nvs error");
      }
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
      int rc = nvs_run(1, j_ssid->valuestring, j_pass->valuestring);
      if (rc != 0) {
        cmd_send_simple("set_wifi", false, "nvs error");
      } else {
        s_wifi_pending = true;
        s_wifi_failures = 0;
        cmd_send_simple("set_wifi", true, "stored, connecting");
        /* 1.1.1: push the NEW creds into the driver BEFORE disconnect —
         * esp_wifi_connect() alone reuses the boot-time config and
         * "verifies" the old password (measured 2026-10-03). */
        pipecat_wifi_apply_runtime_config();
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

void cmd_note_activity(void) {
  s_pong_received = true;
  /* inbound bot traffic over the data channel = the image works
   * end to end -> pass the OTA health gate (no-op once validated). */
  pipecat_ota_health_ok();
}

static void liveness_task(void *arg) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(300000));
    if (!s_pong_received) {
      ESP_LOGE(TAG, "no pong for 90s — session zombie, reboot re-roll");
      esp_restart();
    }
    s_pong_received = false;
    /* Probe is cmd "probe" — NOT "ping": the bot answers with
     * {"cmd":"pong"} and the pong handler never replies. The pair is
     * deliberately ASYMMETRIC — a reply-to-the-reply loop flooded the
     * channel at ~1 pong/sec for 12 min (measured 2026-09-29
     * 19:35–19:47) when probe/pong both used "ping". */
    pipecat_rtvi_send_raw(
        "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"probe\","
        "\"ok\":true,\"data\":\"liveness\"}");
    /* give the round-trip 90s inside the next 5-min window */
  }
}

void cmd_liveness_start(void) {
  static bool started = false;
  static StaticTask_t buf;
  static StackType_t stack[4096 / sizeof(StackType_t)];
  /* webrtc.cpp calls this on every PeerConnectionState CONNECTED —
   * spawn exactly one liveness task per boot, ever (the static
   * buf/stack are single-use; a second spawn would corrupt them). */
  if (started) return;
  started = true;
  xTaskCreateStaticPinnedToCore(liveness_task, "liveness",
                                4096 / sizeof(StackType_t), NULL, 3,
                                stack, &buf, 0);
}