/* board_audio_stub.c — Waveshare ESP32-S3-AUDIO audio, placeholder.
 *
 * Silence only: brings up nothing and paces the capture loop at 20 ms per
 * frame so the encoder/WebRTC path runs exactly as it will with real audio.
 * The ES7210/ES8311/TCA9555 bring-up replaces this file. */

#include <string.h>

#include "board_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t board_audio_init(void) {
    ESP_LOGW("board_audio", "board_audio: stub (silence)");
    return ESP_OK;
}

bool board_audio_read(int16_t *out, int n) {
    memset(out, 0, (size_t)n * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(20));
    return false;
}
