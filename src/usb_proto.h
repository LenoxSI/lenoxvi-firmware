#ifndef LENOXVI_USB_PROTO_H
#define LENOXVI_USB_PROTO_H
/* USB setup protocol v1: the line protocol the dashboard's Node setup page
 * speaks over the board's own USB port. Pure C (cJSON only) so parsing,
 * rules and replies are host-tested (tests/test_usb_proto.c); the ESP glue
 * (driver, task, NVS) lives in usb_cmd.c.
 *
 * Lines are UTF-8, '\n'-terminated ('\r' stripped), at most USB_LINE_MAX
 * bytes including the prefix. Requests are "OH>" + one JSON object; replies
 * are "\nOH<" + one JSON object + "\n". Anything else on the port is log
 * text and is ignored. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define USB_PROTO_VERSION 1
#define USB_LINE_MAX 512        /* bytes per line, prefix included */
#define USB_BOT_URL_MAX 128     /* incl. NUL */

typedef enum { USB_CMD_NONE = 0, USB_CMD_INFO, USB_CMD_SET, USB_CMD_RESTART } usb_cmd_t;

typedef struct {
  usb_cmd_t cmd;
  long id;                      /* -1 = the request had no id */
  bool has_wifi, has_name, has_bot_url;
  char wifi_ssid[33];
  char wifi_pass[65];
  char name[32];
  char bot_url[USB_BOT_URL_MAX];
} usb_req_t;

typedef struct {                /* deliberately has NO password field */
  const char *track, *fw, *name, *wifi_ssid, *bot_url, *link, *ip;
  uint8_t mac[6];
  bool bot_url_saved;
  uint32_t up_s;
} usb_info_t;

typedef struct { char buf[USB_LINE_MAX + 1]; size_t len; bool overflow; } usb_line_t;

void usb_line_init(usb_line_t *st);
/* Feed raw bytes; on_line gets each complete line ('\r' stripped,
 * NUL-terminated). An overlong line calls on_overflow once and is dropped
 * through its '\n'. */
void usb_line_feed(usb_line_t *st, const uint8_t *data, size_t n,
                   void (*on_line)(const char *line, void *ctx),
                   void (*on_overflow)(void *ctx), void *ctx);

/* false = not a request (no "OH>" prefix): ignore silently.
 * true + err[0] != 0 = a request that gets an error reply ("bad json",
 * "unknown cmd", "<field>: <rule>" for a mistyped field). */
bool usb_proto_parse(const char *line, usb_req_t *out, char *err, size_t errlen);

/* 0 = every present field passes; else err = "<field>: <rule>". The Wi-Fi
 * fields travel together. Validates ALL fields before anything is written. */
int usb_proto_validate_set(const usb_req_t *r, char *err, size_t errlen);
bool usb_proto_name_ok(const char *name);      /* [A-Za-z0-9_-]{1,31} */
bool usb_proto_bot_url_ok(const char *url);    /* "" or https://host[:port]/api/offer, <=127 bytes, printable, no space/quote/backslash */

/* Replies are "\nOH<{...}\n". Return the length written, or -1 if it didn't fit. */
int usb_proto_reply_info(const usb_info_t *in, long id, char *buf, size_t len);
int usb_proto_reply_ok(const char *cmd, long id, const char *const *saved, int n_saved,
                       char *buf, size_t len);
int usb_proto_reply_err(const char *cmd, long id, const char *error,
                        const char *const *saved, int n_saved, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* LENOXVI_USB_PROTO_H */
