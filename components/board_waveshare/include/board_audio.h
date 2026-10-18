#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bring up codec power, I2S (full duplex), the mic ADC, then the speaker DAC.
 * Never aborts: returns an error and leaves board_audio_read() producing
 * silence if a mic step fails; a speaker failure only leaves the speaker
 * dead (board_audio_speaker_ok() false, PA off) and still returns ESP_OK. */
esp_err_t board_audio_init(void);
/* Fill `out` with exactly `n` mono int16 samples at 16 kHz (blocking, ~20 ms
 * for n=320). Returns true if real audio, false if silence was substituted. */
bool board_audio_read(int16_t *out, int n);
/* Non-blocking: copies n mono int16 @16 kHz samples into the playback ring
 * buffer; drops (and counts) when the speaker is disabled/dead or the ring
 * is full. Safe to call from libpeer's task. */
void board_audio_play(const int16_t *pcm, int n);
/* Enable/disable the speaker and set its volume (0..100). Disabling flushes
 * the ring and turns the PA off at once. Defaults: disabled, volume 40. */
void board_audio_speaker_set(bool enabled, int volume);
/* Current speaker volume (0..100), whether or not the speaker is up. */
int board_audio_speaker_volume(void);
/* True once the ES8311 and the playback task are up. */
bool board_audio_speaker_ok(void);
#ifdef __cplusplus
}
#endif
