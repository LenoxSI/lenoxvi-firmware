#include <esp_log.h>

#include "main.h"
#include "rgb.h"

static void on_bot_started_speaking() {
  rgb_set_state(RGB_STATE_SPEAKING);
}

static void on_bot_stopped_speaking() {
  rgb_set_state(RGB_STATE_LISTENING);  // back to purple (OWW still awake)
}

static void on_bot_tts_text(const char *text) {
  /* (screen stub removed 2026-10-07 — Waveshare-only artifact; the
   * reSpeaker/Flex nodes have no display. TTS text is observable via
   * RTVI on the wire, nothing needed here.) */
}

static void on_oww_awake() {
  rgb_set_state(RGB_STATE_LISTENING);  // purple solid
}

static void on_oww_sleeping() {
  rgb_set_state(RGB_STATE_DARK);  // lights off
}

rtvi_callbacks_t pipecat_rtvi_callbacks = {
    .on_bot_started_speaking = on_bot_started_speaking,
    .on_bot_stopped_speaking = on_bot_stopped_speaking,
    .on_bot_tts_text = on_bot_tts_text,
    .on_oww_awake = on_oww_awake,
    .on_oww_sleeping = on_oww_sleeping,
};
