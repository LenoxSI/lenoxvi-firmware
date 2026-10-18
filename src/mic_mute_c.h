#pragma once
/* C-linkage read of the software mic-mute state (waveshare LED ring, rgb.c).
 * Defined at the end of media.cpp, waveshare builds only; this header is the
 * one shared prototype so a signature drift fails to compile. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool pipecat_mic_muted_c(void);
#ifdef __cplusplus
}
#endif
