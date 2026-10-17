#include "freertos/FreeRTOS.h"
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

static int rtvi_id = 0;
static QueueHandle_t rtvi_queue;
static PeerConnection *peer_connection = NULL;
static rtvi_callbacks_t *rtvi_callbacks = NULL;

typedef struct {
  cJSON *msg;
} rtvi_msg_t;

// Simple hashing function so we can fake pattern matching and switch on strings
// as a constexpr so it gets evaluated in compile time for static strings
static constexpr unsigned int hash(const char *s, int off = 0) {
  return !s[off] ? 5381 : (hash(s, off + 1) * 33) ^ s[off];
}

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
   * envelopes route to cmd.cpp; legacy RTVI "type" messages unchanged. */
  if (pipecat_cmd_handle(msg->msg)) {
    cmd_note_activity();  /* any bot traffic = session alive */
    return;
  }
  cJSON *j_type = cJSON_GetObjectItem(msg->msg, "type");
  if (j_type == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to find `type` field in RTVI message");
    return;
  }
  ESP_LOGD(LOG_TAG, "RTVI dispatch: type=%s", j_type->valuestring);

  switch (hash(j_type->valuestring)) {
    case hash("bot-started-speaking"):
      rtvi_callbacks->on_bot_started_speaking();
      break;
    case hash("bot-stopped-speaking"):
      rtvi_callbacks->on_bot_stopped_speaking();
      break;
    case hash("oww-awake"):
      rtvi_callbacks->on_oww_awake();
      break;
    case hash("oww-sleeping"):
      rtvi_callbacks->on_oww_sleeping();
      break;
    case hash("bot-tts-text"): {
      cJSON *j_data = cJSON_GetObjectItem(msg->msg, "data");
      cJSON *j_text = cJSON_GetObjectItem(j_data, "text");
      rtvi_callbacks->on_bot_tts_text(j_text->valuestring);
      break;
    }
    case hash("check-ota"): {
      /* Server-pushed OTA check (2026-09-19): a live flash while audio/WiFi
       * run from flash-cache crashes the chip (double exception — observed
       * 11 identical crashes). So the in-session trigger just REBOOTS; the
       * serialized boot-time check then downloads+flashes safely before
       * WebRTC/audio exist. Update latency = one reboot (~15s), which the
       * node does anyway on disconnects. */
      ESP_LOGI(LOG_TAG, "check-ota received — rebooting into boot-time check");
      esp_restart();
      break;
    }
    default:
      break;
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

  rtvi_queue = xQueueCreate(20, sizeof(rtvi_msg_t));
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
  if (rtvi_stack != NULL) {
    xTaskCreateStaticPinnedToCore(rtvi_task, "RTVI Task", 8192, NULL, 5,
                                 rtvi_stack, &rtvi_task_buffer, 0);
    ESP_LOGI(LOG_TAG, "RTVI task spawned (PSRAM stack)");
  } else {
    ESP_LOGE(LOG_TAG, "RTVI PSRAM stack alloc failed");
  }
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
   * proves the session is alive (liveness probe, design note). */
  cmd_note_activity();
}

void pipecat_rtvi_handle_message(const char *msg) {
  cJSON *j_msg = cJSON_Parse(msg);
  if (j_msg == NULL) {
    ESP_LOGE(LOG_TAG, "Error parsing RTVI message");
    return;
  }
  pipecat_rtvi_note_bot_traffic();

  rtvi_msg_t rtvi_msg = {.msg = j_msg};

  /* Non-blocking: the SCTP receive context must never wedge on a full queue.
   * Drop oldest if full (LED state is cosmetic; a dropped oww-awake just
   * means one missed color change, not a dead data channel). */
  if (xQueueSend(rtvi_queue, &rtvi_msg, 0) != pdTRUE) {
    rtvi_msg_t oldest;
    if (xQueueReceive(rtvi_queue, &oldest, 0) == pdTRUE) {
      cJSON_Delete(oldest.msg);
    }
    xQueueSend(rtvi_queue, &rtvi_msg, 0);
  }
}
