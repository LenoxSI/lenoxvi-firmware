/* tests/test_usb_proto.c — run: IDF_PATH=~/esp-idf bash tests/run_usb_proto_test.sh */
#include "usb_proto.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(name, cond) do { if (cond) printf("[PASS] %s\n", name); \
  else { printf("[FAIL] %s\n", name); fails++; } } while (0)

static char got[8][USB_LINE_MAX + 1]; static int n_got, n_over;
static void on_line(const char *l, void *c) { (void)c; if (n_got < 8) strcpy(got[n_got++], l); }
static void on_over(void *c) { (void)c; n_over++; }

int main(void) {
  usb_req_t r; char err[96];

  CHECK("log text is not a request", !usb_proto_parse("I (123) wifi: connected", &r, err, sizeof err));
  CHECK("info with id parses",
        usb_proto_parse("OH>{\"id\":7,\"cmd\":\"info\"}", &r, err, sizeof err) && !err[0]
        && r.cmd == USB_CMD_INFO && r.id == 7);
  CHECK("info without id has id -1",
        usb_proto_parse("OH>{\"cmd\":\"info\"}", &r, err, sizeof err) && !err[0] && r.id == -1);
  CHECK("bad json gets an error reply",
        usb_proto_parse("OH>{nope", &r, err, sizeof err) && strcmp(err, "bad json") == 0);
  CHECK("unknown cmd gets an error reply",
        usb_proto_parse("OH>{\"cmd\":\"fly\"}", &r, err, sizeof err) && strcmp(err, "unknown cmd") == 0);

  /* rules */
  CHECK("name rules", usb_proto_name_ok("Kitchen_2") && usb_proto_name_ok("a")
        && !usb_proto_name_ok("") && !usb_proto_name_ok("Kit chen") && !usb_proto_name_ok("Küche")
        && !usb_proto_name_ok("abcdefghijklmnopqrstuvwxyz012345")   /* 32 chars */);
  CHECK("bot_url accepts https + /api/offer and the empty clear",
        usb_proto_bot_url_ok("https://192.0.2.10:7860/api/offer") && usb_proto_bot_url_ok(""));
  CHECK("bot_url refuses http, a wrong path, no host, spaces, quotes",
        !usb_proto_bot_url_ok("http://192.0.2.10:7860/api/offer")
        && !usb_proto_bot_url_ok("https://192.0.2.10:7860/")
        && !usb_proto_bot_url_ok("https:///api/offer")
        && !usb_proto_bot_url_ok("https://a b/api/offer")
        && !usb_proto_bot_url_ok("https://a\"b/api/offer"));
  char long_url[160]; strcpy(long_url, "https://");
  memset(long_url + 8, 'a', 110); strcpy(long_url + 118, "/api/offer");          /* 128 bytes */
  CHECK("bot_url over 127 bytes is refused", !usb_proto_bot_url_ok(long_url));

  usb_proto_parse("OH>{\"cmd\":\"set\",\"wifi_ssid\":\"Maple\"}", &r, err, sizeof err);
  CHECK("ssid without pass is refused", err[0] || usb_proto_validate_set(&r, err, sizeof err) != 0);
  usb_proto_parse("OH>{\"cmd\":\"set\",\"wifi_ssid\":\"123456789012345678901234567890123\",\"wifi_pass\":\"x\"}",
                  &r, err, sizeof err);
  CHECK("33-byte ssid is refused", err[0] || usb_proto_validate_set(&r, err, sizeof err) != 0);
  usb_proto_parse("OH>{\"cmd\":\"set\",\"wifi_ssid\":\"Maple\",\"wifi_pass\":"
                  "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}", &r, err, sizeof err);
  CHECK("64 hex digits is a valid raw key", !err[0] && usb_proto_validate_set(&r, err, sizeof err) == 0);
  usb_proto_parse("OH>{\"cmd\":\"set\",\"wifi_ssid\":\"Maple\",\"wifi_pass\":"
                  "\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}", &r, err, sizeof err);
  CHECK("64 non-hex chars is refused", err[0] || usb_proto_validate_set(&r, err, sizeof err) != 0);
  usb_proto_parse("OH>{\"cmd\":\"set\",\"name\":\"Kitchen\",\"bot_url\":\"http://x/api/offer\"}", &r, err, sizeof err);
  CHECK("one bad field fails the whole set, naming the field",
        (err[0] || usb_proto_validate_set(&r, err, sizeof err) != 0) && strncmp(err, "bot_url:", 8) == 0);
  usb_proto_parse("OH>{\"cmd\":\"set\",\"name\":7}", &r, err, sizeof err);
  CHECK("a non-string field is refused", err[0] && strncmp(err, "name:", 5) == 0);

  /* replies */
  usb_info_t in = { .track = "waveshare", .fw = "1.2.47", .name = "Kitchen", .wifi_ssid = "Ma\"ple",
                    .bot_url = "https://192.0.2.10:7860/api/offer", .link = "wifi_up", .ip = "192.0.2.50",
                    .mac = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, .bot_url_saved = true, .up_s = 42 };
  char buf[USB_LINE_MAX + 8];
  int n = usb_proto_reply_info(&in, 7, buf, sizeof buf);
  CHECK("info reply is framed \\nOH<{...}\\n", n > 0 && strncmp(buf, "\nOH<{", 5) == 0 && buf[n - 1] == '\n'
        && memchr(buf + 1, '\n', (size_t)n - 2) == NULL);
  CHECK("info reply echoes id, proto, escapes quotes, formats the mac",
        strstr(buf, "\"id\":7") && strstr(buf, "\"proto\":1") && strstr(buf, "Ma\\\"ple")
        && strstr(buf, "aa:bb:cc:dd:ee:ff"));
  CHECK("info reply has no password field", strstr(buf, "pass") == NULL);
  CHECK("a reply that doesn't fit returns -1", usb_proto_reply_info(&in, 7, buf, 16) == -1);
  const char *saved[] = {"bot_url"};
  n = usb_proto_reply_err("set", 3, "name: save failed", saved, 1, buf, sizeof buf);
  CHECK("error reply carries error + the partial saved list",
        n > 0 && strstr(buf, "\"ok\":false") && strstr(buf, "name: save failed") && strstr(buf, "[\"bot_url\"]"));

  /* line assembler */
  usb_line_t st; usb_line_init(&st); n_got = n_over = 0;
  const char *feed = "OH>{\"cmd\":\"in";
  usb_line_feed(&st, (const uint8_t *)feed, strlen(feed), on_line, on_over, NULL);
  feed = "fo\"}\r\nI (1) x\n";
  usb_line_feed(&st, (const uint8_t *)feed, strlen(feed), on_line, on_over, NULL);
  CHECK("split + CRLF lines reassemble", n_got == 2 && strcmp(got[0], "OH>{\"cmd\":\"info\"}") == 0);
  char big[USB_LINE_MAX + 40]; memset(big, 'x', sizeof big);
  usb_line_feed(&st, (const uint8_t *)big, sizeof big, on_line, on_over, NULL);
  usb_line_feed(&st, (const uint8_t *)"tail\nOH>{}\n", 11, on_line, on_over, NULL);
  CHECK("overlong line: one overflow, dropped through its newline, then recovers",
        n_over == 1 && n_got == 3 && strcmp(got[2], "OH>{}") == 0);

  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
