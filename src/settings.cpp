/* settings.cpp — node identity + Wi-Fi creds in NVS (firmware 1.1.0).
 *
 * Compile-time bakes (WIFI_SSID/WIFI_PASSWORD/PIPECAT_ROOM) become
 * FALLBACKS: NVS wins when set, else the baked value, else factory
 * defaults. All writes go through the dashboard command channel
 * (cmd.cpp); nothing here talks to the network. The NVS namespace is
 * separate from OTA's otadata so partition A/B swaps never lose the
 * node's name (a renamed node must stay renamed across OTA).
 */

#include "settings.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <nvs.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "settings";

#define NVS_NAMESPACE "tknode"
#define KEY_NAME "node_name"
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"

static char g_node_name[NODE_NAME_MAX] = "";
static char g_wifi_ssid[WIFI_SSID_MAX] = "";
static char g_wifi_pass[WIFI_PASS_MAX] = "";

#ifndef PIPECAT_ROOM
#define PIPECAT_ROOM "node"
#endif

/* Copy a value from NVS if the key exists and fits; leave dst
 * untouched otherwise. */
static void nvs_read_str(nvs_handle_t h, const char *key, char *dst,
                         size_t dst_size) {
  char tmp[WIFI_PASS_MAX > NODE_NAME_MAX ? WIFI_PASS_MAX : NODE_NAME_MAX];
  size_t len = sizeof(tmp);
  if (nvs_get_str(h, key, tmp, &len) == ESP_OK && len > 0 &&
      len < dst_size) {
    memcpy(dst, tmp, len);
    dst[len] = '\0';
  }
}

static bool is_valid_name_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-';
}

int settings_set_node_name(const char *name) {
  if (!name) return -1;
  char trimmed[NODE_NAME_MAX];
  size_t n = 0;
  for (const char *p = name; *p && n < NODE_NAME_MAX - 1; p++) {
    if (is_valid_name_char(*p)) trimmed[n++] = *p;
  }
  trimmed[n] = '\0';
  if (n == 0) {
    ESP_LOGE(TAG, "set_node_name: empty after trimming");
    return -1;
  }
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_str(h, KEY_NAME, trimmed);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    strlcpy(g_node_name, trimmed, sizeof(g_node_name));
    /* Hostname follows the name — UniFi shows the real node name. */
    esp_netif_t *sta = NULL;
    while ((sta = esp_netif_next(sta)) != NULL) {
      esp_netif_set_hostname(sta, settings_hostname());
    }
    ESP_LOGI(TAG, "node name set: '%s' (hostname follows)", trimmed);
  }
  return err;
}

int settings_set_wifi(const char *ssid, const char *password) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  if (ssid && ssid[0]) {
    err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK && password && password[0]) {
      err = nvs_set_str(h, KEY_PASS, password);
    }
  }
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    if (ssid && ssid[0]) {
      strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
      if (password && password[0]) {
        strlcpy(g_wifi_pass, password, sizeof(g_wifi_pass));
      }
    }
    ESP_LOGI(TAG, "wifi creds stored (apply: reconnect or reboot)");
  }
  return err;
}

bool settings_load(void) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs open failed: %d — using baked defaults", err);
  } else {
    nvs_read_str(h, KEY_NAME, g_node_name, sizeof(g_node_name));
    nvs_read_str(h, KEY_SSID, g_wifi_ssid, sizeof(g_wifi_ssid));
    nvs_read_str(h, KEY_PASS, g_wifi_pass, sizeof(g_wifi_pass));
    nvs_close(h);
  }

  /* Fallback chain: NVS > compile-time bake > factory. */
  if (g_node_name[0] == '\0') {
    strlcpy(g_node_name, PIPECAT_ROOM, sizeof(g_node_name));
  }
#ifdef WIFI_SSID
  if (g_wifi_ssid[0] == '\0') strlcpy(g_wifi_ssid, WIFI_SSID, sizeof(g_wifi_ssid));
  if (g_wifi_pass[0] == '\0') strlcpy(g_wifi_pass, WIFI_PASSWORD, sizeof(g_wifi_pass));
#endif
  ESP_LOGI(TAG, "settings: name='%s' ssid set=%s pass set=%s",
           g_node_name, g_wifi_ssid[0] ? "y" : "n", g_wifi_pass[0] ? "y" : "n");
  return g_node_name[0] != '\0';
}

const char *settings_get_node_name(void) { return g_node_name; }
const char *settings_get_wifi_ssid(void) { return g_wifi_ssid; }
const char *settings_get_wifi_pass(void) { return g_wifi_pass; }

const char *settings_hostname(void) { return g_node_name; }