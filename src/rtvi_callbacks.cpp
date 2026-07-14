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
  // pipecat_screen_log(text);
  // pipecat_screen_log(" ");
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
