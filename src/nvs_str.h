/* Copy a string read back from NVS into a fixed buffer (1.2.28).
 *
 * nvs_get_str() reports the stored length INCLUDING the terminating NUL.
 * The old check treated it as the bare string length, so a value that
 * exactly filled its buffer (a 32-char SSID, a 31-char node name, a 63-char
 * Wi-Fi password) was dropped at every boot and the baked-in fallback won.
 *
 * Pure C, no ESP-IDF — host-tested by tests/run_nvs_str_test.sh. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* src holds len_with_nul bytes as returned by nvs_get_str (string + NUL).
 * Copies into dst and returns true when it fits (len_with_nul <= dst_size);
 * leaves dst untouched and returns false when it doesn't, or when there is
 * nothing to copy (len 0). Always NUL-terminates what it copies. */
static inline bool nvs_str_copy(char *dst, size_t dst_size, const char *src,
                                size_t len_with_nul) {
  if (len_with_nul == 0 || len_with_nul > dst_size) return false;
  memcpy(dst, src, len_with_nul - 1);
  dst[len_with_nul - 1] = '\0';
  return true;
}
