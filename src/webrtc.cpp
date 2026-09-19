#ifndef LINUX_BUILD
#include <driver/i2s_std.h>
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

#include "main.h"
#include "rgb.h"

static PeerConnection *peer_connection = NULL;

#ifndef LINUX_BUILD
StaticTask_t task_buffer;
void pipecat_send_audio_task(void *user_data) {
  pipecat_init_audio_encoder();

  while (1) {
    pipecat_send_audio(peer_connection);
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif

static void pipecat_ondatachannel_onmessage_task(char *msg, size_t len,
                                                 void *userdata, uint16_t sid) {
#ifdef LOG_DATACHANNEL_MESSAGES
  ESP_LOGI(LOG_TAG, "DataChannel Message: %s", msg);
#endif
  pipecat_rtvi_handle_message(msg);
}

/* SCTP open watchdog (2026-09-19): the data channel is a coin flip at
 * session setup — observed live: some sessions the SCTP association never
 * finishes opening (bot logs "Data channel not established within 10s",
 * queues our LED state messages, then discards them; node never logs
 * "DataChannel created"). The mic/audio path is UNAFFECTED (RTP works),
 * so the only casualty is LED state + any RTVI messages. A reboot re-rolls
 * the handshake. 20s gives a slow-but-legit handshake room to land. */
static bool dc_opened = false;
static TickType_t connected_at = 0;

/* DCEP must NOT be fired from the SCTP onopen callback: libpeer's raw SCTP
 * path sends the DCEP exactly once with NO retransmit, and the remote
 * (aiortc) side of the association isn't always ready to accept it the
 * instant COOKIE_ACK lands — a lost DCEP means the server NEVER learns the
 * channel exists ("Data channel not established within 10s... discarding",
 * observed live 2026-09-19; same family as livekit/client-sdk-esp32#63).
 * Create it from a detached task 750ms after the association settles. */
static void dc_create_delayed_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(750));
  if (peer_connection_create_datachannel(peer_connection, DATA_CHANNEL_RELIABLE,
                                         0, 0, (char *)"rtvi-ai",
                                         (char *)"") != -1) {
    ESP_LOGI(LOG_TAG, "DataChannel created (delayed DCEP)");
    dc_opened = true;  // for the watchdog below
  } else {
    ESP_LOGE(LOG_TAG, "Failed to create DataChannel");
  }
  vTaskDelete(NULL);
}

static void pipecat_ondatachannel_onopen_task(void *userdata) {
  xTaskCreate(dc_create_delayed_task, "dc_create", 3072, NULL, 5, NULL);
}

static void check_dc_watchdog(void) {
  if (connected_at == 0 || dc_opened) return;
  if (xTaskGetTickCount() - connected_at > pdMS_TO_TICKS(20000)) {
    ESP_LOGE(LOG_TAG, "DataChannel never opened 20s post-connect — reboot re-roll");
    esp_restart();
  }
}

static void pipecat_onconnectionstatechange_task(PeerConnectionState state,
                                                 void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED) {
    rgb_set_state(RGB_STATE_ERROR);
#ifndef LINUX_BUILD
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
#endif
  } else if (state == PEER_CONNECTION_CONNECTED) {
   rgb_set_state(RGB_STATE_BRIDGE_CONNECTED);
   connected_at = xTaskGetTickCount();  // arm the DC watchdog
 #ifndef LINUX_BUILD
    // Audio init deferred to here: I2S DMA allocations fragment internal RAM,
    // which must stay contiguous through DTLS cert setup (see main.cpp note).
    pipecat_init_audio_capture();
    pipecat_init_audio_decoder();
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        30000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(pipecat_send_audio_task, "audio_publisher",
                                  30000, NULL, 7, stack_memory, &task_buffer,
                                  0);
    pipecat_init_rtvi(peer_connection, &pipecat_rtvi_callbacks);
#endif
  }
}

static void pipecat_on_icecandidate_task(char *description, void *user_data) {
  char *local_buffer = (char *)malloc(MAX_HTTP_OUTPUT_BUFFER + 1);
  memset(local_buffer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
  pipecat_http_request(description, local_buffer);
  peer_connection_set_remote_description(peer_connection, local_buffer,
                                         SDP_TYPE_ANSWER);
  free(local_buffer);
}

void pipecat_init_webrtc() {
  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        pipecat_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };

  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }
  ESP_LOGI(LOG_TAG, "heap at webrtc init: free=%u largest=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  peer_connection_oniceconnectionstatechange(
      peer_connection, pipecat_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, pipecat_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection,
                                pipecat_ondatachannel_onmessage_task,
                                pipecat_ondatachannel_onopen_task, NULL);

  peer_connection_create_offer(peer_connection);
}

void pipecat_webrtc_loop() {
  peer_connection_loop(peer_connection);
  check_dc_watchdog();
}
