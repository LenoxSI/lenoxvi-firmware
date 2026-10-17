#include <peer.h>

#define LOG_TAG "pipecat"
#define MAX_HTTP_OUTPUT_BUFFER 4096
#define HTTP_TIMEOUT_MS 10000
#define TICK_INTERVAL 15

// Wifi
extern void pipecat_init_wifi();
extern void pipecat_wifi_bot_reached(void);   /* M1: offer POST got 200 (flag only — any task) */
extern void pipecat_wifi_trial_start(void);   /* M1: start the trial watcher if a trial is pending */

// WebRTC / Media
extern void pipecat_init_audio_capture();
extern void pipecat_init_i2c_early();
extern void pipecat_init_audio_decoder();
extern void pipecat_init_audio_encoder();
extern void pipecat_send_audio(PeerConnection *peer_connection);
extern void pipecat_audio_decode(uint8_t *data, size_t size);
// Mic mute (H4, 2026-10-16): one persisted state per node (media.cpp).
// Array: kept in sync with the physical Mute button via XMOS pin X0D30.
extern void pipecat_mic_mute_init(void);     // after settings_load()
extern bool pipecat_mic_muted(void);
extern bool pipecat_mic_mute_toggle(void);
extern bool pipecat_mic_mute_set(bool muted);
extern bool pipecat_mute_push_take(void);    // true once per button change
extern "C" void pipecat_mute_hw_diag(int *btn);  // array: raw mute button bit (1 up, 0 down, -1 n/a)
// Audio-loop health (1.2.27, media.cpp): core, frames, mic overflows, worst
// work/gap since the previous call (resets the worst cases).
struct cmd_audio_stats_t;
extern "C" void pipecat_audio_stats(struct cmd_audio_stats_t *out);

// WebRTC / Signalling
extern void pipecat_init_webrtc();
extern void pipecat_webrtc_loop();
extern void pipecat_http_request(char *offer, char *answer);

// RTVI
typedef struct {
  void (*on_bot_started_speaking)();
  void (*on_bot_stopped_speaking)();
  void (*on_bot_tts_text)(const char *text);
  void (*on_oww_awake)();
  void (*on_oww_sleeping)();
} rtvi_callbacks_t;

extern rtvi_callbacks_t pipecat_rtvi_callbacks;

extern void pipecat_init_rtvi(PeerConnection *peer_connection, rtvi_callbacks_t *callbacks);
extern void pipecat_rtvi_send_client_ready();
extern void pipecat_rtvi_handle_message(const char* msg);
extern void pipecat_rtvi_send_raw(const char *json);  /* 1.1.0: cmd replies */

// OTA firmware update
extern void pipecat_check_ota_update(void);
// OTA health gate + rollback (design note H1): arm at boot (read-only), pass on
// the first bot round-trip; rejected = the version the bootloader rolled
// back from, if any.
extern void pipecat_ota_health_begin(void);
extern void pipecat_ota_health_ok(void);
extern bool pipecat_ota_pending_verify(void);
extern bool pipecat_ota_rejected_version(char *out, size_t out_len);
extern bool pipecat_ota_busy(void);
extern void pipecat_ota_defer_drop(void);
