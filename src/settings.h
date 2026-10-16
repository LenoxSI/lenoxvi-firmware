#ifndef PIPECAT_SETTINGS_H
#define PIPECAT_SETTINGS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Node settings stored in NVS — runtime-writable (dashboard command
 * channel), compile-time values become fallbacks only (design note:
 * the v1.5-portal autopsy keeps the NVS layer; firmware 1.1.0). */

#define NODE_NAME_MAX 32
#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 64

/* Load the node's settings at boot. Safe before nvs_flash_init() —
 * app_main initializes NVS first, then calls this. Returns true when a
 * stored node_name exists. */
bool settings_load(void);

/* Current effective values (set by settings_load). */
const char *settings_get_node_name(void);
const char *settings_get_wifi_ssid(void);
const char *settings_get_wifi_pass(void);

/* Persist new values. Empty/NULL ssid or pass keeps the current one.
 * Returns 0 on success, negative esp_err_t codes on NVS failure. */
int settings_set_node_name(const char *name);   /* trimmed, [A-Za-z0-9_-]{1,31} */
int settings_set_wifi(const char *ssid, const char *password);

/* Mic mute (H4, 2026-10-16): persisted so a muted node stays muted across
 * reboots and OTA. Unset = unmuted. */
bool settings_get_muted(void);
int settings_set_muted(bool muted);

/* ESP32 station hostname derived from node_name (lowercase, RFC-valid
 * for DHCP): "living_room" -> "living_room".local on mDNS later; used
 * for esp_netif_set_hostname() so the router AP list shows the name. */
const char *settings_hostname(void);

#ifdef __cplusplus
}
#endif

#endif /* PIPECAT_SETTINGS_H */