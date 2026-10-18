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

/* Small reply helper used across cmd.cpp. */
void cmd_send_simple(const char *cmd, bool ok, const char *note);

/* Connect-push: send the running firmware version to the bot, once per
 * data-channel open (1.1.2 — Nodes table FW column). */
void cmd_push_firmware_version(void);
void cmd_service_mute_push(void);   /* H4: send a pending button mute change */

#if LENOXVI_BOARD_WAVESHARE
/* Board-only commands (set_speaker), tried before "unknown cmd" via the
 * LENOXVI_CMD_BOARD_EXT hook. Returns true when it handled (and answered). */
bool cmd_board_handle(const char *cmd, const cJSON *msg);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PIPECAT_CMD_H */