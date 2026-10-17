#include "boot_diag.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#define BD_MAGIC 0x6f736264u /* "osbd" */
#define BD_WHY_LEN 24

typedef struct {
  uint32_t magic;
  uint32_t up_s;
  char why[BD_WHY_LEN];
} bd_rtc_t;

/* RTC slow memory, left alone by the bootloader: survives software resets,
 * panics and watchdog resets. Garbage after a cold power-on, hence magic. */
static RTC_NOINIT_ATTR bd_rtc_t s_rtc;

static const char *s_reason = "unknown";
static char s_prev_why[BD_WHY_LEN];
static uint32_t s_prev_up_s;
static esp_timer_handle_t s_tick;

static const char *reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

uint32_t boot_diag_up_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static void tick(void *arg) {
  (void)arg;
  s_rtc.up_s = boot_diag_up_s();
}

void boot_diag_init(void) {
  esp_reset_reason_t r = esp_reset_reason();
  s_reason = reason_name(r);
  if (r != ESP_RST_POWERON && s_rtc.magic == BD_MAGIC) {
    memcpy(s_prev_why, s_rtc.why, BD_WHY_LEN);
    s_prev_why[BD_WHY_LEN - 1] = '\0';
    s_prev_up_s = s_rtc.up_s;
  }
  memset(&s_rtc, 0, sizeof(s_rtc));
  s_rtc.magic = BD_MAGIC;
  ESP_LOGW("boot_diag", "reset reason=%s why=\"%s\" prev_up_s=%u", s_reason,
           s_prev_why, (unsigned)s_prev_up_s);
  const esp_timer_create_args_t args = {.callback = tick, .name = "boot_diag"};
  if (esp_timer_create(&args, &s_tick) == ESP_OK) {
    esp_timer_start_periodic(s_tick, 1000000);
  }
}

void boot_diag_mark(const char *why) {
  strncpy(s_rtc.why, why ? why : "", BD_WHY_LEN - 1);
  s_rtc.why[BD_WHY_LEN - 1] = '\0';
  s_rtc.up_s = boot_diag_up_s();
}

void boot_diag_restart(const char *why) {
  boot_diag_mark(why);
  esp_restart();
}

const char *boot_diag_reason(void) { return s_reason; }
const char *boot_diag_prev_why(void) { return s_prev_why; }
uint32_t boot_diag_prev_up_s(void) { return s_prev_up_s; }
