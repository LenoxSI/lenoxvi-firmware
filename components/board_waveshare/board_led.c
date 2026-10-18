/* board_led.c — Waveshare ESP32-S3-AUDIO 7-pixel WS2812B ring (GPIO38, RMT).
 *
 * Renders the reSpeaker XMOS LED effect encoding so src/rgb.c can drive both
 * boards from one state table: 0 = off, 1 = breathing, 3 = solid (anything
 * else renders solid). Pixel 0 doubles as the mute indicator (the array has a
 * separate red mute LED): while muted it is solid red, the rest keep the
 * state effect.
 *
 * A small low-priority task owns the strip. It renders every FRAME_MS while
 * breathing, otherwise only when the target changed, and blocks on a task
 * notification (100 ms timeout) in between. Init fails soft: on any failure
 * the module stays dead and every call is a no-op. */

#include "board_led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define TAG "board_led"

#define LED_GPIO          38
#define LED_COUNT         7
#define LED_RESOLUTION_HZ (10 * 1000 * 1000)
#define MUTE_PIXEL        0

/* WS2812 at full drive is far brighter than the reSpeaker ring — cap at 60%. */
#define BOARD_LED_MAX     153

#define EFFECT_OFF        0
#define EFFECT_BREATHING  1

#define BREATH_PERIOD_MS  2000
#define BREATH_MIN_PERMIL 100      /* breathing floor: 10% of the state brightness */
#define FRAME_MS          25
#define IDLE_WAIT_MS      100

#define TASK_STACK        3072
#define TASK_PRIO         3        /* below audio (7) and rgb (4) */

typedef struct {
    uint8_t  effect;
    uint8_t  brightness;
    uint32_t color;      /* 0x00RRGGBB */
    bool     muted;
} led_target_t;

static led_strip_handle_t s_strip;
static TaskHandle_t       s_task;     /* NULL = dead: every call is a no-op */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_target_t       s_target;   /* guarded by s_lock */
static bool               s_dirty;    /* guarded by s_lock */

static esp_err_t fail(const char *step, esp_err_t err) {
    ESP_LOGE(TAG, "board_led: %s failed: %s", step, esp_err_to_name(err));
    return err;
}

/* out = c * brightness/255 * BOARD_LED_MAX/255 */
static inline uint8_t scale(uint8_t c, uint32_t brightness) {
    return (uint8_t)((uint32_t)c * brightness * BOARD_LED_MAX / (255u * 255u));
}

static void set_pixel(int i, uint32_t color, uint32_t brightness) {
    led_strip_set_pixel(s_strip, i, scale((color >> 16) & 0xFF, brightness),
                        scale((color >> 8) & 0xFF, brightness), scale(color & 0xFF, brightness));
}

/* Triangle 10%..100% of `brightness` over BREATH_PERIOD_MS. */
static uint32_t breath_brightness(uint8_t brightness, uint32_t now_ms) {
    uint32_t ph  = now_ms % BREATH_PERIOD_MS;
    uint32_t tri = ph < BREATH_PERIOD_MS / 2 ? ph : BREATH_PERIOD_MS - ph;  /* 0..period/2 */
    uint32_t permil = BREATH_MIN_PERMIL +
                      (1000 - BREATH_MIN_PERMIL) * tri / (BREATH_PERIOD_MS / 2);
    return (uint32_t)brightness * permil / 1000;
}

static void render(const led_target_t *t, uint32_t now_ms) {
    static bool warned;
    uint32_t b = t->effect == EFFECT_OFF       ? 0
               : t->effect == EFFECT_BREATHING ? breath_brightness(t->brightness, now_ms)
               : t->brightness;
    for (int i = 0; i < LED_COUNT; i++) {
        if (i == MUTE_PIXEL && t->muted) set_pixel(i, 0xFF0000, 255);  /* array table red: 255 */
        else                             set_pixel(i, t->color, b);
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK && !warned) {
        ESP_LOGW(TAG, "board_led: refresh failed: %s", esp_err_to_name(err));
        warned = true;
    }
}

static void led_task(void *arg) {
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        led_target_t t = s_target;
        bool dirty = s_dirty;
        s_dirty = false;
        portEXIT_CRITICAL(&s_lock);

        bool breathing = t.effect == EFFECT_BREATHING;
        if (dirty || breathing) render(&t, (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(breathing ? FRAME_MS : IDLE_WAIT_MS));
    }
}

esp_err_t board_led_init(void) {
    if (s_task) return ESP_OK;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  /* this ring is RGB-order (WS2812B-0807): GRB swapped red/green on hardware */
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = LED_RESOLUTION_HZ,
        .flags = { .with_dma = false },   /* 7 LEDs — no DMA needed */
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        s_strip = NULL;
        return fail("led_strip_new_rmt_device", err);
    }
    err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return fail("led_strip_clear", err);
    }
    if (xTaskCreate(led_task, "board_led", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        led_strip_del(s_strip);
        s_strip = NULL;
        return fail("xTaskCreate", ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "board_led: ring up on GPIO%d (%d LEDs, cap %d/255)",
             LED_GPIO, LED_COUNT, BOARD_LED_MAX);
    return ESP_OK;
}

void board_led_set(uint8_t effect, uint8_t brightness, uint32_t color) {
    if (!s_task) return;
    portENTER_CRITICAL(&s_lock);
    bool changed = s_target.effect != effect || s_target.brightness != brightness ||
                   s_target.color != color;
    s_target.effect = effect;
    s_target.brightness = brightness;
    s_target.color = color;
    s_dirty |= changed;
    portEXIT_CRITICAL(&s_lock);
    if (changed) xTaskNotifyGive(s_task);
}

void board_led_set_muted(bool muted) {
    if (!s_task) return;
    portENTER_CRITICAL(&s_lock);
    bool changed = s_target.muted != muted;
    s_target.muted = muted;
    s_dirty |= changed;
    portEXIT_CRITICAL(&s_lock);
    if (changed) xTaskNotifyGive(s_task);
}
