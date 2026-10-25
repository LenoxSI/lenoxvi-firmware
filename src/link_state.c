/* Link state for the USB `info` reply — see link_state.h (1.2.47). */
#include "link_state.h"

static int s_state = LINK_BOOT;

void link_state_set(link_state_t s) { __atomic_store_n(&s_state, (int)s, __ATOMIC_RELAXED); }

link_state_t link_state_get(void) { return (link_state_t)__atomic_load_n(&s_state, __ATOMIC_RELAXED); }

const char *link_state_name(link_state_t s) {
  switch (s) {
    case LINK_WIFI_CONNECTING: return "wifi_connecting";
    case LINK_WIFI_UP: return "wifi_up";
    case LINK_BOT: return "bot";
    case LINK_BOOT:
    default: return "boot";
  }
}
