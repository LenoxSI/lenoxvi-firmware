#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bring up codec power, the mic ADC and I2S RX. Never aborts: returns an error
 * and leaves board_audio_read() producing silence if any step fails. */
esp_err_t board_audio_init(void);
/* Fill `out` with exactly `n` mono int16 samples at 16 kHz (blocking, ~20 ms
 * for n=320). Returns true if real audio, false if silence was substituted. */
bool board_audio_read(int16_t *out, int n);
#ifdef __cplusplus
}
#endif
