#include "rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb";

/* ── reSpeaker XVF3800 LED ring over I2C ──────────────────────────────────
 * The 12-LED ring on the reSpeaker board is owned and driven by the XMOS,
 * NOT by a WS2812 strip on the ESP32 (that was the Waveshare board's setup —
 * GPIO38 writes went nowhere on this board; "no LEDs" bug, 2026-09-19).
 *
 * We set state by writing the XMOS GPO servicer over I2C (Seeed v1.0.8 map):
 *   resid 20: LED_EFFECT (cmd 12) 0=off 1=breathing 2=rainbow 3=solid 4=DOA 5=ring
 *             LED_BRIGHTNESS (cmd 13) 0..255
 *             LED_COLOR (cmd 16) uint32 RGB (single byte payload = value)
 *             LED_RING_COLOR (cmd 19) 12 × uint32
 * The XMOS animates effects 1/2/4/5 itself — we only push state changes.
 *
 * I2C primitives live in media.cpp (xvf3800 device handle, probe-with-retry
 * at boot). rgb calls into them via a small bridge so this file stays
 * board-agnostic in shape. */

/* State → XMOS effect mapping (colors chosen to match the old ring semantics):
 *   BOOT            → breathing, blue      (power-up)
 *   WIFI_CONNECTING → breathing, amber
 *   WIFI_UP         → solid dim amber
 *   BRIDGE_CONNECTED→ off                  (dark when idle — dark-room rule)
 *   LISTENING       → solid, purple        (wake word accepted)
 *   PROCESSING      → breathing, cyan      (STT/LLM work)
 *   SPEAKING        → solid, cyan          (TTS out the Sonos)
 *   ERROR           → breathing, red
 *   DARK            → off */

typedef struct {
    uint8_t effect;   /* GPO cmd 12 value */
    uint8_t brightness; /* GPO cmd 13 value (0..255) */
    uint32_t color;   /* GPO cmd 16 value (0x00RRGGBB) */
} led_effect_t;

#define XVF_GPO_RESID        20
#define XVF_CMD_LED_EFFECT   12
#define XVF_CMD_LED_BRIGHT   13
#define XVF_CMD_LED_COLOR    16

/* Bridge into media.cpp's XVF control plane (C linkage: rgb.c is C, media.cpp is C++) */
#ifdef __cplusplus
extern "C" {
esp_err_t xvf_led_write(uint8_t cmd, const uint8_t *payload, size_t len);
bool xvf_led_bus_ready(void);
}
#else
extern esp_err_t xvf_led_write(uint8_t cmd, const uint8_t *payload, size_t len);
extern bool xvf_led_bus_ready(void);
extern void xvf_mute_service(void);   /* H4: array mute <-> X0D30 sync (media.cpp) */
#endif

static esp_err_t led_set_effect(uint8_t effect) {
    return xvf_led_write(XVF_CMD_LED_EFFECT, &effect, 1);
}
static esp_err_t led_set_brightness(uint8_t bright) {
    return xvf_led_write(XVF_CMD_LED_BRIGHT, &bright, 1);
}
static esp_err_t led_set_color(uint32_t rgb) {
    uint8_t payload[4] = {(uint8_t)(rgb & 0xFF), (uint8_t)((rgb >> 8) & 0xFF),
                          (uint8_t)((rgb >> 16) & 0xFF), (uint8_t)((rgb >> 24) & 0xFF)};
    return xvf_led_write(XVF_CMD_LED_COLOR, payload, 4);
}

static const led_effect_t *effect_for(rgb_state_t s) {
    static const led_effect_t EFF_BOOT       = {1, 255, 0x0000FF};
    static const led_effect_t EFF_WIFI_CONN  = {1, 255, 0xFF9E00};
    static const led_effect_t EFF_WIFI_UP    = {3,  40, 0xFF9E00};
    static const led_effect_t EFF_BRIDGE     = {0,   0, 0x000000};
    static const led_effect_t EFF_LISTENING  = {3, 255, 0x800080};
    static const led_effect_t EFF_PROCESSING = {1, 255, 0x00FFFF};
    static const led_effect_t EFF_SPEAKING   = {3, 255, 0x00FFFF};
    static const led_effect_t EFF_ERROR      = {1, 255, 0xFF0000};
    static const led_effect_t EFF_DARK       = {0,   0, 0x000000};
    switch (s) {
        case RGB_STATE_BOOT:            return &EFF_BOOT;
        case RGB_STATE_WIFI_CONNECTING: return &EFF_WIFI_CONN;
        case RGB_STATE_WIFI_UP:         return &EFF_WIFI_UP;
        case RGB_STATE_BRIDGE_CONNECTED: return &EFF_BRIDGE;
        case RGB_STATE_LISTENING:       return &EFF_LISTENING;
        case RGB_STATE_PROCESSING:       return &EFF_PROCESSING;
        case RGB_STATE_SPEAKING:        return &EFF_SPEAKING;
        case RGB_STATE_ERROR:           return &EFF_ERROR;
        default:                        return &EFF_DARK;
    }
}

// ── Shared state ──────────────────────────────────────────────
static volatile rgb_state_t current_state = RGB_STATE_BOOT;
static volatile rgb_state_t rendered_state = RGB_STATE_DARK;

// ── State priority ───────────────────────────────────────────
// Higher enum value = higher priority. We only switch to a new
// state if it's >= current (error always wins, boot always wins).
// Exception: BOOT is one-shot and drops to the next state after.

static bool should_switch(rgb_state_t proposed) {
    if (rendered_state == RGB_STATE_BOOT && proposed != RGB_STATE_BOOT)
        return true;
    if (proposed == RGB_STATE_DARK || proposed == RGB_STATE_BRIDGE_CONNECTED)
        return true;
    return proposed >= current_state;
}

// ── Render task ──────────────────────────────────────────────

static void rgb_task(void *arg) {
    /* The I2C bus doesn't exist until WebRTC-connect audio init (DTLS heap
     * ordering — see media.cpp). Until then, keep the latest state and DON'T
     * spam failed writes; the moment the bus is ready, the states in
     * current_state render for real. This is why boot/WiFi amber never
     * showed (2026-09-19): writes before bus-ready all died. */
    bool bus_ready_flushed = false;
    for (;;) {
        if (!bus_ready_flushed) {
            if (!xvf_led_bus_ready()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            rendered_state = RGB_STATE_DARK;  /* force re-render of current */
            bus_ready_flushed = true;
        }
        xvf_mute_service();   /* H4: runs every tick, rate-limits itself */
        rgb_state_t s = current_state;

        if (s != rendered_state) {
            if (!should_switch(s)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            const led_effect_t *eff = effect_for(s);
            /* Push all three registers; effect order matters (color/bright first
             * so an effect change applies with them in place). */
            esp_err_t r1 = led_set_color(eff->color);
            esp_err_t r2 = led_set_brightness(eff->brightness);
            esp_err_t r3 = led_set_effect(eff->effect);
            if (r1 != ESP_OK || r2 != ESP_OK || r3 != ESP_OK) {
                ESP_LOGW(TAG, "LED write failed (c=%s b=%s e=%s)",
                         esp_err_to_name(r1), esp_err_to_name(r2), esp_err_to_name(r3));
                vTaskDelay(pdMS_TO_TICKS(500));  /* retry next tick */
                continue;
            }
            rendered_state = s;
            ESP_LOGI(TAG, "LED state -> %d (effect=%u bright=%u)", (int)s,
                     (unsigned)eff->effect, (unsigned)eff->brightness);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Public API ────────────────────────────────────────────────

void rgb_init(void) {
#if LENOXVI_BOARD_FLEX
    /* FLEX (2026-10-07): the reSpeaker Flex has NO LED ring — the XIAO
     * footprint isn't wired to any pixels, and the XMOS LED servicer
     * (resid 20) has nothing behind it on this board. Skipping the RGB
     * task entirely; the flex node's state is visible via dashboard +
     * serial. */
    ESP_LOGI(TAG, "rgb: flex build — no LED ring on this board, task skipped");
#else
    /* XMOS probe happens in media.cpp audio init — but rgb_init runs BEFORE
     * audio init (main.cpp boot order), and the XVF3800 servicer NACKs for
     * the first seconds after power-up. So: BOOT state is queued via the
     * state machine; the render task starts here and retries writes until
     * the servicer answers. The BOOT effect (blue breathing) lands as soon
     * as the XMOS is ready — same probe-with-retry pattern as the DSP profile.
     */
    current_state = RGB_STATE_BOOT;
    rendered_state = RGB_STATE_DARK;
    BaseType_t ok = xTaskCreate(rgb_task, "rgb", 4096, NULL, 4, NULL);
    if (ok != pdPASS) ESP_LOGE(TAG, "rgb task spawn failed");
#endif
}

void rgb_set_state(rgb_state_t new_state) {
    current_state = new_state;
}

rgb_state_t rgb_get_state(void) {
    return current_state;
}