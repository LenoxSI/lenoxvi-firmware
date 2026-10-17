/* Host test for src/wifi_trial.c (M1): the boot-time decision of a Wi-Fi
 * change trial, plus a simulation of whole trial lifecycles (pass on boot N,
 * fail every boot, power blips) against an in-memory "NVS".
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc src/wifi_trial.c tests/test_wifi_trial.c */
#include <stdio.h>
#include <string.h>

#include "wifi_trial.h"

static int fails = 0;
#define CHECK(name, cond) do { if (cond) printf("PASS %s\n", name); \
  else { printf("FAIL %s\n", name); fails++; } } while (0)

/* In-memory model of the NVS keys the firmware uses. */
typedef struct { char ssid[33], prev[33]; unsigned char trial; } nvs_t;

static void begin(nvs_t *n, const char *new_ssid) {       /* set_wifi */
  strcpy(n->prev, n->ssid); strcpy(n->ssid, new_ssid); n->trial = 1;
}
/* One boot: returns the action; reaches_bot = the offer POST got 200. */
static wifi_trial_action_t boot(nvs_t *n, int reaches_bot) {
  unsigned char next;
  wifi_trial_action_t a = wifi_trial_boot_decision(n->trial, n->prev[0] != 0, &next);
  n->trial = next;
  if (a == WIFI_TRIAL_ROLLBACK) { strcpy(n->ssid, n->prev); n->prev[0] = 0; }
  if (a == WIFI_TRIAL_ATTEMPT && reaches_bot) { n->trial = 0; n->prev[0] = 0; }  /* trial_passed */
  return a;
}

int main(void) {
  unsigned char next = 99;
  /* pure decisions */
  CHECK("no trial -> NONE, counter stays 0",
        wifi_trial_boot_decision(0, true, &next) == WIFI_TRIAL_NONE && next == 0);
  CHECK("untested (1) -> ATTEMPT, store 2 before trying",
        wifi_trial_boot_decision(1, true, &next) == WIFI_TRIAL_ATTEMPT && next == 2);
  CHECK("3rd attempt (3) -> ATTEMPT, store 4",
        wifi_trial_boot_decision(3, true, &next) == WIFI_TRIAL_ATTEMPT && next == 4);
  CHECK("exhausted (4) with previous -> ROLLBACK, clear",
        wifi_trial_boot_decision(4, true, &next) == WIFI_TRIAL_ROLLBACK && next == 0);
  CHECK("exhausted (4) without previous -> GIVE_UP, clear",
        wifi_trial_boot_decision(4, false, &next) == WIFI_TRIAL_GIVE_UP && next == 0);
  CHECK("corrupt counter (255) -> ROLLBACK, clear (never loops)",
        wifi_trial_boot_decision(255, true, &next) == WIFI_TRIAL_ROLLBACK && next == 0);

  /* lifecycle: good network, passes on the first boot */
  nvs_t n = {"HomeNet", "", 0};
  begin(&n, "NewNet");
  CHECK("good network: boot 1 attempts", boot(&n, 1) == WIFI_TRIAL_ATTEMPT);
  CHECK("good network: trial cleared, new SSID kept, previous erased",
        n.trial == 0 && strcmp(n.ssid, "NewNet") == 0 && n.prev[0] == 0);
  CHECK("good network: next boot is normal", boot(&n, 1) == WIFI_TRIAL_NONE && strcmp(n.ssid, "NewNet") == 0);

  /* lifecycle: bad network (wrong password / guest VLAN) every boot */
  nvs_t b = {"HomeNet", "", 0};
  begin(&b, "GuestNet");
  int attempts = 0;
  wifi_trial_action_t a;
  while ((a = boot(&b, 0)) == WIFI_TRIAL_ATTEMPT) attempts++;
  CHECK("bad network: exactly WIFI_TRIAL_MAX_BOOTS attempts", attempts == WIFI_TRIAL_MAX_BOOTS);
  CHECK("bad network: then ROLLBACK to the previous SSID",
        a == WIFI_TRIAL_ROLLBACK && strcmp(b.ssid, "HomeNet") == 0 && b.trial == 0);
  CHECK("bad network: boot after rollback is normal on the old network",
        boot(&b, 1) == WIFI_TRIAL_NONE && strcmp(b.ssid, "HomeNet") == 0);

  /* lifecycle: fails twice (power blips / slow router), passes on attempt 3 */
  nvs_t s = {"HomeNet", "", 0};
  begin(&s, "NewNet");
  boot(&s, 0); boot(&s, 0);
  CHECK("late success: 3rd attempt still allowed", boot(&s, 1) == WIFI_TRIAL_ATTEMPT);
  CHECK("late success: new network kept", s.trial == 0 && strcmp(s.ssid, "NewNet") == 0);

  /* a second change during a trial: previous = the untested creds (documented) */
  nvs_t d = {"HomeNet", "", 0};
  begin(&d, "TypoNet");
  begin(&d, "NewNet");
  CHECK("re-change mid-trial restarts the count at 1", d.trial == 1);

  printf("\n%s\n", fails ? "FAILED" : "ALL PASSED");
  return fails ? 1 : 0;
}
