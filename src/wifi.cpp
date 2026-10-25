#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "cmd_json.h"
#include "boot_diag.h"
#include "rgb.h"
#include "settings.h"
#include "cmd.h"

static bool g_wifi_connected = false;
static bool g_webrtc_started = false;

// WebRTC bring-up on its own task — the sys_evt task's stack can't hold
// DTLS cert generation + ICE + SDP (stack overflow observed 2026-09-15).
static void pipecat_webrtc_init_task(void *user_data) {
  pipecat_init_webrtc();
  vTaskDelete(NULL);
}

static void pipecat_webrtc_init_task(void *pvParameter);

void pipecat_start_webrtc() {
  // Serialized after the boot OTA check (got-ip handler, 2026-09-19):
  // WebRTC init must not race OTA (heap) and OTA flashing must not race
  // flash-cached code execution (double exception). boot_ota calls this
  // when it's done (or immediately if its stack alloc failed).
  static bool webrtc_started = false;
  if (webrtc_started) return;
  webrtc_started = true;
  static StackType_t webrtc_stack[8192];
  static StaticTask_t webrtc_task_buf;
  xTaskCreateStaticPinnedToCore(pipecat_webrtc_init_task, "webrtc_init",
                                8192, NULL, 5, webrtc_stack,
                                &webrtc_task_buf, 0);
  ESP_LOGI(LOG_TAG, "WebRTC init task spawned");
}

/* ── Wi-Fi change trial (M1, 1.2.23) ─────────────────────────────────
 * Started at boot ONLY while a trial is pending (settings_wifi_trial_boot
 * decided this boot is an attempt). The trial passes on the first inbound
 * bot message over the data channel: cmd_note_activity() (cmd.cpp) raises
 * g_bot_reached, the same hook the OTA health gate uses. An HTTP 200 on the
 * offer POST is not enough — it only proves TCP to the bot, while a working
 * session also needs UDP, ICE and DTLS to get through the new network.
 * That caller runs on the data-channel path, on a stack we don't control, so it
 * only sets a flag; this task (internal-RAM stack, xTaskCreate) does the NVS
 * write. No bot message within WIFI_TRIAL_TIMEOUT_MS (wrong password, wrong
 * network, bot unreachable, UDP blocked) = reboot: the next boot counts the
 * next attempt, and after the last one the old network returns. */
#define WIFI_TRIAL_TIMEOUT_MS 90000
/* A wrong password shows up as an auth/handshake failure within seconds, so
 * waiting out the full timeout only delays the rollback. This many in a row
 * (no got-ip between them) ends the attempt early. NO_AP_FOUND isn't counted:
 * a far or rebooting AP produces it too. */
#define WIFI_TRIAL_AUTHFAIL_MAX 3
static volatile bool g_bot_reached = false;
static volatile int s_trial_authfails = 0;  /* written by the event handler only */

void pipecat_wifi_bot_reached(void) { g_bot_reached = true; }

static void wifi_trial_task(void *arg) {
  const TickType_t start = xTaskGetTickCount();
  while (!g_bot_reached) {
    if (s_trial_authfails >= WIFI_TRIAL_AUTHFAIL_MAX && !pipecat_ota_busy()) {
      ESP_LOGE(LOG_TAG, "wifi trial: the network refused the credentials %d times in a "
               "row — rebooting (next attempt)", WIFI_TRIAL_AUTHFAIL_MAX);
      vTaskDelay(pdMS_TO_TICKS(200));
      boot_diag_restart("wifi-trial-authfail");
    }
    if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(WIFI_TRIAL_TIMEOUT_MS)) {
      if (pipecat_ota_busy()) {           /* never reboot under a flash write */
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      ESP_LOGE(LOG_TAG, "wifi trial: bot not reached within %d s — rebooting (next attempt)",
               WIFI_TRIAL_TIMEOUT_MS / 1000);
      vTaskDelay(pdMS_TO_TICKS(200));
      boot_diag_restart("wifi-trial-timeout");
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (settings_wifi_trial_passed() != 0) {
    /* Should not happen (internal stack). If it does, the counter is still
     * set and a later reboot would roll back good creds — say so loudly. */
    ESP_LOGE(LOG_TAG, "wifi trial: bot reached but clearing the trial FAILED");
  }
  vTaskDelete(NULL);
}

void pipecat_wifi_trial_start(void) {
  if (!settings_wifi_trial_pending()) return;
  if (xTaskCreate(wifi_trial_task, "wifi_trial", 4096, NULL, 4, NULL) != pdPASS) {
    ESP_LOGE(LOG_TAG, "wifi trial: task spawn failed — rebooting to retry");
    vTaskDelay(pdMS_TO_TICKS(500));
    boot_diag_restart("wifi-trial-spawn");
  }
  ESP_LOGW(LOG_TAG, "wifi trial: waiting up to %d s for the bot", WIFI_TRIAL_TIMEOUT_MS / 1000);
}

/* Delayed reconnect runs from a one-shot esp_timer, not a sleep in the
 * event handler: the handler runs on the default event loop task, and
 * blocking there for up to 30 s stalls every other event handler (IP,
 * HTTP, OTA) for the whole AP outage. The callback runs on the esp_timer
 * task (small stack), so it only kicks the driver. */
static esp_timer_handle_t s_reconnect_timer = NULL;

/* Link history for get_info: how many times the station dropped since boot
 * and the driver's reason for the last one (wifi_err_reason_t, 0 = none
 * yet), so the dashboard can tell a wrong password from a weak signal.
 * Written only by the event handler; plain int reads elsewhere. */
static volatile int s_last_disc_reason = 0;
static volatile uint32_t s_disconnects = 0;

extern "C" void pipecat_wifi_link_stats(cmd_link_stats_t *out) {
  out->last_reason = s_last_disc_reason;
  out->disconnects = s_disconnects;
}

static bool wifi_reason_is_auth_fail(int reason) {
  return reason == WIFI_REASON_AUTH_FAIL ||
         reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_AUTH_EXPIRE;
}

static void wifi_reconnect_timer_cb(void *arg) {
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) ESP_LOGE(LOG_TAG, "delayed esp_wifi_connect failed: %d", err);
}

/* Reconnect after delay_ms. Any timer failure falls back to an immediate
 * connect so a retry is never lost. */
static void wifi_schedule_reconnect(int delay_ms) {
  if (delay_ms > 0 && s_reconnect_timer == NULL) {
    const esp_timer_create_args_t args = {
        .callback = &wifi_reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &s_reconnect_timer);
    if (err != ESP_OK) {
      ESP_LOGE(LOG_TAG, "reconnect timer create failed: %d", err);
      s_reconnect_timer = NULL;
    }
  }
  if (delay_ms > 0 && s_reconnect_timer != NULL) {
    esp_timer_stop(s_reconnect_timer);  // ESP_ERR_INVALID_STATE if idle: fine
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    if (err == ESP_OK) return;
    ESP_LOGE(LOG_TAG, "reconnect timer start failed: %d — connecting now", err);
  }
  esp_wifi_connect();
}

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  static int s_retry_num = 0;  // reset to 0 on IP_EVENT_STA_GOT_IP
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(LOG_TAG, "disconnected reason: %d", d->reason);
    s_last_disc_reason = d->reason;
    s_disconnects = s_disconnects + 1;
    if (settings_wifi_trial_pending()) {  /* the trial task acts on it */
      s_trial_authfails = wifi_reason_is_auth_fail(d->reason) ? s_trial_authfails + 1 : 0;
    }
    rgb_set_state(RGB_STATE_WIFI_CONNECTING);
    /* 023 fix (2026-09-29): the old 5-strike counter gave up for the
     * whole boot after a ~30s body-block — infinite retries with
     * backoff instead; only a got-ip resets the backoff. */
    /* Clamp the shift before it can overflow: 2^5 s already exceeds the
     * 30 s cap, and a larger shift is undefined behaviour that went
     * negative (zero delay, hot retry loop) after a long outage. */
    int exp = (s_retry_num < 5) ? -1 : s_retry_num - 5;
    if (exp > 5) exp = 5;
    int delay_ms = (exp < 0) ? 0 : (1 << exp) * 1000;
    if (delay_ms > 30000) delay_ms = 30000;  // cap at 30s
    wifi_schedule_reconnect(delay_ms);
    if (s_retry_num < INT_MAX) s_retry_num++;  // saturate: never wraps
    ESP_LOGI(LOG_TAG, "retrying connect (attempt %d)", s_retry_num);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    g_wifi_connected = true;
    s_retry_num = 0;  // reset retry counter on successful connection
    s_trial_authfails = 0;  // the network took the credentials
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);  // no stale retry
    rgb_set_state(RGB_STATE_WIFI_UP);
    // OTA check does NOT run here (empirical bisect 2026-09-19): the same
    // version GET that works at CONNECTED (build 47: 200 OK, update found)
    // crashes deterministically at got-ip time. It runs from the CONNECTED
    // callback in webrtc.cpp now.
    pipecat_start_webrtc();
  }
}

void pipecat_init_wifi() {
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &pipecat_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &pipecat_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  /* Hostname from NVS BEFORE connect — the DHCP DISCOVER carries it,
   * so the router (UniFi) shows the node name, not the default
   * "espressif". Rename-time setting is too late for the current
   * lease (measured 2026-09-29: rename acked, router never updated). */
  const char *hn = settings_hostname();
  if (hn && hn[0]) {
    esp_err_t herr = esp_netif_set_hostname(sta_netif, hn);
    ESP_LOGI(LOG_TAG, "hostname '%s' set on sta netif (rc=%d)", hn, herr);
  }
  assert(sta_netif);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(LOG_TAG, "Connecting to WiFi SSID: %s",
           settings_get_wifi_ssid());
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, settings_get_wifi_ssid(),
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, settings_get_wifi_pass(),
          sizeof(wifi_config.sta.password));
  /* Force WPA2-PSK — prevents WPA3 SAE path which fails on most APs
   * with ESP-IDF 5.x. PMF capable but not required covers WPA2/WPA3
   * transition mode networks too. */
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable    = true;
  wifi_config.sta.pmf_cfg.required   = false;

  ESP_ERROR_CHECK(esp_wifi_set_config(
      static_cast<wifi_interface_t>(ESP_IF_WIFI_STA), &wifi_config));
  rgb_set_state(RGB_STATE_WIFI_CONNECTING);
  ESP_ERROR_CHECK(esp_wifi_connect());

  // block until we get an IP address
  while (!g_wifi_connected) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
