/* Wi-Fi change trial — the boot-time decision (M1, 1.2.23).
 *
 * A dashboard Wi-Fi change is a TRIAL that survives reboots: set_wifi saves
 * the current creds as "previous", stores the new ones and sets trial = 1,
 * then reboots. Every boot during a trial counts one attempt BEFORE trying
 * (so a crash mid-attempt still counts); an attempt passes only when the bot
 * answered the offer POST (HTTP 200) — "got an IP" is not enough (a guest
 * network or VLAN gives an IP but never reaches the bot). After
 * WIFI_TRIAL_MAX_BOOTS failed boots the previous creds come back.
 *
 * Pure C, no ESP-IDF headers: tests/test_wifi_trial.c runs it on the host. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_TRIAL_MAX_BOOTS 3

typedef enum {
  WIFI_TRIAL_NONE = 0,      /* no trial: boot normally */
  WIFI_TRIAL_ATTEMPT,       /* try the new creds; *next_trial is the value to store first */
  WIFI_TRIAL_ROLLBACK,      /* attempts exhausted: restore previous creds, clear the trial */
  WIFI_TRIAL_GIVE_UP,       /* attempts exhausted but no previous creds saved: keep current, clear */
} wifi_trial_action_t;

/* stored_trial: the NVS counter (0 = none, 1 = untested, n = n-1 boots
 * already tried). have_previous: a non-empty previous SSID is saved.
 * Writes the counter to store before this boot's attempt into *next_trial. */
wifi_trial_action_t wifi_trial_boot_decision(uint8_t stored_trial, bool have_previous,
                                             uint8_t *next_trial);

/* Wi-Fi password shape (1.2.35). Up to 63 characters is a WPA passphrase;
 * exactly 64 is a raw PSK and must be hex (the Wi-Fi driver reads 64 chars
 * as hex); anything longer is refused, never silently cut short. NULL or
 * empty is accepted: set_wifi keeps the current password then. */
bool wifi_pass_acceptable(const char *pass);

#ifdef __cplusplus
}
#endif
