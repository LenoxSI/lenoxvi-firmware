#ifndef PIPECAT_CMD_H
#define PIPECAT_CMD_H

#include <cJSON.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Try to handle a data-channel message as a dashboard command.
 * Returns true when the message had a "cmd" field (consumed). */
bool pipecat_cmd_handle(const cJSON *msg);

/* Start the 5-min ping liveness task (call once, post-CONNECTED). */
void cmd_liveness_start(void);

/* Mark session-alive (pong or any bot traffic seen). */
void cmd_note_activity(void);

/* Verify/rollback hooks called from the WiFi event handler. */
void cmd_wifi_on_connected(void);
void cmd_wifi_on_disconnected(void);

/* Small reply helper used across cmd.cpp. */
void cmd_send_simple(const char *cmd, bool ok, const char *note);

/* Connect-push: send the running firmware version to the bot, once per
 * data-channel open (1.1.2 — Nodes table FW column). */
void cmd_push_firmware_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PIPECAT_CMD_H */