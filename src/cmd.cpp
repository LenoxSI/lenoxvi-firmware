/* cmd.cpp — bot<->node command schema over the data channel (firmware 1.1.0+).
 *
 * Envelope: {"cmd": "<name>", "id": <n>, ...args}
 * Replies:  {"id": <n>, "evt": "cmd_result", "cmd": "<name>", "ok": bool,
 *            "data": {...}}
 * The reply echoes the request's "id" (1.2.37; a request without one gets a
 * reply without one). Unprompted pushes never carry an id.
 *
 * Existing RTVI "type" messages are untouched; anything with a string "cmd"
 * field routes here (rtvi.cpp tries cmd first). Bot -> node: ping, pong,
 * restart, get_info, set_mute, set_name, scan, set_wifi, and set_speaker
 * (waveshare). Node -> bot, unprompted in the same envelope: fw_push (on
 * channel open), mute_push (array button), and probe, whose "data" says why:
 * "liveness" (30 s after connect, then every 5 min) or "ota_health" (once at
 * channel open while a fresh OTA image awaits its first bot round-trip).
 * The bot answers a probe with {"cmd":"pong"}.
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
#include <stdlib.h>

#include "main.h"
#include "settings.h"
#include "wifi_trial.h"
#include "cmd_json.h"

/* libpeer patch 0004 (agent.c): keepalive counters since boot. Declared here
 * because peer.h doesn't expose agent.h. */
extern "C" void agent_keepalive_stats(uint32_t *sent, uint32_t *resent, uint32_t *rescued);

static const char *TAG = "cmd";

/* Request id of the command being handled. Every reply sent while handling
 * it echoes the id, so the bot can match replies to requests (without it, a
 * timed-out command's late reply could answer the retry). -1 = the request
 * carried no id (older bots), or no request at all (unprompted pushes).
 * Commands are handled one at a time on the RTVI task. */
static int s_req_id = -1;

/* Send a reply to the request being handled, with its id injected. */
static void cmd_reply_raw(const char *json) {
  if (s_req_id < 0 || json == NULL || json[0] != '{' || json[1] == '}') {
    pipecat_rtvi_send_raw(json);
    return;
  }
  size_t n = strlen(json) + 24;
  char *buf = (char *)malloc(n);
  if (buf == NULL) {  /* oom: the reply still goes out, just without the id */
    pipecat_rtvi_send_raw(json);
    return;
  }
  snprintf(buf, n, "{\"id\":%d,%s", s_req_id, json + 1);
  pipecat_rtvi_send_raw(buf);
  free(buf);
}

/* Reply helper shared by every handler: wraps ok + note in the
 * cmd_result envelope. (Declared in cmd.h; defined before use.) */
void cmd_send_simple(const char *cmd, bool ok, const char *note) {
  /* M7 (1.2.20): built with cJSON (cmd_json.c) — `cmd` can be an echo of
   * whatever the bot sent (the unknown-cmd reply), so it's escaped, never
   * pasted. "type":"cmd_result" is still REQUIRED (pipecat drops messages
   * without it, measured 2026-09-29) — the builder always sets it. */
  char *reply = cmd_json_simple(cmd, ok, note);
  if (!reply) { ESP_LOGE(TAG, "reply: oom (%s)", cmd ? cmd : "?"); return; }
  cmd_reply_raw(reply);
  cmd_json_free(reply);
}

/* ── NVS writes must run on an internal-RAM stack ──────────────────
 * The RTVI task's stack is PSRAM ("RTVI task spawned (PSRAM stack)",
 * webrtc.cpp). NVS commit suspends the flash cache — a stack access
 * to PSRAM inside that window double-faults. Measured 2026-09-29
 * ~19:20: every set_name → "Guru Meditation: Core 0 panic'ed
 * (Double exception)", A1=0x60100000 (PSRAM SP), backtrace CORRUPTED,
 * node rebooted before acking. Reads are safe (no cache suspension —
 * settings_load at boot is fine); WRITES go through this runner: one
 * long-lived worker task on a static internal stack, created on first
 * use and never deleted. (A task per write on the same static buffers was
 * unsafe: a deleted task's TCB is only reaped later by the idle task, so a
 * second write straight after the first could re-create a task on a TCB
 * the kernel still had on its lists.) */
typedef struct {
  int kind;  /* 0 = set_name, 1 = set_wifi (starts a trial), 2 = save the mute state */
  const char *a, *b;
  int rc;
} nvs_job_t;

static nvs_job_t s_job;
static SemaphoreHandle_t s_job_lock;  /* one job at a time */
static SemaphoreHandle_t s_job_done;
static TaskHandle_t s_job_worker;

static void nvs_job_task(void *arg) {
  (void)arg;
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* nvs_run posts a job */
    /* Mute saves the state as it is NOW, not as it was when the job was
     * posted: a button press in between must not be overwritten by the
     * older value. */
    s_job.rc = s_job.kind == 0 ? settings_set_node_name(s_job.a)
             : s_job.kind == 1 ? settings_begin_wifi_trial(s_job.a, s_job.b)
                               : settings_set_muted(pipecat_mic_muted());
    xSemaphoreGive(s_job_done);
  }
}

static int nvs_run(int kind, const char *a, const char *b) {
  /* A plain static array is internal DRAM — it must stay that way (never
   * PSRAM), or the write double-faults exactly as on the RTVI stack. */
  static StaticTask_t buf;
  static StackType_t stack[6144 / sizeof(StackType_t)];
  /* One-time setup: 0 = not yet, 1 = being set up, 2 = ready. The claim
   * keeps two first callers from building two workers on one buffer. */
  static int ready = 0;
  int st = 0;
  if (__atomic_compare_exchange_n(&ready, &st, 1, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    if (s_job_lock == NULL) s_job_lock = xSemaphoreCreateMutex();
    if (s_job_done == NULL) s_job_done = xSemaphoreCreateBinary();
    if (s_job_lock == NULL || s_job_done == NULL) {
      __atomic_store_n(&ready, 0, __ATOMIC_RELEASE);  /* retry next call */
      return -100;
    }
    s_job_worker = xTaskCreateStaticPinnedToCore(
        nvs_job_task, "nvs_write", 6144 / sizeof(StackType_t), NULL,
        5, stack, &buf, 0);
    if (s_job_worker == NULL) {
      __atomic_store_n(&ready, 0, __ATOMIC_RELEASE);
      return -102;
    }
    __atomic_store_n(&ready, 2, __ATOMIC_RELEASE);
  } else {
    while ((st = __atomic_load_n(&ready, __ATOMIC_ACQUIRE)) == 1) {
      vTaskDelay(1);  /* another caller is creating the worker */
    }
    if (st != 2) return -100;
  }
  xSemaphoreTake(s_job_lock, portMAX_DELAY);
  s_job.kind = kind; s_job.a = a; s_job.b = b; s_job.rc = -101;
  xTaskNotifyGive(s_job_worker);
  xSemaphoreTake(s_job_done, portMAX_DELAY);
  int rc = s_job.rc;
  xSemaphoreGive(s_job_lock);
  return rc;
}

static void do_scan(void) {
  /* Scan from the RTVI task wedges the data channel ~2s — acceptable
   * for a dashboard-triggered scan (the bot knows to wait). Blocking
   * scan while associated is supported by the WiFi driver. */
  wifi_scan_config_t cfg = {};
  esp_err_t err = esp_wifi_scan_start(&cfg, true);
  if (err != ESP_OK) {
    /* e.g. ESP_ERR_WIFI_STATE while (re)associating: say so, rather than
     * reply an empty list the dashboard shows as "no networks". */
    ESP_LOGW(TAG, "scan: start failed: %s", esp_err_to_name(err));
    cmd_send_simple("scan", false, esp_err_to_name(err));
    return;
  }
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  /* Fetch more access points than we reply with: a mesh network shows up
   * once per access point, so the cap is applied to distinct names below,
   * not to raw entries (that could cut the user's own network). */
  if (n > 48) n = 48;
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
    char ssid[33], clean[33];
    memcpy(ssid, aps[i].ssid, 32);
    ssid[32] = 0;
    /* Hidden networks have no name to pick. A name that isn't valid UTF-8
     * would be shown with '?' substitutions — a name that doesn't exist on
     * air, so joining it could only fail. Leave both out. */
    if (ssid[0] == 0) continue;
    cmd_json_utf8_sanitize(ssid, clean, sizeof(clean));
    if (strcmp(ssid, clean) != 0) continue;
    /* dedupe: the driver returns one entry per access point; keep the
     * strongest signal for each name */
    int j = 0;
    while (j < kept && strcmp((char *)names[j], ssid) != 0) j++;
    if (j < kept) {
      if (aps[i].rssi > rssi[j]) rssi[j] = aps[i].rssi;
      continue;
    }
    memcpy(names[kept], ssid, 33);
    rssi[kept] = aps[i].rssi;
    kept++;
  }
  /* Strongest first, then cap the reply size: the networks cut (here, or by
   * the byte cap in cmd_json_scan) are the weakest ones. */
  for (int i = 1; i < kept; i++) {
    for (int j = i; j > 0 && rssi[j] > rssi[j - 1]; j--) {
      uint8_t t_name[33];
      memcpy(t_name, names[j], 33);
      memcpy(names[j], names[j - 1], 33);
      memcpy(names[j - 1], t_name, 33);
      int8_t t_rssi = rssi[j];
      rssi[j] = rssi[j - 1];
      rssi[j - 1] = t_rssi;
    }
  }
  if (kept > 24) kept = 24;
  /* M7 (1.2.20): network names come off the AIR — any neighbour can name a
   * network `a","x":1`. cJSON escapes them and invalid UTF-8 becomes '?'
   * (cmd_json.c); output capped like the old 1400 B buffer. */
  char *reply = cmd_json_scan((const uint8_t (*)[33])names, rssi, kept, 1500);
  if (reply) {
    cmd_reply_raw(reply);
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

static bool cmd_dispatch(const cJSON *msg) {
  const cJSON *j_cmd = cJSON_GetObjectItem(msg, "cmd");
  if (j_cmd == NULL || !cJSON_IsString(j_cmd)) return false;
  const char *cmd = j_cmd->valuestring;

  if (strcmp(cmd, "ping") == 0) {
    cmd_send_simple("ping", true, "pong");
  } else if (strcmp(cmd, "pong") == 0) {
    /* Liveness answer from the bot (cmd "probe" above). Activity is
     * noted by rtvi.cpp's dispatch — deliberately NO reply here:
     * asymmetry is what makes the loop structurally impossible. */
  } else if (strcmp(cmd, "restart") == 0) {
    if (pipecat_ota_busy()) {
      /* A reboot mid-flash throws the download away; the OTA task reboots
       * into the new image itself when the flash completes. */
      cmd_send_simple("restart", false, "ota in progress");
    } else {
      cmd_send_simple("restart", true, "rebooting");
      vTaskDelay(pdMS_TO_TICKS(300));  /* let the ack leave the wire */
      boot_diag_restart("cmd-restart");
    }
  } else if (strcmp(cmd, "get_info") == 0) {
    int dbg_btn;
    pipecat_mute_hw_diag(&dbg_btn);   /* array: raw mute-button bit (support/diagnostics) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    cmd_audio_stats_t au;
    pipecat_audio_stats(&au);         /* audio-loop health (1.2.27) */
    wifi_ap_record_t ap;              /* reboot black box + link (1.2.29) */
    cmd_ka_stats_t ka;  /* libpeer 0004 keepalive counters (1.2.36) */
    agent_keepalive_stats(&ka.sent, &ka.resent, &ka.rescued);
    cmd_link_stats_t link;
    pipecat_wifi_link_stats(&link);
    cmd_ota_status_t ota;
    pipecat_ota_status(&ota);
    cmd_boot_info_t bi = {boot_diag_reason(), boot_diag_prev_why(),
                          boot_diag_prev_up_s(), boot_diag_up_s(),
                          esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0, &ka,
                          &link, &ota};
    char *reply = cmd_json_info(settings_get_node_name(), mac,
                                esp_app_get_description()->version, XVF_OTA_TRACK,
                                pipecat_mic_muted(), dbg_btn, &au, &bi);
    if (reply) {
      cmd_reply_raw(reply);
      cmd_json_free(reply);
    } else {
      cmd_send_simple("get_info", false, "oom");
    }
  } else if (strcmp(cmd, "set_mute") == 0) {
    /* Mic mute (flex 1.1.0; both boards since 1.2.11, H4): "mute":
     * true|false, or omit for toggle. Persisted; on array it drives the
     * same XMOS pin as the physical button (red LED). Echoes the resulting
     * state so the dashboard renders truth from the ack, not optimistic UI.
     * A "mute" that is present but not a bool is refused: an explicit value
     * must never invert the state. */
    const cJSON *j_mute = cJSON_GetObjectItem(msg, "mute");
    if (j_mute != NULL && !cJSON_IsBool(j_mute)) {
      cmd_send_simple("set_mute", false, "mute must be bool");
      return true;
    }
    bool now;
    if (j_mute == NULL) {
      now = pipecat_mic_mute_toggle();
    } else {
      now = pipecat_mic_mute_set(cJSON_IsTrue(j_mute));
    }
    /* Persist through the internal-stack runner — THIS task's stack is in
     * PSRAM and a direct NVS write double-faults the core (the 2026-09-29
     * set_name crash; repeated by the first 1.2.11 build, 2026-10-16). The
     * mute itself already applied above; a failed save only costs
     * persistence, and says so in the log. */
    if (nvs_run(2, NULL, NULL) != 0) {
      ESP_LOGW(TAG, "set_mute: applied but NOT saved (nvs error)");
    }
    char reply[160];
    snprintf(reply, sizeof(reply),
             "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\","
             "\"cmd\":\"set_mute\",\"ok\":true,"
             "\"data\":{\"muted\":%s}}",
             now ? "true" : "false");
    cmd_reply_raw(reply);
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
          cmd_reply_raw(reply);
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
     * verifies by REACHING THE BOT (a bot message over the data channel, not
     * just an IP or an HTTP answer) and rolls back to the previous network
     * after 3 failed boots (wifi_trial.h). The old flow kept the previous
     * creds in RAM and called "got an IP" success — a guest network/VLAN
     * stranded the node in a reboot loop. */
    const cJSON *j_ssid = cJSON_GetObjectItem(msg, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(msg, "pass");
    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass) || !j_ssid->valuestring[0]) {
      cmd_send_simple("set_wifi", false, "missing ssid/pass");
    } else if (strlen(j_ssid->valuestring) > 32) {
      /* 802.11 caps a network name at 32 BYTES (a UTF-8 name can hit that
       * in fewer characters). A longer one would be stored, then fail to
       * load at the next boot, and the trial would burn three boots before
       * rolling back. */
      cmd_send_simple("set_wifi", false, "network name must be at most 32 bytes");
    } else if (!wifi_pass_acceptable(j_pass->valuestring)) {
      cmd_send_simple("set_wifi", false, "password must be up to 63 characters, or 64 hex digits");
    } else if (pipecat_ota_busy()) {
      /* Checked before the NVS write: storing a trial and then refusing to
       * reboot would leave it armed for whatever boot comes next. */
      cmd_send_simple("set_wifi", false, "ota in progress");
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

bool pipecat_cmd_handle(const cJSON *msg) {
  /* Every reply sent while handling this request echoes its id (if any). */
  const cJSON *j_id = cJSON_GetObjectItem(msg, "id");
  s_req_id = (cJSON_IsNumber(j_id) && j_id->valuedouble >= 0 && j_id->valuedouble <= 2147483647.0)
                 ? j_id->valueint : -1;
  bool handled = cmd_dispatch(msg);
  s_req_id = -1;
  return handled;
}

/* ── Session liveness (023 zombie fix) ─────────────────────────────
 * The node probes over the data channel; the bot always answers. The
 * first probe goes out 30 s after the session connects (1.2.35: a session
 * that is dead from the start used to go unnoticed for 5-10 min), then one
 * every 5 min. Each probe gets 90 s to be answered; any inbound bot traffic
 * counts as an answer (cmd_note_activity). No answer = reboot, which
 * re-rolls the handshake. */
#define LIVENESS_FIRST_MS   30000
#define LIVENESS_PERIOD_MS 300000
#define LIVENESS_WINDOW_MS  90000
static volatile bool s_pong_received = true;

void cmd_note_activity(void) {
  s_pong_received = true;
  /* Inbound bot traffic over the data channel = the image and the network
   * work end to end (ICE, DTLS and SCTP up — not just TCP to the bot). That
   * passes both gates: the OTA health gate, and a pending Wi-Fi trial (a
   * network that lets TCP through but blocks UDP must fail the trial). Both
   * are no-ops once passed; the Wi-Fi one only sets a flag (no NVS). */
  pipecat_ota_health_ok();
  pipecat_wifi_bot_reached();
}

static void liveness_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(LIVENESS_FIRST_MS));
  while (1) {
    s_pong_received = false;
    /* Probe is cmd "probe" — NOT "ping": the bot answers with
     * {"cmd":"pong"} and the pong handler never replies. The pair is
     * deliberately ASYMMETRIC — a reply-to-the-reply loop flooded the
     * channel at ~1 pong/sec for 12 min (measured 2026-09-29
     * 19:35–19:47) when probe/pong both used "ping". */
    pipecat_rtvi_send_raw(
        "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"probe\","
        "\"ok\":true,\"data\":\"liveness\"}");
    vTaskDelay(pdMS_TO_TICKS(LIVENESS_WINDOW_MS));
    if (!s_pong_received && pipecat_ota_busy()) {
      /* A firmware flash starves the session, so a missed pong is expected;
       * rebooting would throw the download away. Wait it out — a good flash
       * reboots into the new image, a failed one leaves us to decide. */
      ESP_LOGW(TAG, "no pong for 90s, but an OTA flash is in flight — holding the reboot");
      while (pipecat_ota_busy()) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!s_pong_received) {
      ESP_LOGE(TAG, "no pong for 90s — session zombie, reboot re-roll");
      boot_diag_restart("liveness-no-pong");
    }
    vTaskDelay(pdMS_TO_TICKS(LIVENESS_PERIOD_MS - LIVENESS_WINDOW_MS));
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
  cmd_reply_raw(reply);
  return true;
}
#endif