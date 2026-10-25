/* USB setup listener: protocol v1 (usb_proto.h) on the board's own USB port.
 * Started before Wi-Fi so a node stuck on a bad network still answers.
 * NEVER log a received line or any set field: the line carries the password. */
#include "usb_cmd.h"
#include "usb_proto.h"
#include "link_state.h"
#include "settings.h"
#include "cmd.h"
#include "boot_diag.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

extern bool pipecat_ota_busy_c(void);   /* ota.cpp */
static const char *TAG = "usb_cmd";
static StaticTask_t s_tcb;
static StackType_t s_stack[4096 / sizeof(StackType_t)];   /* static = internal DRAM */
static char s_out[USB_LINE_MAX + 8];
static usb_req_t s_req;

static void send(int n) { if (n > 0) usb_serial_jtag_write_bytes(s_out, (size_t)n, pdMS_TO_TICKS(50)); }
static const char *cmd_name(usb_cmd_t c) {
  return c == USB_CMD_INFO ? "info" : c == USB_CMD_SET ? "set" : c == USB_CMD_RESTART ? "restart" : "";
}

static void do_info(long id) {
  static char ip[16];
  ip[0] = '\0';
  link_state_t ls = link_state_get();
  if (ls >= LINK_WIFI_UP) {
    esp_netif_ip_info_t ii;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ii) == ESP_OK) snprintf(ip, sizeof ip, IPSTR, IP2STR(&ii.ip));
  }
  usb_info_t in = { .track = XVF_OTA_TRACK, .fw = esp_app_get_description()->version,
                    .name = settings_get_node_name(), .wifi_ssid = settings_get_wifi_ssid(),
                    .bot_url = settings_get_bot_url(), .link = link_state_name(ls), .ip = ip,
                    .bot_url_saved = settings_bot_url_saved(),
                    .up_s = (uint32_t)(esp_timer_get_time() / 1000000) };
  esp_read_mac(in.mac, ESP_MAC_WIFI_STA);
  send(usb_proto_reply_info(&in, id, s_out, sizeof s_out));
}

static void do_set(long id) {
  char err[96]; const char *saved[3]; int k = 0;
  if (pipecat_ota_busy_c()) { send(usb_proto_reply_err("set", id, "ota in progress", NULL, 0, s_out, sizeof s_out)); goto wipe; }
  if (usb_proto_validate_set(&s_req, err, sizeof err)) { send(usb_proto_reply_err("set", id, err, NULL, 0, s_out, sizeof s_out)); goto wipe; }
  if (s_req.has_bot_url) {
    if (pipecat_nvs_run(NVS_JOB_BOT_URL, s_req.bot_url, NULL)) { send(usb_proto_reply_err("set", id, "bot_url: save failed", saved, k, s_out, sizeof s_out)); goto wipe; }
    saved[k++] = "bot_url";
  }
  if (s_req.has_name) {
    if (pipecat_nvs_run(NVS_JOB_NAME, s_req.name, NULL)) { send(usb_proto_reply_err("set", id, "name: save failed", saved, k, s_out, sizeof s_out)); goto wipe; }
    saved[k++] = "name";
  }
  if (s_req.has_wifi) {
    if (pipecat_nvs_run(NVS_JOB_WIFI_DIRECT, s_req.wifi_ssid, s_req.wifi_pass)) { send(usb_proto_reply_err("set", id, "wifi: save failed", saved, k, s_out, sizeof s_out)); goto wipe; }
    saved[k++] = "wifi";
  }
  send(usb_proto_reply_ok("set", id, saved, k, s_out, sizeof s_out));
wipe:
  memset(s_req.wifi_pass, 0, sizeof s_req.wifi_pass);   /* don't keep the password around */
}

static void on_line(const char *line, void *ctx) {
  (void)ctx;
  char err[96];
  if (!usb_proto_parse(line, &s_req, err, sizeof err)) return;   /* not ours: ignore, never log */
  if (err[0]) {
    memset(s_req.wifi_pass, 0, sizeof s_req.wifi_pass);
    send(usb_proto_reply_err(cmd_name(s_req.cmd), s_req.id, err, NULL, 0, s_out, sizeof s_out));
    return;
  }
  switch (s_req.cmd) {
    case USB_CMD_INFO: do_info(s_req.id); break;
    case USB_CMD_SET: do_set(s_req.id); break;
    case USB_CMD_RESTART:
      if (pipecat_ota_busy_c()) { send(usb_proto_reply_err("restart", s_req.id, "ota in progress", NULL, 0, s_out, sizeof s_out)); break; }
      send(usb_proto_reply_ok("restart", s_req.id, NULL, 0, s_out, sizeof s_out));
      vTaskDelay(pdMS_TO_TICKS(150));                          /* let the ack leave the wire */
      boot_diag_restart("usb-restart");
      break;
    default: break;
  }
}
static void on_overflow(void *ctx) {
  (void)ctx;
  send(usb_proto_reply_err("", -1, "line too long", NULL, 0, s_out, sizeof s_out));
}

static void usb_task(void *arg) {
  (void)arg;
  static usb_line_t st;
  usb_line_init(&st);
  uint8_t buf[64];
  for (;;) {
    /* Bounded wait: with no computer attached this just times out and loops. */
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(1000));
    if (n > 0) usb_line_feed(&st, buf, (size_t)n, on_line, on_overflow, NULL);
  }
}

void usb_cmd_start(void) {
  /* tx holds one whole framed reply (up to USB_LINE_MAX + 2 bytes): the ring
   * buffer refuses a single write larger than itself. */
  usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 1024, .rx_buffer_size = 512 };
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "USB driver install failed: USB setup unavailable this boot");
    return;                                                    /* the node runs normally without it */
  }
  usb_serial_jtag_vfs_use_driver();                            /* console writes through the driver: no FIFO contention */
  if (!xTaskCreateStaticPinnedToCore(usb_task, "usb_cmd", sizeof s_stack / sizeof s_stack[0], NULL,
                                     3, s_stack, &s_tcb, 0)) {
    ESP_LOGE(TAG, "usb_cmd task create failed");
  }
}
