#include "rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "rgb";

// ── Driver handle ─────────────────────────────────────────────
static led_strip_handle_t strip = NULL;

// ── Shared state ──────────────────────────────────────────────
static volatile rgb_state_t current_state = RGB_STATE_BOOT;
static volatile rgb_state_t rendered_state = RGB_STATE_DARK;

// ── Brightness helper ─────────────────────────────────────────
// Channel values are 0–255 pre-brightness; we scale by bright/255.
static inline uint8_t scale(uint8_t v, uint8_t bright) {
    return (uint16_t)v * bright / 255;
}

// ── Set all 7 pixels to the same color ────────────────────────
static void set_all_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t bright) {
    uint8_t sr = scale(r, bright);
    uint8_t sg = scale(g, bright);
    uint8_t sb = scale(b, bright);
    for (int i = 0; i < RGB_LED_COUNT; i++) {
        led_strip_set_pixel(strip, i, sr, sg, sb);
    }
    led_strip_refresh(strip);
}

static void set_all_off(void) {
    led_strip_clear(strip);
}

// ── Per-state render frames ───────────────────────────────────
// Each returns true if the caller should loop (animated) or false
// if the state is solid and the task should block until a change.

static bool render_boot(void) {
    // White fade-in/fade-out, 200ms total
    for (int b = 0; b <= RGB_BRIGHTNESS_MAX; b += 15) {
        set_all_rgb(255, 255, 255, b);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int b = RGB_BRIGHTNESS_MAX; b >= 0; b -= 15) {
        set_all_rgb(255, 255, 255, b);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    set_all_off();
    return false;  // one-shot
}

static bool render_wifi_connecting(void) {
    set_all_rgb(255, 200, 0, 77);   // yellow, 30%
    return false;
}

static bool render_wifi_up(void) {
    set_all_rgb(255, 200, 0, 153);  // yellow, 60%
    return false;
}

static bool render_bridge_connected(void) {
    // Lights off when idle — don't light up a dark room
    set_all_off();
    return false;
}

static bool render_listening(void) {
    // Purple solid — wake word accepted, listening for user speech
    set_all_rgb(128, 0, 128, 128);  // purple, 50%
    return false;
}

static bool render_processing(void) {
    // Cyan pulse: 300ms on, 200ms off
    set_all_rgb(0, 255, 255, 179);   // 70%
    vTaskDelay(pdMS_TO_TICKS(300));
    set_all_off();
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
}

static bool render_speaking(void) {
    set_all_rgb(0, 255, 255, 179);   // cyan solid, 70%
    return false;
}

static bool render_error(void) {
    // Red blink: 100ms on/off × 3, then 1s pause
    for (int i = 0; i < 3; i++) {
        set_all_rgb(255, 0, 0, 200);
        vTaskDelay(pdMS_TO_TICKS(100));
        set_all_off();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    return true;
}

static bool render_dark(void) {
    set_all_off();
    return false;
}

// ── State priority ───────────────────────────────────────────
// Higher enum value = higher priority. We only switch to a new
// state if it's >= current (error always wins, boot always wins).
// Exception: BOOT is one-shot and drops to the next state after.

static bool should_switch(rgb_state_t proposed) {
    // BOOT, DARK, and BRIDGE_CONNECTED always pass — they're resets/clears,
    // not downgrades.  BRIDGE_CONNECTED fires on every fresh WebRTC connect
    // and must override stale states (e.g. SPEAKING left over from a prior
    // connection that died without a clean disconnect).
    if (rendered_state == RGB_STATE_BOOT && proposed != RGB_STATE_BOOT)
        return true;
    if (proposed == RGB_STATE_DARK || proposed == RGB_STATE_BRIDGE_CONNECTED)
        return true;
    return proposed >= current_state;
}

// ── Render task ──────────────────────────────────────────────

static void rgb_task(void *arg) {
    bool animated = false;

    for (;;) {
        rgb_state_t s = current_state;

        if (s != rendered_state) {
            if (!should_switch(s)) {
                // Lower priority — skip this frame, keep animating current
                s = rendered_state;
            } else {
                rendered_state = s;
                animated = false;  // reset on state change
            }
        }

        bool loop = false;

        switch (rendered_state) {
            case RGB_STATE_BOOT:            loop = render_boot();
                                            if (!loop) {
                                                // BOOT is one-shot — drop to DARK after fade
                                                rendered_state = RGB_STATE_DARK;
                                                current_state = RGB_STATE_DARK;
                                            }
                                            break;
            case RGB_STATE_WIFI_CONNECTING:  loop = render_wifi_connecting(); break;
            case RGB_STATE_WIFI_UP:          loop = render_wifi_up(); break;
            case RGB_STATE_BRIDGE_CONNECTED: loop = render_bridge_connected(); break;
            case RGB_STATE_LISTENING:        loop = render_listening(); break;
            case RGB_STATE_PROCESSING:       loop = render_processing(); break;
            case RGB_STATE_SPEAKING:         loop = render_speaking(); break;
            case RGB_STATE_ERROR:            loop = render_error(); break;
            case RGB_STATE_DARK:             loop = render_dark(); break;
        }

        if (loop) {
            // Animated state — loop back immediately (delay is inside render fn)
            continue;
        }

        // Solid state — wait for a state change notification
        // Poll at 50ms so BOOT→next transition is snappy
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Public API ────────────────────────────────────────────────

void rgb_init(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = RGB_RESOLUTION_HZ,
        .flags.with_dma = false,  // 7 LEDs — no DMA needed
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return;
    }
    led_strip_clear(strip);
    current_state = RGB_STATE_BOOT;
    rendered_state = RGB_STATE_DARK;

    xTaskCreate(rgb_task, "rgb", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "RGB ring initialized on GPIO%d (%d LEDs)", RGB_GPIO, RGB_LED_COUNT);
}

void rgb_set_state(rgb_state_t new_state) {
    if (new_state == current_state) return;
    current_state = new_state;
    ESP_LOGD(TAG, "state -> %d", new_state);
}

rgb_state_t rgb_get_state(void) {
    return current_state;
}