#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#ifndef LINUX_BUILD
#include "nvs_flash.h"
#include "rgb.h"

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  rgb_init();
  pipecat_init_screen();
  peer_init();
  pipecat_init_audio_capture();
  pipecat_init_audio_decoder();
  pipecat_init_wifi();

  // OTA: check for firmware update after WiFi connects, before WebRTC.
  // If an update is found and flashed, the device reboots into the new image.
  // Mark the current app valid first (rollback support) so a good image
  // survives even if WebRTC fails after this point.
  pipecat_ota_mark_valid();
  pipecat_check_ota_update();

  pipecat_init_webrtc();

  pipecat_screen_system_log("Pipecat ESP32 client initialized\n");

  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  pipecat_webrtc();

  while (1) {
    pipecat_webrtc_loop();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif
