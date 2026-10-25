#ifndef LENOXVI_LINK_STATE_H
#define LENOXVI_LINK_STATE_H
/* Where the node is on its way to the bot, for the USB `info` reply:
 * boot -> wifi_connecting -> wifi_up (has an IP, bot not reached) -> bot.
 * Set next to the matching LED state changes; read from any task. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LINK_BOOT = 0, LINK_WIFI_CONNECTING, LINK_WIFI_UP, LINK_BOT } link_state_t;

void link_state_set(link_state_t s);          /* any task; atomic */
link_state_t link_state_get(void);
const char *link_state_name(link_state_t s);  /* "boot" "wifi_connecting" "wifi_up" "bot" */

#ifdef __cplusplus
}
#endif
#endif /* LENOXVI_LINK_STATE_H */
