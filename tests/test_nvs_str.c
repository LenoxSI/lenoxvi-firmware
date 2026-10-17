/* Host test for src/nvs_str.h (1.2.28). Built by tests/run_nvs_str_test.sh
 * with ASan + UBSan. nvs_get_str() lengths INCLUDE the NUL, so a value of
 * n chars arrives as len n+1. */
#include <stdio.h>
#include <string.h>

#include "nvs_str.h"

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } } while (0)

/* exercise with an exact-size heap buffer so ASan catches any overrun */
#include <stdlib.h>
static int copy_into(size_t dst_size, const char *s, char *out, size_t out_size) {
  char *dst = (char *)malloc(dst_size);
  memset(dst, 'X', dst_size);
  dst[dst_size - 1] = '\0';
  strcpy(out, "UNTOUCHED");
  int ok = nvs_str_copy(dst, dst_size, s, strlen(s) + 1);
  if (ok) { strncpy(out, dst, out_size - 1); out[out_size - 1] = '\0'; }
  free(dst);
  return ok;
}

int main(void) {
  char out[128];
  const char *ssid32 = "0123456789abcdef0123456789abcdef";           /* 32 */
  const char *name31 = "abcdefghijklmnopqrstuvwxyz01234";            /* 31 */
  const char *pass63 = "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"; /* 63 */
  CHECK("32-char SSID loads into the 33-byte buffer (was dropped)",
        copy_into(33, ssid32, out, sizeof out) && strcmp(out, ssid32) == 0);
  CHECK("31-char node name loads into the 32-byte buffer (was dropped)",
        copy_into(32, name31, out, sizeof out) && strcmp(out, name31) == 0);
  CHECK("63-char password loads into the 64-byte buffer (was dropped)",
        copy_into(64, pass63, out, sizeof out) && strcmp(out, pass63) == 0);
  CHECK("short value loads",
        copy_into(33, "HomeNet", out, sizeof out) && strcmp(out, "HomeNet") == 0);
  CHECK("empty stored string loads as empty (len 1)",
        copy_into(33, "", out, sizeof out) && strcmp(out, "") == 0);
  /* one char too long: must be refused, dst untouched */
  {
    char dst[32]; strcpy(dst, "keep");
    const char *s = "abcdefghijklmnopqrstuvwxyz012345";             /* 32 > 31 */
    int ok = nvs_str_copy(dst, sizeof dst, s, strlen(s) + 1);
    CHECK("value one char too long is refused, dst untouched",
          !ok && strcmp(dst, "keep") == 0);
  }
  {
    char dst[8]; strcpy(dst, "keep");
    int ok = nvs_str_copy(dst, sizeof dst, "", 0);
    CHECK("len 0 (nothing read) leaves dst untouched", !ok && strcmp(dst, "keep") == 0);
  }
  printf("\n%s\n", fails ? "SOME FAILED" : "ALL PASSED");
  return fails ? 1 : 0;
}
