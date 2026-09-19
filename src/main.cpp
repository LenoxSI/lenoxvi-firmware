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
  /* I2C + XVF probe BEFORE WiFi: the LED ring renders boot/WiFi states
   * through the XMOS, so the bus must exist before those states fire.
   * (Audio/I2S init stays deferred to post-DTLS — heap ordering.) */
  pipecat_init_i2c_early();
  pipecat_init_screen();
  peer_init();
  // NOTE: audio init (I2S DMA buffers) is deferred until AFTER the WebRTC
  // peer connects — see wifi.cpp / webrtc.cpp. I2S's internal-RAM DMA
  // allocations fragment the heap; mbedtls_ssl_cookie_setup needs one large
  // CONTIGUOUS internal block and fails with -0x0020 (ALLOC_FAILED) if I2S
  // runs first (measured: largest block 25.6KB with I2S-first vs enough when
  // DTLS allocates first). The audio task starts on CONNECTED anyway.
  pipecat_init_wifi();

  // OTA: check for firmware update after WiFi connects, before WebRTC.
  // If an update is found and flashed, the device reboots into the new image.
  // Mark the current app valid first (rollback support) so a good image
  // survives even if WebRTC fails after this point.
  pipecat_ota_mark_valid();
  pipecat_check_ota_update();

  // WebRTC init is deferred to the first IP_EVENT_STA_GOT_IP (see wifi.cpp) —
  // starting it before the network stack allocates exhausts the heap and
  // kills the DTLS certificate setup.

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
