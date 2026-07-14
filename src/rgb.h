#pragma once

#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB Status Indicator — 7× WS2812B-0807 ring on GPIO38
 *
 * Dedicated FreeRTOS task renders pipeline state to the LED ring.
 * Other tasks set the state via rgb_set_state() — thread-safe,
 * minimal critical section (single enum write).
 */

#define RGB_GPIO            38
#define RGB_LED_COUNT       7
#define RGB_RESOLUTION_HZ   (10 * 1000 * 1000)  // 10 MHz RMT
#define RGB_BRIGHTNESS_MAX  153                  // 60% of 255

// State priority: higher number = higher priority
typedef enum {
    RGB_STATE_DARK = 0,
    RGB_STATE_WIFI_CONNECTING,
    RGB_STATE_WIFI_UP,
    RGB_STATE_BRIDGE_CONNECTED,   // OWW sleeping
    RGB_STATE_LISTENING,          // OWW awake
    RGB_STATE_PROCESSING,         // STT/LLM
    RGB_STATE_SPEAKING,           // TTS
    RGB_STATE_ERROR,
    RGB_STATE_BOOT,              // highest — shows briefly on start
} rgb_state_t;

/**
 * Initialize the LED strip and spawn the RGB render task.
 * Call once from app_main() after codecs/WiFi init is queued.
 */
void rgb_init(void);

/**
 * Set the current pipeline state. Safe to call from any task/ISR.
 * The render task picks this up on its next frame.
 */
void rgb_set_state(rgb_state_t new_state);

/**
 * Get the current state (for logging / debugging).
 */
rgb_state_t rgb_get_state(void);

#ifdef __cplusplus
}
#endif