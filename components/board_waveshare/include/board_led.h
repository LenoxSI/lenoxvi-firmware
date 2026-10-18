#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bring up the 7-pixel WS2812B ring (GPIO38, RMT) and its animation task.
 * Never aborts: on failure logs, returns the error, and every later call
 * is a no-op. */
esp_err_t board_led_init(void);
/* Show a state effect on the ring, same encoding as the reSpeaker XMOS:
 * effect 0 = off, 1 = breathing, 3 = solid; brightness 0..255; color
 * 0x00RRGGBB. Returns at once — the animation task renders it. */
void board_led_set(uint8_t effect, uint8_t brightness, uint32_t color);
/* Mute indicator (the array's separate red LED): while muted, pixel 0 is
 * solid red; the other pixels keep the state effect. */
void board_led_set_muted(bool muted);
#ifdef __cplusplus
}
#endif
