#include "freertos/FreeRTOS.h"
#include "boot_diag.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <esp_log.h>
#include <peer.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "rgb.h"
#include "cmd.h"

#define MAX_TYPE_LEN 32
#define MAX_ID_LEN 64
#define RTVI_QUEUE_LEN 20

static int rtvi_id = 0;
static QueueHandle_t rtvi_queue;
static PeerConnection *peer_connection = NULL;
static rtvi_callbacks_t *rtvi_callbacks = NULL;

/* "bot-thinking" (rtvi_callbacks.cpp). Declared here, not in the
 * rtvi_callbacks_t struct, so main.h stays as it is. */
extern void pipecat_rtvi_on_bot_thinking(void);

typedef struct {
  cJSON *msg;
} rtvi_msg_t;

static rtvi_msg_t *create_rtvi_message(const char *type) {
  cJSON *j_msg = cJSON_CreateObject();

  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "label", "rtvi-ai") == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }
  if (cJSON_AddStringToObject(j_msg, "type", type) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  char id[MAX_ID_LEN];
  sprintf(id, "%d", rtvi_id++);
  if (cJSON_AddStringToObject(j_msg, "id", id) == NULL) {
    cJSON_Delete(j_msg);
    ESP_LOGE(LOG_TAG, "Unable to create RTVI message");
    return NULL;
  }

  rtvi_msg_t *msg = (rtvi_msg_t *)malloc(sizeof(rtvi_msg_t));
  msg->msg = j_msg;

  return msg;
}

static void destroy_rtvi_message(rtvi_msg_t *msg) {
  cJSON_Delete(msg->msg);
  free(msg);
}

static char *rtvi_message_to_string(rtvi_msg_t *msg) {
  if (msg == NULL || msg->msg == NULL) {
    return NULL;
  }

  char *msg_str = cJSON_Print(msg->msg);

  return msg_str;
}

static void rtvi_handle_message(const rtvi_msg_t *msg) {
  /* Dashboard command schema first (firmware 1.1.0): {"cmd": ...}
   * envelopes route to cmd.cpp; legacy RTVI "type" messages unchanged.
   * Bot activity was already noted when the message arrived
   * (pipecat_rtvi_handle_message), before anything is dispatched. */
  if (pipecat_cmd_handle(msg->msg)) {
    return;
  }
  /* Every field is checked before use: one malformed message (a non-string
   * type, a bot-tts-text without text) must not crash the node. Plain
   * strcmp, so an unknown type can never alias a handled one. */
  const cJSON *j_type = cJSON_GetObjectItem(msg->msg, "type");
  if (!cJSON_IsString(j_type)) {
    ESP_LOGE(LOG_TAG, "Unable to find `type` field in RTVI message");
    return;
  }
  const char *type = j_type->valuestring;
  ESP_LOGD(LOG_TAG, "RTVI dispatch: type=%s", type);

  if (strcmp(type, "bot-started-speaking") == 0) {
    rtvi_callbacks->on_bot_started_speaking();
  } else if (strcmp(type, "bot-stopped-speaking") == 0) {
    rtvi_callbacks->on_bot_stopped_speaking();
  } else if (strcmp(type, "bot-thinking") == 0) {
    pipecat_rtvi_on_bot_thinking();
  } else if (strcmp(type, "oww-awake") == 0) {
    rtvi_callbacks->on_oww_awake();
  } else if (strcmp(type, "oww-sleeping") == 0) {
    rtvi_callbacks->on_oww_sleeping();
  } else if (strcmp(type, "bot-tts-text") == 0) {
    const cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
    const cJSON *j_text = cJSON_GetObjectItem(j_data, "text");
    if (cJSON_IsString(j_text)) {
      rtvi_callbacks->on_bot_tts_text(j_text->valuestring);
    }
  } else if (strcmp(type, "check-ota") == 0) {
    /* Legacy: the bot no longer sends this. Rebooting starts a new session,
     * and the OTA check runs again about a minute after it connects.
     * Ignored while an update is downloading/flashing (a reboot would throw
     * it away). */
    if (pipecat_ota_busy()) {
      ESP_LOGW(LOG_TAG, "check-ota received — OTA already in flight, ignoring");
      return;
    }
    ESP_LOGI(LOG_TAG, "check-ota received — rebooting into a fresh OTA check");
    boot_diag_restart("check-ota");
  }
}

static void rtvi_task(void *pvParameter) {
  rtvi_msg_t msg;

  while (1) {
    if (xQueueReceive(rtvi_queue, &msg, portMAX_DELAY)) {
      rtvi_handle_message(&msg);
      cJSON_Delete(msg.msg);
    }
  }
}

void pipecat_init_rtvi(PeerConnection *connection,
                       rtvi_callbacks_t *callbacks) {
  peer_connection = connection;
  rtvi_callbacks = callbacks;

  rtvi_queue = xQueueCreate(RTVI_QUEUE_LEN, sizeof(rtvi_msg_t));
  /* Without the queue or the task the node would stay connected (bot
   * traffic still counts as liveness) yet never answer a command. Reboot
   * loudly instead. */
  if (rtvi_queue == NULL) {
    ESP_LOGE(LOG_TAG, "RTVI queue alloc failed — rebooting");
    boot_diag_restart("rtvi-init");
  }
  /* RTVI task stack: bot-output/bot-tts-text messages carry full sentence
   * payloads; cJSON parse + data walks overflowed a 4096 stack (2026-09-19).
   * But an 8192-word internal-RAM stack makes xTaskCreatePinnedToCore
   * SILENTLY FAIL at this point in boot (heap nearly exhausted after
   * WiFi+WebRTC+audio DMA; observed: 29 DC messages delivered, ZERO
   * dispatches — no task existed to consume them). Same fix as the audio
   * publisher task: static allocation with the stack in PSRAM. */
  static StaticTask_t rtvi_task_buffer;
  static StackType_t *rtvi_stack = NULL;
  if (rtvi_stack == NULL) {  /* survive re-init after watchdog reboot */
    rtvi_stack = (StackType_t *)heap_caps_malloc(8192 * sizeof(StackType_t),
                                                 MALLOC_CAP_SPIRAM);
  }
  if (rtvi_stack == NULL) {
    ESP_LOGE(LOG_TAG, "RTVI PSRAM stack alloc failed — rebooting");
    boot_diag_restart("rtvi-init");
  }
  if (xTaskCreateStaticPinnedToCore(rtvi_task, "RTVI Task", 8192, NULL, 5,
                                    rtvi_stack, &rtvi_task_buffer, 0) == NULL) {
    ESP_LOGE(LOG_TAG, "RTVI task create failed — rebooting");
    boot_diag_restart("rtvi-init");
  }
  ESP_LOGI(LOG_TAG, "RTVI task spawned (PSRAM stack)");
}

void pipecat_rtvi_send_client_ready() {
  rtvi_msg_t *msg = create_rtvi_message("client-ready");

  char *msg_str = rtvi_message_to_string(msg);

  peer_connection_datachannel_send(peer_connection, msg_str, strlen(msg_str));

  cJSON_free(msg_str);

  destroy_rtvi_message(msg);
}

void pipecat_rtvi_send_raw(const char *json) {
  /* Raw JSON send for command-schema replies (cmd.cpp) — no label/id
   * scaffolding; the caller owns the full envelope. libpeer's send
   * takes char* (non-const); cast is safe (it does not modify). */
  if (peer_connection == NULL) return;
  char *buf = strdup(json);
  if (buf == NULL) return;
  peer_connection_datachannel_send(peer_connection, buf, strlen(buf));
  free(buf);
}

void pipecat_rtvi_note_bot_traffic(void) {
  /* Called for legacy RTVI messages too — anything from the bot
   * proves the session is alive (liveness probe). */
  cmd_note_activity();
}

static bool is_cmd_message(const cJSON *msg) {
  return cJSON_IsString(cJSON_GetObjectItem(msg, "cmd"));
}

void pipecat_rtvi_handle_message(const char *msg) {
  cJSON *j_msg = cJSON_Parse(msg);
  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Error parsing RTVI message");
    return;
  }
  /* Noted here, on arrival, so the OTA health gate has seen the bot before
   * any command (a restart, say) is dispatched. */
  pipecat_rtvi_note_bot_traffic();
  if (rtvi_queue == NULL) {  /* RTVI not set up yet: nobody to hand it to */
    cJSON_Delete(j_msg);
    return;
  }

  rtvi_msg_t rtvi_msg = {.msg = j_msg};

  /* Non-blocking: the SCTP receive context must never wedge on a full queue. */
  if (xQueueSend(rtvi_queue, &rtvi_msg, 0) == pdTRUE) return;

  /* Full. Drop the oldest message that is NOT a command: LED state is
   * cosmetic (a dropped oww-awake is one missed color change), but a dropped
   * command is never answered and the bot reports a dead session. Take
   * everything out, drop one, put the rest back in order. Only this
   * receive path sends to the queue, so the space freed is still there to
   * refill. If every queued message is a command, the new message is the
   * one dropped. */
  rtvi_msg_t held[RTVI_QUEUE_LEN];
  int n = 0;
  while (n < RTVI_QUEUE_LEN && xQueueReceive(rtvi_queue, &held[n], 0) == pdTRUE) n++;
  int victim = -1;
  for (int i = 0; i < n; i++) {
    if (!is_cmd_message(held[i].msg)) { victim = i; break; }
  }
  for (int i = 0; i < n; i++) {
    if (i == victim) {
      cJSON_Delete(held[i].msg);
    } else {
      xQueueSend(rtvi_queue, &held[i], 0);
    }
  }
  if ((victim >= 0 || n < RTVI_QUEUE_LEN) && xQueueSend(rtvi_queue, &rtvi_msg, 0) == pdTRUE) {
    return;
  }
  if (is_cmd_message(j_msg)) {
    ESP_LOGW(LOG_TAG, "RTVI queue full of commands — new command dropped");
  }
  cJSON_Delete(j_msg);
}
