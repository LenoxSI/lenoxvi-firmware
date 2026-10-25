/* Host test for the LED render decision in src/rgb.c: the connect flash and
 * the WIFI_UP effect. rgb.c is included directly (render_effect is static)
 * against stub FreeRTOS/ESP headers with a hand-driven tick clock.
 *   bash tests/run_rgb_test.sh */
#include <stdio.h>

static unsigned long g_ticks;                       /* 1 tick = 1 ms */
#include "../src/rgb.c"

/* Array-path bridge stubs (never called: the render task isn't run). */
esp_err_t xvf_led_write(uint8_t cmd, const uint8_t *payload, size_t len) {
    (void)cmd; (void)payload; (void)len; return ESP_OK;
}
bool xvf_led_bus_ready(void) { return true; }

static int fails;
static void check(const char *name, int cond) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}
static int is_flash(const led_effect_t *e) { return e->effect == 3 && e->color == 0x00FF00 && e->brightness == 120; }
static int is_dark(const led_effect_t *e)  { return e->effect == 0; }

int main(void) {
    const led_effect_t *e;

    /* 1. a session connects: green flash for 1.5 s from first render, then dark */
    g_ticks = 1000;
    rgb_set_state(RGB_STATE_BRIDGE_CONNECTED); rgb_flash_connected();
    e = render_effect(current_state);
    check("flash: shows solid green right after connect", is_flash(e));
    g_ticks = 1000 + 1499; e = render_effect(current_state);
    check("flash: still green at 1.499 s", is_flash(e));
    g_ticks = 1000 + 1500; e = render_effect(current_state);
    check("flash: dark idle at 1.5 s", is_dark(e));
    g_ticks = 1000 + 60000; e = render_effect(current_state);
    check("flash: stays dark afterwards (one-shot)", is_dark(e));

    /* 2. the render task read the pre-connect state after the flash was armed */
    g_ticks = 100000;
    rgb_set_state(RGB_STATE_WIFI_UP);
    rgb_flash_connected();                          /* armed, state not yet seen */
    e = render_effect(RGB_STATE_WIFI_UP);
    check("race: a Wi-Fi state read does not cancel an armed flash", !is_flash(e) && s_flash_pending);
    rgb_set_state(RGB_STATE_BRIDGE_CONNECTED);
    e = render_effect(current_state);
    check("race: the flash still shows once the connect state is seen", is_flash(e));

    /* 3. a wake word mid-flash ends it for good */
    g_ticks = 100200;
    rgb_set_state(RGB_STATE_LISTENING);
    e = render_effect(current_state);
    check("wake: listening replaces the flash at once", e->color == 0x800080 && e->effect == 3);
    rgb_set_state(RGB_STATE_BRIDGE_CONNECTED);
    e = render_effect(current_state);
    check("wake: back to idle within the old window stays dark", is_dark(e));

    /* 4. BRIDGE_CONNECTED without the call (the OTA path clearing UPDATING) */
    g_ticks = 200000;
    rgb_set_state(RGB_STATE_BRIDGE_CONNECTED);
    e = render_effect(current_state);
    check("ota path: BRIDGE_CONNECTED alone never flashes", is_dark(e));

    /* 5. session activity before the first render cancels a stale armed flash */
    rgb_flash_connected();
    e = render_effect(RGB_STATE_DARK);
    e = render_effect(RGB_STATE_BRIDGE_CONNECTED);
    check("stale: dark before render cancels the flash", is_dark(e));

    /* 6. bot unreachable breathes */
    e = effect_for(RGB_STATE_WIFI_UP);
    check("wifi up: breathing dim amber, not a steady light", e->effect == 1 && e->brightness == 40 && e->color == 0xFF9E00);

    /* 7. the flash colour is unique: not UPDATING's breathing green */
    e = effect_for(RGB_STATE_UPDATING);
    check("flash differs from updating (solid vs breathing)", e->effect == 1 && !is_flash(e));

    /* 8. tick wrap: the deadline math survives the 32-bit wrap */
    g_ticks = 0xFFFFFFFFul - 500;
    rgb_set_state(RGB_STATE_BRIDGE_CONNECTED); rgb_flash_connected();
    e = render_effect(current_state);
    g_ticks = 499;                                  /* 1000 ms later, wrapped */
    e = render_effect(current_state);
    check("wrap: still flashing 1.0 s later across the wrap", is_flash(e));
    g_ticks = 1100;
    e = render_effect(current_state);
    check("wrap: dark after 1.6 s across the wrap", is_dark(e));

    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
