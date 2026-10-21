/* cmd.cpp — bot→node command schema over the data channel (firmware 1.1.0).
 *
 * Envelope: {"cmd": "<name>", "id": <n>, ...args}
 * Replies:  {"evt": "cmd_result", "cmd": "<name>", "id": <n>, "ok": bool,
 *            "data": {...}}
 *
 * Existing RTVI "type" messages are untouched; anything with a "cmd"
 * field routes here (rtvi.cpp tries cmd first). One channel, versioned
 * by the protocol schema: scan, set_wifi,
 * set_name, restart, get_info, ping.
 */

#include "cmd.h"
#include "boot_diag.h"

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
#include "wifi_trial.h"
#include "cmd_json.h"

static const char *TAG = "cmd";

/* Reply helper shared by every handler: wraps ok + note in the
 * cmd_result envelope. (Declared in cmd.h; defined before use.) */
void cmd_send_simple(const char *cmd, bool ok, const char *note) {
  /* M7 (1.2.20): built with cJSON (cmd_json.c) — `cmd` can be an echo of
   * whatever the bot sent (the unknown-cmd reply), so it's escaped, never
   * pasted. "type":"cmd_result" is still REQUIRED (pipecat drops messages
   * without it, measured 2026-09-29) — the builder always sets it. */
  char *reply = cmd_json_simple(cmd, ok, note);
  if (!reply) { ESP_LOGE(TAG, "reply: oom (%s)", cmd ? cmd : "?"); return; }
  pipecat_rtvi_send_raw(reply);
  cmd_json_free(reply);
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
  int kind;  /* 0 = set_name, 1 = set_wifi (starts a trial), 2 = mute (a != NULL = muted) */
  const char *a, *b;
  int rc;
} nvs_job_t;

static nvs_job_t s_job;
static SemaphoreHandle_t s_job_done;

static void nvs_job_task(void *arg) {
  nvs_job_t *job = (nvs_job_t *)arg;
  job->rc = job->kind == 0 ? settings_set_node_name(job->a)
          : job->kind == 1 ? settings_begin_wifi_trial(job->a, job->b)
                           : settings_set_muted(job->a != NULL);
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

static void do_scan(void) {
  /* Scan from the RTVI task wedges the data channel ~2s — acceptable
   * for a dashboard-triggered scan (the bot knows to wait). Blocking
   * scan while associated is supported by the WiFi driver. */
  wifi_scan_config_t cfg = {};
  esp_wifi_scan_start(&cfg, true);
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 24) n = 24;  /* cap the reply size */
  /* +1: malloc(0) may return NULL — an empty scan is an empty list, not
   * "oom" (the pre-1.2.20 code reported oom with no networks in range). */
  wifi_ap_record_t *aps =
      (wifi_ap_record_t *)malloc((n + 1) * sizeof(wifi_ap_record_t));
  uint8_t (*names)[33] = (uint8_t (*)[33])malloc((n + 1) * 33);
  int8_t *rssi = (int8_t *)malloc(n + 1);
  if (!aps || !names || !rssi) {
    cmd_send_simple("scan", false, "oom");
    free(aps); free(names); free(rssi);
    return;
  }
  uint16_t got = n;
  esp_wifi_scan_get_ap_records(&got, aps);
  int kept = 0;
  for (int i = 0; i < got; i++) {
    /* dedupe: driver returns one entry per channel per SSID */
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (strncmp((char *)aps[j].ssid, (char *)aps[i].ssid, 32) == 0) {
        dup = true; break;
      }
    }
    if (dup) continue;
    memcpy(names[kept], aps[i].ssid, 33);
    names[kept][32] = 0;
    rssi[kept] = aps[i].rssi;
    kept++;
  }
  /* M7 (1.2.20): network names come off the AIR — any neighbour can name a
   * network `a","x":1`. cJSON escapes them and invalid UTF-8 becomes '?'
   * (cmd_json.c); output capped like the old 1400 B buffer. */
  char *reply = cmd_json_scan((const uint8_t (*)[33])names, rssi, kept, 1500);
  if (reply) {
    pipecat_rtvi_send_raw(reply);
    cmd_json_free(reply);
  } else {
    cmd_send_simple("scan", false, "oom");
  }
  ESP_LOGI(TAG, "scan: replied %d SSIDs", kept);
  free(aps);
  free(names);
  free(rssi);
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
  /* "line" (1.2.19): the board line this image was built for — the same
   * compiled-in XVF_OTA_TRACK the OTA board guard uses (array|flex|waveshare),
   * so the dashboard can label each node's hardware. */
  /* if the bootloader rolled back from a version, say which —
   * "rejected" rides alongside "fw" (the bot ignores unknown keys today;
   * it's there so a silent rollback can be surfaced). Version strings come
   * from esp_app_desc (digits + dots), never user input. */
  /* "wifi_rollback" (1.2.35): this boot restored the previous network
   * after a failed Wi-Fi change (settings_wifi_rolled_back). Sent only when
   * true; an older bot ignores it. */
  char rejected[32];
  char extra[96] = "";
  if (pipecat_ota_rejected_version(rejected, sizeof(rejected))) {
    snprintf(extra, sizeof(extra), ",\"rejected\":\"%s\"", rejected);
  }
  if (settings_wifi_rolled_back()) {
    strlcat(extra, ",\"wifi_rollback\":true", sizeof(extra));
  }
  snprintf(reply, sizeof(reply),
           "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"fw_push\","
           "\"ok\":true,\"data\":{\"fw\":\"%s\",\"line\":\"%s\"%s,\"muted\":%s}}",
           app_desc->version, XVF_OTA_TRACK, extra, pipecat_mic_muted() ? "true" : "false");
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
    boot_diag_restart("cmd-restart");
  } else if (strcmp(cmd, "get_info") == 0) {
    int dbg_btn;
    pipecat_mute_hw_diag(&dbg_btn);   /* array: raw mute-button bit (support/diagnostics) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    cmd_audio_stats_t au;
    pipecat_audio_stats(&au);         /* audio-loop health (1.2.27) */
    wifi_ap_record_t ap;              /* reboot black box + link (1.2.29) */
    cmd_boot_info_t bi = {boot_diag_reason(), boot_diag_prev_why(),
                          boot_diag_prev_up_s(), boot_diag_up_s(),
                          esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0};
    char *reply = cmd_json_info(settings_get_node_name(), mac,
                                esp_app_get_description()->version, XVF_OTA_TRACK,
                                pipecat_mic_muted(), dbg_btn, &au, &bi);
    if (reply) {
      pipecat_rtvi_send_raw(reply);
      cmd_json_free(reply);
    } else {
      cmd_send_simple("get_info", false, "oom");
    }
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
    /* Persist through the internal-stack runner — THIS task's stack is in
     * PSRAM and a direct NVS write double-faults the core (the 2026-09-29
     * set_name crash; repeated by the first 1.2.11 build, 2026-10-16). The
     * mute itself already applied above; a failed save only costs
     * persistence, and says so in the log. */
    if (nvs_run(2, now ? "1" : NULL, NULL) != 0) {
      ESP_LOGW(TAG, "set_mute: applied but NOT saved (nvs error)");
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
        char *reply = cmd_json_name(settings_get_node_name());
        if (reply) {
          pipecat_rtvi_send_raw(reply);
          cmd_json_free(reply);
        } else {
          cmd_send_simple("set_name", false, "oom");
        }
      } else {
        cmd_send_simple("set_name", false, "nvs error");
      }
    }
  } else if (strcmp(cmd, "scan") == 0) {
    do_scan();
  } else if (strcmp(cmd, "set_wifi") == 0) {
    /* M1 (1.2.23): a Wi-Fi change is a TRIAL that survives reboots — store
     * previous + new + trial=1 in one NVS commit, ack, reboot. The boot path
     * verifies by REACHING THE BOT (offer POST 200, not just an IP) and rolls
     * back to the previous network after 3 failed boots (wifi_trial.h). The
     * old flow kept the previous creds in RAM and called "got an IP"
     * success — a guest network/VLAN stranded the node in a reboot loop. */
    const cJSON *j_ssid = cJSON_GetObjectItem(msg, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(msg, "pass");
    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass) || !j_ssid->valuestring[0]) {
      cmd_send_simple("set_wifi", false, "missing ssid/pass");
    } else if (!wifi_pass_acceptable(j_pass->valuestring)) {
      cmd_send_simple("set_wifi", false, "password must be up to 63 characters, or 64 hex digits");
    } else if (nvs_run(1, j_ssid->valuestring, j_pass->valuestring) != 0) {
      cmd_send_simple("set_wifi", false, "nvs error");
    } else {
      cmd_send_simple("set_wifi", true, "stored, rebooting to verify");
      vTaskDelay(pdMS_TO_TICKS(300));  /* let the ack leave the wire */
      boot_diag_restart("set-wifi");
    }
  } LENOXVI_CMD_BOARD_EXT(cmd, msg) else {  /* waveshare: set_speaker (cmd_board_handle) */
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
      boot_diag_restart("liveness-no-pong");
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

#if LENOXVI_BOARD_WAVESHARE  /* waveshare: built-in speaker control (cmd_board_handle, cmd.h) */
#include "board_audio.h"
/* set_speaker: {"enabled": bool, "volume": 0..100 (optional, default: keep,
 * clamped)}. Applies at once; NOT persisted (no NVS — a reboot comes back
 * disabled at volume 40). Echoes the resulting state, like set_mute. */
bool cmd_board_handle(const char *cmd, const cJSON *msg) {
  if (strcmp(cmd, "set_speaker") != 0) return false;
  const cJSON *j_en = cJSON_GetObjectItem(msg, "enabled");
  const cJSON *j_vol = cJSON_GetObjectItem(msg, "volume");
  if (j_en == NULL) {
    cmd_send_simple("set_speaker", false, "missing enabled");
    return true;
  }
  if (!cJSON_IsBool(j_en)) {
    cmd_send_simple("set_speaker", false, "enabled must be bool");
    return true;
  }
  if (j_vol != NULL && !cJSON_IsNumber(j_vol)) {
    cmd_send_simple("set_speaker", false, "volume must be a number");
    return true;
  }
  bool enabled = cJSON_IsTrue(j_en);
  int volume = board_audio_speaker_volume();
  if (j_vol != NULL) {
    double v = j_vol->valuedouble;
    volume = v < 0 ? 0 : v > 100 ? 100 : (int)v;
  }
  board_audio_speaker_set(enabled, volume);
  char reply[192];
  snprintf(reply, sizeof(reply),
           "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\","
           "\"cmd\":\"set_speaker\",\"ok\":true,"
           "\"data\":{\"enabled\":%s,\"volume\":%d,\"speaker_ok\":%s}}",
           enabled ? "true" : "false", board_audio_speaker_volume(),
           board_audio_speaker_ok() ? "true" : "false");
  pipecat_rtvi_send_raw(reply);
  return true;
}
#endif