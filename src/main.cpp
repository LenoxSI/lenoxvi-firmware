#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#include "settings.h"

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
  settings_load();  /* NVS name + wifi creds; bakes become fallbacks */
  pipecat_mic_mute_init();  /* H4: a muted node boots muted */
  /* Hostname follows the node name (UniFi shows it); sta netif exists
   * after esp_netif_init inside pipecat_init_wifi — set it from the
   * got-ip path instead (esp_netif_set_hostname on the default sta). */
  /* I2C + XVF probe BEFORE WiFi: the LED ring renders boot/WiFi states
   * through the XMOS, so the bus must exist before those states fire.
   * (Audio/I2S init stays deferred to post-DTLS — heap ordering.) */
  pipecat_init_i2c_early();
  peer_init();
  // OTA health gate (design note H1, 2026-10-13). This used to be a
  // boot-time mark-valid — which made rollback meaningless: every image
  // validated itself before it ever proved it could reach the bot. Now
  // this call is READ-ONLY (state query + spawn the gate task); the image
  // earns validity on the first bot round-trip over the data channel, and
  // the otadata write happens in the gate's own task with a static
  // internal-RAM stack. History: the 2026-09-19 double-exception crash
  // loop came from writing otadata inside the WiFi event handler (tiny
  // sys_evt stack) — see ota.cpp's gate comment.
  pipecat_ota_health_begin();
  // NOTE: audio init (I2S DMA buffers) is deferred until AFTER the WebRTC
  // peer connects — see wifi.cpp / webrtc.cpp. I2S's internal-RAM DMA
  // allocations fragment the heap; mbedtls_ssl_cookie_setup needs one large
  // CONTIGUOUS internal block and fails with -0x0020 (ALLOC_FAILED) if I2S
  // runs first (measured: largest block 25.6KB with I2S-first vs enough when
  // DTLS allocates first). The audio task starts on CONNECTED anyway.
  pipecat_init_wifi();

  // OTA check now runs from the got-ip event handler (wifi.cpp) — running
  // it here raced the network AND failed TLS allocation before the heap
  // settled (mbedtls -0x7F00, 2026-09-19).

  // WebRTC init is deferred to the first IP_EVENT_STA_GOT_IP (see wifi.cpp) —
  // starting it before the network stack allocates exhausts the heap and
  // kills the DTLS certificate setup.

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
