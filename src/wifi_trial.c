/* Wi-Fi change trial — boot decision (see wifi_trial.h). */
#include "wifi_trial.h"

wifi_trial_action_t wifi_trial_boot_decision(uint8_t stored_trial, bool have_previous,
                                             uint8_t *next_trial) {
  if (stored_trial == 0) {
    *next_trial = 0;
    return WIFI_TRIAL_NONE;
  }
  if (stored_trial <= WIFI_TRIAL_MAX_BOOTS) {
    *next_trial = (uint8_t)(stored_trial + 1);   /* count THIS boot before trying */
    return WIFI_TRIAL_ATTEMPT;
  }
  *next_trial = 0;                               /* exhausted (or a corrupt huge value) */
  return have_previous ? WIFI_TRIAL_ROLLBACK : WIFI_TRIAL_GIVE_UP;
}
