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
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "wifi_trial.h"
#include "nvs_str.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "tknode"
#define KEY_NAME "node_name"
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"
#define KEY_MUTED "muted"
/* M1 (1.2.23): Wi-Fi change trial — see wifi_trial.h */
#define KEY_PREV_SSID "wifi_pssid"
#define KEY_PREV_PASS "wifi_ppass"
#define KEY_TRIAL "wifi_trial"

static char g_node_name[NODE_NAME_MAX] = "";
static char g_wifi_ssid[WIFI_SSID_MAX] = "";
static char g_wifi_pass[WIFI_PASS_MAX] = "";
static bool g_muted = false;
/* set_mute (nvs_write task) and the array's button (mute task) both save
 * the mute state; one at a time, so the check and the write can't
 * interleave. Created in settings_load, before either task exists. */
static StaticSemaphore_t g_muted_lock_buf;
static SemaphoreHandle_t g_muted_lock = NULL;
/* settings_hostname()'s result: the node name made DHCP/DNS-safe. */
static char g_hostname[NODE_NAME_MAX] = "";

#ifndef PIPECAT_ROOM
#define PIPECAT_ROOM "node"
#endif

/* NVS writes suspend the flash cache; a task whose STACK is in PSRAM then
 * double-faults the core (2026-09-29 set_name; again 2026-10-16 with the
 * first 1.2.11 set_mute). Every setter checks first and REFUSES with an
 * error instead of crashing the node — route writes through cmd.cpp's
 * nvs_run (internal-RAM stack). */
static bool stack_in_psram(const char *what) {
  int probe = 0;
  if (esp_ptr_external_ram(&probe)) {
    ESP_LOGE(TAG, "%s: refused — called on a PSRAM stack (would crash the "
             "node); route the write through nvs_run", what);
    return true;
  }
  return false;
}

/* Copy a value from NVS if the key exists and fits; leave dst
 * untouched otherwise. len from nvs_get_str includes the NUL — see
 * nvs_str.h (values that exactly filled their buffer used to be dropped). */
static void nvs_read_str(nvs_handle_t h, const char *key, char *dst,
                         size_t dst_size) {
  char tmp[WIFI_PASS_MAX > NODE_NAME_MAX ? WIFI_PASS_MAX : NODE_NAME_MAX];
  size_t len = sizeof(tmp);
  if (nvs_get_str(h, key, tmp, &len) == ESP_OK) {
    nvs_str_copy(dst, dst_size, tmp, len);
  }
}

static bool is_valid_name_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-';
}

int settings_set_node_name(const char *name) {
  if (stack_in_psram("set_node_name")) return -ESP_ERR_INVALID_STATE;
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
    const char *hn = settings_hostname();
    esp_netif_t *sta = NULL;
    while (hn[0] && (sta = esp_netif_next(sta)) != NULL) {
      esp_netif_set_hostname(sta, hn);
    }
    ESP_LOGI(TAG, "node name set: '%s' (hostname follows)", trimmed);
  }
  return err;
}

bool settings_load(void) {
  g_muted_lock = xSemaphoreCreateMutexStatic(&g_muted_lock_buf);
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs open failed: %d — using baked defaults", err);
  } else {
    nvs_read_str(h, KEY_NAME, g_node_name, sizeof(g_node_name));
    nvs_read_str(h, KEY_SSID, g_wifi_ssid, sizeof(g_wifi_ssid));
    nvs_read_str(h, KEY_PASS, g_wifi_pass, sizeof(g_wifi_pass));
    uint8_t m = 0;
    if (nvs_get_u8(h, KEY_MUTED, &m) == ESP_OK) g_muted = (m != 0);
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
  ESP_LOGI(TAG, "settings: name='%s' ssid set=%s pass set=%s muted=%s",
           g_node_name, g_wifi_ssid[0] ? "y" : "n", g_wifi_pass[0] ? "y" : "n",
           g_muted ? "y" : "n");
  return g_node_name[0] != '\0';
}

bool settings_get_muted(void) { return g_muted; }

int settings_set_muted(bool muted) {
  if (muted == g_muted) return 0;   /* no flash write for a no-op */
  if (stack_in_psram("set_muted")) return -ESP_ERR_INVALID_STATE;
  if (g_muted_lock) xSemaphoreTake(g_muted_lock, portMAX_DELAY);
  if (muted == g_muted) {           /* the other saver just stored it */
    if (g_muted_lock) xSemaphoreGive(g_muted_lock);
    return 0;
  }
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_u8(h, KEY_MUTED, muted ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  if (err == ESP_OK) {
    g_muted = muted;
    ESP_LOGI(TAG, "mute state stored: %s", muted ? "muted" : "unmuted");
  } else {
    ESP_LOGW(TAG, "mute state NOT stored: %s", esp_err_to_name(err));
  }
  if (g_muted_lock) xSemaphoreGive(g_muted_lock);
  return err == ESP_OK ? 0 : -err;
}

const char *settings_get_node_name(void) { return g_node_name; }
const char *settings_get_wifi_ssid(void) { return g_wifi_ssid; }
const char *settings_get_wifi_pass(void) { return g_wifi_pass; }

/* DHCP/DNS hostnames allow only a-z, 0-9 and '-' (RFC 1123), no leading or
 * trailing '-', at most 63 characters. Node names may hold '_' or capitals,
 * which some routers reject (the node then shows as "espressif" or not at
 * all), so the hostname is a cleaned copy; the node name itself is kept. */
const char *settings_hostname(void) {
  size_t n = 0;
  for (const char *p = g_node_name; *p && n < sizeof(g_hostname) - 1 && n < 63; p++) {
    char c = (char)tolower((unsigned char)*p);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '-';
    if (c == '-' && n == 0) continue;   /* no leading '-' */
    g_hostname[n++] = c;
  }
  while (n > 0 && g_hostname[n - 1] == '-') n--;   /* no trailing '-' */
  g_hostname[n] = '\0';
  return g_hostname;
}
/* ── Wi-Fi change trial (M1, 1.2.23) ────────────────────────────────── */
static bool g_wifi_trial_pending = false;
/* This boot restored the previous network after a failed trial (1.2.35):
 * fw_push tells the bot, so the dashboard can say so. RAM only, so it is
 * reported for this boot's sessions and gone after the next reboot. */
static bool g_wifi_rolled_back = false;

int settings_begin_wifi_trial(const char *ssid, const char *password) {
  if (stack_in_psram("begin_wifi_trial")) return -ESP_ERR_INVALID_STATE;
  if (!ssid || !ssid[0]) return -1;
  if (!wifi_pass_acceptable(password)) return -1;   /* 65+ chars, or 64 non-hex */
  /* Same rule as before: an empty password keeps the current one. */
  const char *new_pass = (password && password[0]) ? password : g_wifi_pass;
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  /* One commit: previous = current, current = new, trial = 1 (untested). */
  err = nvs_set_str(h, KEY_PREV_SSID, g_wifi_ssid);
  if (err == ESP_OK) err = nvs_set_str(h, KEY_PREV_PASS, g_wifi_pass);
  if (err == ESP_OK) err = nvs_set_str(h, KEY_SSID, ssid);
  if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, new_pass);
  if (err == ESP_OK) err = nvs_set_u8(h, KEY_TRIAL, 1);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    char p[WIFI_PASS_MAX];
    strlcpy(p, new_pass, sizeof(p));
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, p, sizeof(g_wifi_pass));
    ESP_LOGI(TAG, "wifi trial started: new creds stored, previous kept (reboot to try)");
  }
  return err;
}

int settings_wifi_trial_boot(void) {
  if (stack_in_psram("wifi_trial_boot")) return WIFI_TRIAL_NONE;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return WIFI_TRIAL_NONE;
  uint8_t stored = 0;
  nvs_get_u8(h, KEY_TRIAL, &stored);
  char prev_ssid[WIFI_SSID_MAX] = "", prev_pass[WIFI_PASS_MAX] = "";
  nvs_read_str(h, KEY_PREV_SSID, prev_ssid, sizeof(prev_ssid));
  nvs_read_str(h, KEY_PREV_PASS, prev_pass, sizeof(prev_pass));
  uint8_t next = 0;
  wifi_trial_action_t act = wifi_trial_boot_decision(stored, prev_ssid[0] != '\0', &next);
  esp_err_t err = ESP_OK;
  switch (act) {
    case WIFI_TRIAL_NONE:
      break;
    case WIFI_TRIAL_ATTEMPT:
      err = nvs_set_u8(h, KEY_TRIAL, next);   /* count this boot BEFORE trying */
      if (err == ESP_OK) err = nvs_commit(h);
      g_wifi_trial_pending = true;
      ESP_LOGW(TAG, "wifi trial: attempt %u of %u with the new creds",
               (unsigned)(next - 1), (unsigned)WIFI_TRIAL_MAX_BOOTS);
      break;
    case WIFI_TRIAL_ROLLBACK:
      err = nvs_set_str(h, KEY_SSID, prev_ssid);
      if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, prev_pass);
      if (err == ESP_OK) err = nvs_set_u8(h, KEY_TRIAL, 0);
      nvs_erase_key(h, KEY_PREV_SSID);
      nvs_erase_key(h, KEY_PREV_PASS);
      if (err == ESP_OK) err = nvs_commit(h);
      if (err == ESP_OK) {
        strlcpy(g_wifi_ssid, prev_ssid, sizeof(g_wifi_ssid));
        strlcpy(g_wifi_pass, prev_pass, sizeof(g_wifi_pass));
        g_wifi_rolled_back = true;
      }
      ESP_LOGE(TAG, "wifi trial FAILED %u boots — rolled back to the previous network (rc=%d)",
               (unsigned)WIFI_TRIAL_MAX_BOOTS, err);
      break;
    case WIFI_TRIAL_GIVE_UP:
      err = nvs_set_u8(h, KEY_TRIAL, 0);
      if (err == ESP_OK) err = nvs_commit(h);
      ESP_LOGE(TAG, "wifi trial failed and no previous creds saved — keeping current");
      break;
  }
  nvs_close(h);
  return act;
}

bool settings_wifi_trial_pending(void) { return g_wifi_trial_pending; }
bool settings_wifi_rolled_back(void) { return g_wifi_rolled_back; }

int settings_wifi_trial_passed(void) {
  if (stack_in_psram("wifi_trial_passed")) return -ESP_ERR_INVALID_STATE;
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_u8(h, KEY_TRIAL, 0);
  nvs_erase_key(h, KEY_PREV_SSID);
  nvs_erase_key(h, KEY_PREV_PASS);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    g_wifi_trial_pending = false;
    ESP_LOGI(TAG, "wifi trial PASSED: the bot answered on the new network");
  }
  return err;
}
