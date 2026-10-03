#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
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

/* 1.1.1 (2026-10-03): push CURRENT settings (NVS is the truth) into the
 * WiFi driver's runtime config. Without this, set_wifi's NVS write is
 * invisible to esp_wifi_connect() — the driver keeps the boot-time
 * config, re-associates with the OLD password, and the verify step
 * reports "connected" without ever testing the new creds (measured live
 * 2026-10-03 12:52: wrong password acked connected in 1.2s, zero
 * failures, rollback unreachable). Call BEFORE any reconnect that
 * should use fresh creds — set_wifi apply and the rollback restore. */
void pipecat_wifi_apply_runtime_config(void) {
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, settings_get_wifi_ssid(),
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, settings_get_wifi_pass(),
          sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable    = true;
  wifi_config.sta.pmf_cfg.required   = false;
  esp_err_t err = esp_wifi_set_config(
      static_cast<wifi_interface_t>(ESP_IF_WIFI_STA), &wifi_config);
  ESP_LOGW(LOG_TAG, "runtime wifi config push (ssid set, rc=%d)", err);
}

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  static int s_retry_num = 0;  // reset to 0 on IP_EVENT_STA_GOT_IP
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(LOG_TAG, "disconnected reason: %d", d->reason);
    rgb_set_state(RGB_STATE_WIFI_CONNECTING);
    /* 023 fix (2026-09-29): the old 5-strike counter gave up for the
     * whole boot after a ~30s body-block — infinite retries with
     * backoff instead; only a got-ip resets the backoff. */
    int delay_ms = (s_retry_num < 5) ? 0 : (1 << (s_retry_num - 5)) * 1000;
    if (delay_ms > 30000) delay_ms = 30000;  // cap at 30s
    if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_wifi_connect();
    s_retry_num++;
    ESP_LOGI(LOG_TAG, "retrying connect (attempt %d)", s_retry_num);
    cmd_wifi_on_disconnected();  // set_wifi verify/rollback hook
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    g_wifi_connected = true;
    s_retry_num = 0;  // reset retry counter on successful connection
    rgb_set_state(RGB_STATE_WIFI_UP);
    cmd_wifi_on_connected();  // set_wifi verify/rollback hook
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

  /* Quick scan — shows every visible SSID so we can confirm the radio works */
  wifi_scan_config_t scan_cfg = {};
  esp_wifi_scan_start(&scan_cfg, true);   /* blocking */
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  ESP_LOGI(LOG_TAG, "WiFi scan: %d APs found", ap_count);
  if (ap_count > 0) {
    wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_count, aps);
    for (int i = 0; i < ap_count; i++) {
      ESP_LOGI(LOG_TAG, "  [%d] SSID: '%.32s'  RSSI: %d  Ch: %d",
               i, aps[i].ssid, aps[i].rssi, aps[i].primary);
    }
    free(aps);
  }

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
