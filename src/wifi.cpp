#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "rgb.h"

static bool g_wifi_connected = false;
static bool g_webrtc_started = false;

// WebRTC bring-up on its own task — the sys_evt task's stack can't hold
// DTLS cert generation + ICE + SDP (stack overflow observed 2026-09-15).
static void pipecat_webrtc_init_task(void *user_data) {
  pipecat_init_webrtc();
  vTaskDelete(NULL);
}

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  static int s_retry_num = 0;  // reset to 0 on IP_EVENT_STA_GOT_IP
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(LOG_TAG, "disconnected reason: %d", d->reason);
    rgb_set_state(RGB_STATE_WIFI_CONNECTING);
    if (s_retry_num < 5) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(LOG_TAG, "retry to connect to the AP");
    }
    ESP_LOGI(LOG_TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    g_wifi_connected = true;
    s_retry_num = 0;  // reset retry counter on successful connection
    rgb_set_state(RGB_STATE_WIFI_UP);
    // WebRTC bring-up runs on its OWN task: it was overflowing the sys_evt
    // task's stack (DTLS cert gen + ICE + SDP from inside an event handler).
    if (!g_webrtc_started) {
      g_webrtc_started = true;
      static StackType_t webrtc_stack[8192];
      static StaticTask_t webrtc_task_buf;
      xTaskCreateStaticPinnedToCore(pipecat_webrtc_init_task, "webrtc_init",
                                    8192, NULL, 5, webrtc_stack,
                                    &webrtc_task_buf, 0);
      ESP_LOGI(LOG_TAG, "WebRTC init task spawned");
    }
  }
}

void pipecat_init_wifi() {
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &pipecat_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &pipecat_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
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

  ESP_LOGI(LOG_TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, (char *)WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, (char *)WIFI_PASSWORD,
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
