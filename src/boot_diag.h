/* Reboot black box (1.2.29): why did the node last reset?
 *
 * Two facts survive a reset in RTC memory (no flash writes): the tag of
 * the deliberate reboot path that fired, if any, and the uptime refreshed
 * once a second. At boot they are moved into RAM next to the chip's own
 * reset reason and reported in get_info's "boot" block:
 *   reason  chip view: power_on, sw, panic, int_wdt, task_wdt, wdt,
 *           brownout, ... (esp_reset_reason)
 *   why     our own reboot path when reason is "sw" ("" otherwise, or when
 *           a library called esp_restart directly)
 *   prev_up_s  seconds the previous boot had been running (to within 1 s)
 * A cold power-on has no previous record: why "" and prev_up_s 0.
 *
 * Every deliberate reboot in this firmware goes through boot_diag_restart;
 * paths that reboot inside an IDF call (rollback) tag first with
 * boot_diag_mark. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LINUX_BUILD
#include <esp_system.h>
static inline void boot_diag_init(void) {}
static inline void boot_diag_mark(const char *why) { (void)why; }
static inline void boot_diag_restart(const char *why) { (void)why; esp_restart(); }
#else
/* Call first thing in app_main: captures the previous boot's record and
 * starts this boot's uptime heartbeat. */
void boot_diag_init(void);
/* Record why the next reset happens (truncated to 23 chars). */
void boot_diag_mark(const char *why);
/* boot_diag_mark(why) then esp_restart(). Never returns. */
void boot_diag_restart(const char *why) __attribute__((noreturn));

/* This boot's view of the previous reset, for get_info. */
const char *boot_diag_reason(void);   /* static string, never NULL */
const char *boot_diag_prev_why(void); /* "" when none */
uint32_t boot_diag_prev_up_s(void);
uint32_t boot_diag_up_s(void);
#endif

#ifdef __cplusplus
}
#endif
