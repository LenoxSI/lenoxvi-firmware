#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB Status Indicator — 12-LED ring on the reSpeaker XVF3800, driven by
 * the XMOS over I2C (GPO servicer resid 20). The XMOS animates effects
 * (breathing/rainbow/DOA) itself; we push state changes only.
 *
 * Other tasks set the state via rgb_set_state() — thread-safe,
 * minimal critical section (single enum write).
 */

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
    RGB_STATE_UPDATING,          // firmware download/flash in progress
    RGB_STATE_BOOT,              // highest — shows briefly on start
} rgb_state_t;

/**
 * Spawn the RGB render task. Safe to call before the XMOS servicer is up —
 * the task retries I2C writes until the probe succeeds (same pattern as the
 * DSP profile in media.cpp).
 */
void rgb_init(void);

/**
 * Set the current pipeline state. Safe to call from any task/ISR.
 */
void rgb_set_state(rgb_state_t new_state);

/**
 * Get the current state (for logging / debugging).
 */
rgb_state_t rgb_get_state(void);

#ifdef __cplusplus
}
#endif