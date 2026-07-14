/* screen.cpp — stub for Waveshare ESP32-S3-AUDIO-Board
 *
 * The original screen.cpp uses lvgl + esp-box-3 BSP for an LCD display.
 * The Waveshare audio board has no display in this config, so all
 * screen functions are no-ops. Removes the lvgl/BSP dependency entirely.
 */

#include "esp_log.h"
#include "main.h"

void pipecat_init_screen() {
    ESP_LOGI(LOG_TAG, "Screen: no display (stub)");
}

void pipecat_screen_system_log(const char *text) {
    ESP_LOGI(LOG_TAG, "[screen] %s", text);
}

void pipecat_screen_new_log() {}

void pipecat_screen_log(const char *text) {
    ESP_LOGI(LOG_TAG, "[screen] %s", text);
}
