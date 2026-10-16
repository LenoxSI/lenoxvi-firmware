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
#include "cmd.h"

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

/* CONNECT watchdog (design note H3, 2026-10-14): the DC watchdog above only
 * arms AT CONNECTED — a session that never gets there (offer lost, ICE
 * stuck in CHECKING, or NEW forever) had NO timer and the node sat dark
 * until a power-cycle. Armed when the offer is created; a healthy connect
 * lands ~1 s after the offer (measured: offer 5.7 s → connected 6.6 s
 * after boot), so 30 s is generous for a slow-but-legit ICE. */
#define CONNECT_TIMEOUT_MS 30000
static TickType_t offer_created_at = 0;

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
    /* 1.1.2 connect-push: the running fw version rides the FIRST bytes
     * on the fresh channel (after this the bot's Nodes FW column knows
     * what's ACTUALLY running — the manifest only knows what's
     * available). Sends before the liveness probe window, once per
     * session open. */
    cmd_push_firmware_version();
    /* a fresh OTA image earns validity on the first bot
     * round-trip. The regular liveness probe is 5 min out, and an idle
     * house may send nothing — so probe once now; the bot's pong passes
     * the gate (cmd_note_activity). Asymmetric probe/pong as always: the
     * pong handler never replies. */
    if (pipecat_ota_pending_verify()) {
      pipecat_rtvi_send_raw(
          "{\"type\":\"cmd_result\",\"evt\":\"cmd_result\",\"cmd\":\"probe\","
          "\"ok\":true,\"data\":\"ota_health\"}");
      ESP_LOGI(LOG_TAG, "OTA health probe sent (image pending verify)");
    }
  } else {
    ESP_LOGE(LOG_TAG, "Failed to create DataChannel");
  }
  vTaskDelete(NULL);
}

static void pipecat_ondatachannel_onopen_task(void *userdata) {
  xTaskCreate(dc_create_delayed_task, "dc_create", 3072, NULL, 5, NULL);
}

static void check_connect_watchdog(void) {
  if (offer_created_at == 0 || connected_at != 0) return;
  if (xTaskGetTickCount() - offer_created_at > pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
    ESP_LOGE(LOG_TAG, "never reached CONNECTED %d s after the offer (stuck in "
             "NEW/CHECKING) — reboot re-roll", CONNECT_TIMEOUT_MS / 1000);
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }
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

  /* FAILED was silently ignored here — a session that ICE
   * declared dead just sat. Same re-roll as a drop. */
  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_FAILED ||
      state == PEER_CONNECTION_CLOSED) {
    rgb_set_state(RGB_STATE_ERROR);
    /* OTA flash in flight (2026-10-05): the churn loop — five reboots
     * to land one 1.2MB flash — was THIS esp_restart() firing mid-
     * download (the pull starves the audio task, the peer drops, the
     * handler reboots before esp_ota_write finishes). The OTA task
     * owns the only reboot during a flash; a dropped session during
     * a flash is EXPECTED — it re-offers when the OTA task reboots
     * the node with the new image. */
    if (pipecat_ota_busy()) {
      ESP_LOGW(LOG_TAG, "session dropped mid-OTA — not rebooting, flash in flight");
      pipecat_ota_defer_drop();  /* a FAILED flash reboots instead (ota.cpp) */
      return;
    }
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
    /* 023 fix (2026-09-29): NULL-check the audio-task stack alloc — a
     * silent alloc failure meant the audio task never started and the
     * session streamed silence (zero-audio session, 2026-09-25). */
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        30000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    if (stack_memory == NULL) {
      ESP_LOGE(LOG_TAG, "audio task PSRAM stack alloc failed — rebooting");
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }
    xTaskCreateStaticPinnedToCore(pipecat_send_audio_task, "audio_publisher",
                                  30000, NULL, 7, stack_memory, &task_buffer,
                                  0);
    pipecat_init_rtvi(peer_connection, &pipecat_rtvi_callbacks);
    cmd_liveness_start();  /* 5-min ping liveness (design note zombie fix) */
    // OTA check at CONNECTED (empirical bisect 2026-09-19): the identical
    // version GET crashed deterministically at got-ip (StoreProhibited,
    // ROM memcpy, TLS setup) but worked perfectly here in build 47 — heap
    // is settled, TLS session established by the offer POST moments
    // earlier. Internal-RAM static stack (largest free block ~20K here —
    // plain xTaskCreate silently fails).
    static bool ota_done = false;
    if (!ota_done) {
      ota_done = true;
      extern void pipecat_check_ota_update(void);
      static StaticTask_t ota_task_buf;
      static StackType_t ota_stack[6144 / sizeof(StackType_t)];
      xTaskCreateStaticPinnedToCore(
          [](void *arg) {
            /* Give the TLS stack a moment post-offer: spawning at the
             * CONNECTED instant races the offer POST's session setup and
             * the version GET dies ESP_ERR_HTTP_CONNECT (2026-09-19).
             * 5s costs nothing — deploys are not latency-sensitive.
             * Now 60s (2026-10-05): the check runs PAST session setup —
             * the audio task, WebRTC and the pipeline are all steady,
             * and a 1.2MB download no longer starves a booting session
             * (the 09:17 churn loop). If the session still drops mid-
             * download, pipecat_ota_busy() keeps the disconnect handler
             * from rebooting the node — the flash finishes, then the
             * OTA task's own reboot lands the new image. */
            vTaskDelay(pdMS_TO_TICKS(60000));
            pipecat_check_ota_update();  // reboots itself if it flashes
            vTaskDelete(NULL);
          },
          "ota_check", 6144 / sizeof(StackType_t), NULL, 3, ota_stack, &ota_task_buf, 0);
      ESP_LOGI(LOG_TAG, "OTA check task spawned (CONNECTED, 60s deferred)");
    }
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

  offer_created_at = xTaskGetTickCount();  /* arm the connect watchdog */
  if (offer_created_at == 0) offer_created_at = 1;  /* 0 = "not armed" */
  peer_connection_create_offer(peer_connection);
}

void pipecat_webrtc_loop() {
  peer_connection_loop(peer_connection);
  check_connect_watchdog();
  check_dc_watchdog();
  if (dc_opened) cmd_service_mute_push();   /* H4: button changes reach the bot */
}
