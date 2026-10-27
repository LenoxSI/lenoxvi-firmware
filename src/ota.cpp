#include "main.h"
#include "cmd_json.h"
#include "boot_diag.h"
#include "rgb.h"
#include "settings.h"

#ifndef LINUX_BUILD

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

static const char *OTA_TAG = "ota";
static bool rollback_retry_allowed(const char *remote, uint8_t *spend);  /* defined below */
static bool rollback_retry_spend(const char *remote, uint8_t n);

/* Wait for the station to have an IP before any HTTP (2026-09-19): the boot
 * check fired ~1s after esp_wifi_start and lost the race — got-ip lands ~1s
 * later, so the version GET died ESP_ERR_HTTP_CONNECT every boot. The
 * WebRTC offer path never saw this because it starts from the got-ip event. */
static void wait_for_network(void) {
    for (int i = 0; i < 150; i++) {  /* up to 15s */
        esp_netif_t *n = NULL;
        while ((n = esp_netif_next(n)) != NULL) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(n, &ip) == ESP_OK &&
                ip.ip.addr != 0) {
                return;  /* some interface has an IP */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(OTA_TAG, "no IP after 15s — proceeding anyway");
}

#define OTA_CHECK_TIMEOUT_MS 5000
#define OTA_VERSION_BUF_SIZE 512
#define OTA_DOWNLOAD_BUF_SIZE 4096

static int parse_version_from_json(const char *json, char *version_out, size_t version_len,
                                    char *filename_out, size_t filename_len) {
    const char *vkey = "\"version\"";
    const char *vp = strstr(json, vkey);
    if (!vp) return -1;
    vp += strlen(vkey);
    while (*vp && (*vp == ':' || *vp == ' ' || *vp == '\t' || *vp == '\n')) vp++;
    if (*vp != '"') return -1;
    vp++;
    const char *vstart = vp;
    while (*vp && *vp != '"') vp++;
    if (*vp != '"') return -1;
    size_t vlen = vp - vstart;
    if (vlen >= version_len) vlen = version_len - 1;
    strncpy(version_out, vstart, vlen);
    version_out[vlen] = '\0';

    const char *fkey = "\"filename\"";
    const char *fp = strstr(json, fkey);
    if (!fp) { filename_out[0] = '\0'; return 0; }
    fp += strlen(fkey);
    while (*fp && (*fp == ':' || *fp == ' ' || *fp == '\t' || *fp == '\n')) fp++;
    if (*fp != '"') { filename_out[0] = '\0'; return 0; }
    fp++;
    const char *fstart = fp;
    while (*fp && *fp != '"') fp++;
    if (*fp != '"') { filename_out[0] = '\0'; return 0; }
    size_t flen = fp - fstart;
    if (flen >= filename_len) flen = filename_len - 1;
    strncpy(filename_out, fstart, flen);
    filename_out[flen] = '\0';
    return 0;
}

static int compare_versions(const char *local, const char *remote) {
    int la = 0, lb = 0, lc = 0;
    int ra = 0, rb = 0, rc = 0;
    sscanf(local, "%d.%d.%d", &la, &lb, &lc);
    sscanf(remote, "%d.%d.%d", &ra, &rb, &rc);
    if (la != ra) return la < ra ? -1 : 1;
    if (lb != rb) return lb < rb ? -1 : 1;
    if (lc != rc) return lc < rc ? -1 : 1;
    return 0;
}

/* OTA track (one name per firmware line, 2026-10-16): each line's
 * CMakeLists hard-codes XVF_OTA_TRACK (array | flex | waveshare), so an image only
 * ever reads /firmware/<its track>/version and can never pick up the other
 * line's manifest. Never env-driven, and no silent fallback: a build without
 * a track is a compile error. (The bot still serves the old track-less
 * /firmware/version for array images <= 1.2.8 — that URL is not built
 * into any new image.) */
#ifndef XVF_OTA_TRACK
#error "XVF_OTA_TRACK must be set by the firmware line's CMakeLists (array | flex | waveshare)"
#endif
#define FW_URL_FMT "%.*s/firmware/" XVF_OTA_TRACK "/%s"

static void build_firmware_url(char *out, size_t out_len, const char *suffix) {
    const char *base = settings_get_bot_url();   /* OTA follows the host address (1.2.47) */
    const char *api_off = strstr(base, "/api/offer");
    size_t prefix_len = api_off ? (size_t)(api_off - base) : strlen(base);
    if (prefix_len >= out_len) prefix_len = out_len - 1;
    snprintf(out, out_len, FW_URL_FMT, (int)prefix_len, base, suffix);
}

static esp_err_t version_http_event_handler(esp_http_client_event_t *evt) {
    static int output_len = 0;
    char *buf = (char *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (buf) {
            int copy = evt->data_len;
            if (output_len + copy >= OTA_VERSION_BUF_SIZE - 1)
                copy = OTA_VERSION_BUF_SIZE - 1 - output_len;
            if (copy > 0) {
                memcpy(buf + output_len, evt->data, copy);
                output_len += copy;
                buf[output_len] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ON_CONNECTED:
        output_len = 0;
        if (buf) buf[0] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

// Download and flash firmware using lower-level esp_ota_ops API (plain HTTP)

/* OTA-in-flight flag (2026-10-05): the 09:17–09:22 churn loop — five
 * reboots to land ONE flash — was the WebRTC DISCONNECTED handler's
 * esp_restart() firing mid-download: the 1.2MB pull starves the audio
 * task, the peer drops, and the handler rebooted the node before
 * esp_ota_write could finish. The OTA task owns the ONLY reboot during
 * a flash; webrtc.cpp's disconnect handler checks this flag and defers
 * instead of killing a flash in progress. */
static volatile bool s_ota_in_flight = false;
/* Set by webrtc.cpp when it SKIPPED a reboot because a flash was in flight.
 * If the flash then fails, nobody else will ever reboot the node — the
 * session is gone and the one-per-boot OTA check is spent — so the OTA
 * path owns that reboot (2026-10-15). */
static volatile bool s_drop_deferred = false;

bool pipecat_ota_busy(void) { return s_ota_in_flight; }
/* The same flag for C callers (usb_cmd.c): main.h declares pipecat_ota_busy
 * with C++ linkage. */
extern "C" bool pipecat_ota_busy_c(void) { return s_ota_in_flight; }

/* Last OTA outcome this boot, for get_info: every failure used to be
 * serial-only, so a node that refused or failed an update just sat there.
 * `last` is always a string literal (the tags below), `http` the last HTTP
 * status the check or download saw (0 = none yet), `at_s` the uptime when
 * the outcome was recorded. Written by the OTA task, read by the RTVI task:
 * a short critical section keeps the three fields consistent.
 *   none                 no check has finished yet
 *   up-to-date           the bot offers nothing newer
 *   check-failed         version GET failed (network/TLS) or its body was unusable
 *   bad-http             the check or the download got a non-200 (see http)
 *   wrong-board          the image is for another board line
 *   version-mismatch     the image's own version isn't the one the manifest offered
 *   refused-rolled-back  that version already rolled back too many times
 *   too-big              the image doesn't fit the OTA partition
 *   download-failed      connection or read error mid-download
 *   flash-failed         writing, validating or activating the image failed
 *   deferred             an attempt failed (often a starved session dropping);
 *                        the next attempt is scheduled
 *   updated              new image flashed, rebooting into it */
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static cmd_ota_status_t s_status = {"none", 0, 0};

/* http 0 = no new status code: keep the last one seen. */
static void ota_record(const char *tag, int http) {
    uint32_t now = boot_diag_up_s();
    taskENTER_CRITICAL(&s_status_mux);
    s_status.last = tag;
    if (http != 0) s_status.http = http;
    s_status.at_s = now;
    taskEXIT_CRITICAL(&s_status_mux);
}

extern "C" void pipecat_ota_status(cmd_ota_status_t *out) {
    taskENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_mux);
}
void pipecat_ota_defer_drop(void) { s_drop_deferred = true; }

static esp_err_t download_and_flash_ota_impl(const char *firmware_url, const char *remote_version,
                                             uint8_t rb_spend);

/* Download retry (2026-10-15): the check runs ONCE per boot, so one
 * transient failure (WiFi blip, bot restarting, a truncated read) used to
 * leave the node on the old image until its next reboot. Each attempt is
 * self-contained (esp_ota_abort releases the partition), so a retry is a
 * clean restart of the download. */
#define OTA_DOWNLOAD_ATTEMPTS 3
#define OTA_ERR_WRONG_BOARD ESP_ERR_NOT_SUPPORTED   /* fw M4 refusal: never retried */
/* Also never retried: the same file will be just as wrong next time. */
#define OTA_ERR_VERSION_MISMATCH ESP_ERR_INVALID_VERSION
#define OTA_ERR_TOO_BIG ESP_ERR_INVALID_SIZE
static const uint32_t k_ota_retry_delay_ms[OTA_DOWNLOAD_ATTEMPTS - 1] = {10000, 30000};

static esp_err_t download_and_flash_ota(const char *firmware_url, const char *remote_version,
                                        uint8_t rb_spend) {
    s_ota_in_flight = true;  // webrtc disconnect handler must NOT reboot now
    /* The ring shows UPDATING for the whole download + flash, retry waits
     * included: unplugging now throws the update away. */
    rgb_set_state(RGB_STATE_UPDATING);
    esp_err_t rc = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_DOWNLOAD_ATTEMPTS; attempt++) {
        rc = download_and_flash_ota_impl(firmware_url, remote_version, rb_spend);
        if (rc == ESP_OK || rc == OTA_ERR_WRONG_BOARD ||
            rc == OTA_ERR_VERSION_MISMATCH || rc == OTA_ERR_TOO_BIG) break;
        if (attempt < OTA_DOWNLOAD_ATTEMPTS) {
            uint32_t delay_ms = k_ota_retry_delay_ms[attempt - 1];
            ESP_LOGW(OTA_TAG, "OTA attempt %d/%d failed (%s) — retrying in %lu s",
                     attempt, OTA_DOWNLOAD_ATTEMPTS, esp_err_to_name(rc),
                     (unsigned long)(delay_ms / 1000));
            ota_record("deferred", 0);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    s_ota_in_flight = false;
    if (rc != ESP_OK) {
        ESP_LOGE(OTA_TAG, "OTA gave up after %d attempts (%s) — staying on the "
                 "running image", OTA_DOWNLOAD_ATTEMPTS, esp_err_to_name(rc));
        if (s_drop_deferred) {
            ESP_LOGE(OTA_TAG, "the session dropped during the OTA and its reboot "
                     "was deferred — rebooting now to reconnect");
            vTaskDelay(pdMS_TO_TICKS(500));  /* let the log line leave */
            boot_diag_restart("ota-failed-deferred");
        }
        /* The session is still up: back to connected-idle. UPDATING sticks
         * against everything else (rgb.c), so this clear is explicit. */
        rgb_set_state(RGB_STATE_BRIDGE_CONNECTED);
    }
    return rc;
}

/* fw M4 board identity (2026-10-16): an image names its own line in
 * esp_app_desc_t.project_name (CMake project(): array_esp32 / flex_esp32).
 * The incoming image's descriptor rides its first bytes — 24-byte image
 * header + 8-byte segment header, then magic at byte 32 and project_name at
 * byte 80 — and it must equal the RUNNING image's name. A mislaid manifest
 * or a hand-copied .bin therefore cannot cross-flash this board: the
 * partition write is aborted before esp_ota_set_boot_partition, which is the
 * only step that matters. Writing the inactive partition first is harmless. */
#define OTA_IDENT_BYTES 112
#define OTA_IDENT_MAGIC_OFF 32
#define OTA_IDENT_VERSION_OFF 48
#define OTA_IDENT_NAME_OFF 80

/* ESP_OK, OTA_ERR_WRONG_BOARD, or OTA_ERR_VERSION_MISMATCH. The version
 * check catches a manifest that names a newer version than the .bin it
 * points at: flashing it would boot the older image, see "update available"
 * again and reflash on every boot, passing the health gate each time. */
static esp_err_t incoming_image_is_ours(const uint8_t *hdr, const char *remote_version) {
    const uint8_t magic[4] = {0x32, 0x54, 0xCD, 0xAB};   /* ESP_APP_DESC_MAGIC_WORD, LE */
    const char *ours = esp_app_get_description()->project_name;
    char theirs[33] = {0};
    char their_version[33] = {0};
    if (memcmp(hdr + OTA_IDENT_MAGIC_OFF, magic, sizeof(magic)) != 0) {
        ESP_LOGE(OTA_TAG, "incoming image has no app descriptor — not flashing");
        return OTA_ERR_WRONG_BOARD;
    }
    memcpy(theirs, hdr + OTA_IDENT_NAME_OFF, 32);
    if (strncmp(theirs, ours, 32) != 0) {
        ESP_LOGE(OTA_TAG, "incoming image is '%s' but this board runs '%s' — wrong "
                 "firmware line, refusing it (cross-flash guard)", theirs, ours);
        return OTA_ERR_WRONG_BOARD;
    }
    memcpy(their_version, hdr + OTA_IDENT_VERSION_OFF, 32);
    if (strncmp(their_version, remote_version, 32) != 0) {
        ESP_LOGE(OTA_TAG, "incoming image is version '%s' but the manifest offered '%s' — "
                 "not flashing", their_version, remote_version);
        return OTA_ERR_VERSION_MISMATCH;
    }
    ESP_LOGI(OTA_TAG, "incoming image identity OK (%s %s)", theirs, their_version);
    return ESP_OK;
}

static esp_err_t download_and_flash_ota_impl(const char *firmware_url, const char *remote_version,
                                             uint8_t rb_spend) {
    /* The TLS read path needs internal DMA-capable RAM (hardware AES bounce
     * buffer). Log it on every attempt so a RAM squeeze shows up as numbers,
     * not as "esp-aes: Failed to allocate memory" (2026-10-15). */
    ESP_LOGI(OTA_TAG, "internal DMA RAM: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    esp_http_client_config_t config = {};
    config.url = firmware_url;
    config.timeout_ms = 30000;
    /* keep_alive OFF for the 1.2MB pull (2026-09-19): with it on, the first
     * attempt died ~4s in with ESP_ERR_HTTP_CONNECT — the version-check
     * connection had gone stale and the long download needs its own socket.
     * Fresh connection, no reuse. */
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(OTA_TAG, "Failed to init HTTP client");
        ota_record("download-failed", 0);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        ota_record("download-failed", 0);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* A 404/500 body used to be streamed into the OTA partition until
         * esp_ota_write rejected the magic byte — fail cleanly instead. */
        ESP_LOGE(OTA_TAG, "Firmware download HTTP %d — not flashing", status);
        ota_record("bad-http", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length <= 0) {
        ESP_LOGE(OTA_TAG, "No content-length header");
        ota_record("download-failed", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ESP_LOGI(OTA_TAG, "Firmware size: %d bytes", content_length);

    // Get the next inactive OTA partition
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(OTA_TAG, "No OTA partition available");
        ota_record("flash-failed", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    /* Refuse before esp_ota_begin erases anything: an oversized image would
     * otherwise erase and write the whole slot before esp_ota_write says no. */
    if ((uint32_t)content_length > update_partition->size) {
        ESP_LOGE(OTA_TAG, "image is %d bytes but partition %s holds %lu — not flashing",
                 content_length, update_partition->label, (unsigned long)update_partition->size);
        ota_record("too-big", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return OTA_ERR_TOO_BIG;
    }
    ESP_LOGI(OTA_TAG, "Writing to partition %s at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_record("flash-failed", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    char *buf = (char *)malloc(OTA_DOWNLOAD_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(OTA_TAG, "Failed to allocate download buffer");
        ota_record("download-failed", status);
        esp_ota_abort(update_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int next_log = content_length / 10;  /* progress line every 10% */
    bool error = false;
    esp_err_t refused = ESP_OK;
    uint8_t ident[OTA_IDENT_BYTES];
    int ident_len = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buf, OTA_DOWNLOAD_BUF_SIZE);
        if (read_len <= 0) {
            ESP_LOGE(OTA_TAG, "HTTP read error at %d/%d", total_read, content_length);
            ota_record("download-failed", status);
            error = true;
            break;
        }
        if (ident_len < OTA_IDENT_BYTES) {   /* M4: collect the header, check once */
            int take = read_len < OTA_IDENT_BYTES - ident_len ? read_len : OTA_IDENT_BYTES - ident_len;
            memcpy(ident + ident_len, buf, take);
            ident_len += take;
            if (ident_len == OTA_IDENT_BYTES) {
                refused = incoming_image_is_ours(ident, remote_version);
                if (refused != ESP_OK) {
                    ota_record(refused == OTA_ERR_VERSION_MISMATCH ? "version-mismatch"
                                                                   : "wrong-board", status);
                    error = true;
                    break;
                }
            }
        }
        err = esp_ota_write(update_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(OTA_TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            ota_record("flash-failed", status);
            error = true;
            break;
        }
        total_read += read_len;
        /* TLS reads come in arbitrary sizes, so an exact-boundary test
         * (total % 64 KB == 0) almost never printed. */
        if (total_read >= next_log) {
            ESP_LOGI(OTA_TAG, "Progress: %d/%d (%d%%)", total_read, content_length,
                     (int)((int64_t)total_read * 100 / content_length));
            next_log += content_length / 10;
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (refused != ESP_OK) {
        esp_ota_abort(update_handle);
        return refused;
    }
    if (error || total_read != content_length || ident_len < OTA_IDENT_BYTES) {
        ESP_LOGE(OTA_TAG, "Download incomplete: %d/%d", total_read, content_length);
        if (!error) ota_record("download-failed", status);
        esp_ota_abort(update_handle);
        return ESP_FAIL;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        /* 1.3.0: VALIDATE_FAILED covers "not signed by the key this image was
         * signed with" (signed updates) as well as a corrupt image. Either way
         * the slot never becomes bootable; the node keeps running. */
        ESP_LOGE(OTA_TAG, "esp_ota_end failed: %s%s", esp_err_to_name(err),
                 err == ESP_ERR_OTA_VALIDATE_FAILED ? " (image refused: bad or missing signature, or corrupt)" : "");
        ota_record(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image-refused" : "flash-failed", status);
        return err;
    }

    /* The rollback retry is spent only now that a complete, valid image is
     * in the slot: a download that dies on the network costs nothing. If the
     * count can't be saved, don't switch (a bad image could loop forever). */
    if (rb_spend != 0 && !rollback_retry_spend(remote_version, rb_spend)) {
        ota_record("flash-failed", status);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_record("flash-failed", status);
        return err;
    }

    ESP_LOGI(OTA_TAG, "OTA write complete, booting from %s", update_partition->label);
    return ESP_OK;
}

void pipecat_check_ota_update(void) {
    /* a still-unvalidated image never checks for updates — the
     * write would be refused anyway (esp_ota_begin returns
     * ROLLBACK_INVALID_STATE while PENDING_VERIFY), and the rollback-retry
     * guard must not spend a retry on a download that can't happen. The
     * next boot (validated) checks normally. */
    if (pipecat_ota_pending_verify()) {
        ESP_LOGW(OTA_TAG, "skipping OTA check: running image still pending verify");
        return;
    }
    /* EMPIRICAL BISECT (2026-09-19): this exact check+download+flash ran
     * fine at CONNECTED in build 47 (version GET 200, 1.0.1->1.0.2,
     * download started). It crashes at got-ip time (StoreProhibited in
     * ROM memcpy during TLS setup, deterministic). So: CONNECTED only.
     * If a live esp_ota_write still crashes, the staging architecture
     * (download now, flash pre-WiFi next boot) is the fallback. */
    wait_for_network();
    char version_url[256];
    build_firmware_url(version_url, sizeof(version_url), "version");

    ESP_LOGI(OTA_TAG, "Checking for firmware update at %s", version_url);

    char response_buf[OTA_VERSION_BUF_SIZE] = {0};

    esp_http_client_config_t config = {};
    config.url = version_url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = OTA_CHECK_TIMEOUT_MS;
    config.event_handler = version_http_event_handler;
    config.user_data = response_buf;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(OTA_TAG, "Failed to init HTTP client for version check");
        ota_record("check-failed", 0);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(OTA_TAG, "Version check failed: %s", esp_err_to_name(err));
        ota_record("check-failed", 0);
        esp_http_client_cleanup(client);
        return;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(OTA_TAG, "Version check returned HTTP %d", status);
        ota_record("bad-http", status);
        return;
    }

    if (strlen(response_buf) == 0) {
        ESP_LOGW(OTA_TAG, "Version check: empty response");
        ota_record("check-failed", status);
        return;
    }

    char remote_version[32] = {0};
    char remote_filename[128] = {0};

    if (parse_version_from_json(response_buf, remote_version, sizeof(remote_version),
                                remote_filename, sizeof(remote_filename)) != 0) {
        ESP_LOGW(OTA_TAG, "Could not parse JSON: %s", response_buf);
        ota_record("check-failed", status);
        return;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *local_version = app_desc->version;
    ESP_LOGI(OTA_TAG, "Local version: %s, remote version: %s", local_version, remote_version);

    if (compare_versions(local_version, remote_version) >= 0) {
        ESP_LOGI(OTA_TAG, "Firmware is up to date");
        ota_record("up-to-date", status);
        return;
    }

    ESP_LOGI(OTA_TAG, "Update available: %s -> %s", local_version, remote_version);

    /* never reflash-loop a version the bootloader already rolled
     * back from (bounded retries, then refuse). */
    uint8_t rb_spend = 0;
    if (!rollback_retry_allowed(remote_version, &rb_spend)) {
        ota_record("refused-rolled-back", status);
        return;
    }

    char firmware_url[256];
    if (strlen(remote_filename) > 0) {
        build_firmware_url(firmware_url, sizeof(firmware_url), remote_filename);
    } else {
        build_firmware_url(firmware_url, sizeof(firmware_url), "latest");
    }

    ESP_LOGI(OTA_TAG, "Downloading firmware from %s", firmware_url);

    err = download_and_flash_ota(firmware_url, remote_version, rb_spend);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "OTA update failed: %s", esp_err_to_name(err));
        return;
    }

    ota_record("updated", 200);
    ESP_LOGI(OTA_TAG, "OTA update successful, rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    boot_diag_restart("ota-success");
}

/* ── OTA health gate + rollback (H1, 2026-10-13) ────────
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the BOOTLOADER boots a fresh
 * OTA image in ESP_OTA_IMG_PENDING_VERIFY. If that image reboots (crash,
 * panic loop, watchdog, any esp_restart) before it marks itself valid, the
 * bootloader rolls back to the previous image — a bad-but-bootable image
 * no longer needs a USB rescue.
 *
 * The old code marked the image valid at BOOT, before WiFi, which made
 * rollback meaningless (every image validated itself in milliseconds).
 * Now validity has to be EARNED: the bot must talk back over the data
 * channel (cmd_note_activity -> pipecat_ota_health_ok). That proves WiFi,
 * the offer POST, DTLS/SCTP, the DCEP and the RTVI dispatch — the whole
 * path an image needs to be useful. webrtc.cpp sends one liveness probe
 * on DC open while pending so a quiet house still produces the pong.
 *
 * Landmine (2026-09-19): marking valid after got-ip once double-exception
 * crash-looped the node — that write ran in the WiFi event handler on the
 * 2304-byte sys_evt stack (fw review M2). The gate's flash write runs in
 * its OWN task with a static internal-RAM stack (.bss is internal on this
 * build), the same shape as the ota_check task whose esp_ota_write path
 * has flashed with WiFi up since 1.1.3.
 *
 * No gate within OTA_HEALTH_TIMEOUT_MS -> explicit rollback + reboot.
 * Bootloaders WITHOUT rollback (a node not yet USB-flashed) never set
 * PENDING_VERIFY, so the gate simply never arms: same behavior as before.
 */
#define OTA_HEALTH_TIMEOUT_MS   (5 * 60 * 1000)
/* A rolled-back version gets this many re-attempts before the node refuses
 * it: a GOOD image can fail the gate for bot-side reasons (bot down or
 * restarting during the window), so one rollback must not blacklist it
 * forever — but a truly bad image must not reflash-loop the flash to death
 * (rollback -> old image OTAs the "newer" version again -> rollback...). */
#define OTA_ROLLBACK_MAX_RETRIES 2
#define OTA_NVS_NAMESPACE "tknode"   /* settings.cpp's namespace */
#define OTA_NVS_KEY_RB_VER "ota_rb_ver"
#define OTA_NVS_KEY_RB_N   "ota_rb_n"

static volatile bool s_pending_verify = false;
static TaskHandle_t s_health_task = NULL;
/* The version the bootloader rolled back from, read once at boot. A later
 * download overwrites that slot from its first sector, so the slot header
 * is only trustworthy before any download starts; when an earlier partial
 * download already erased it, the name recorded in NVS stands in. */
static char s_rejected[32] = {0};
static bool s_rejected_from_slot = false;

static void rejected_version_load(void) {
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad == NULL) return;  /* no rolled-back image in the other slot */
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        strlcpy(s_rejected, d.version, sizeof(s_rejected));
        s_rejected_from_slot = true;
        return;
    }
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_rejected);
    if (nvs_get_str(h, OTA_NVS_KEY_RB_VER, s_rejected, &len) != ESP_OK) s_rejected[0] = '\0';
    nvs_close(h);
}

static void ota_health_task(void *arg) {
    uint32_t ok = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OTA_HEALTH_TIMEOUT_MS));
    if (ok) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_pending_verify = false;
            ESP_LOGI(OTA_TAG, "health gate PASSED (bot round-trip) — image marked VALID");
        } else {
            /* Stay pending: the next reboot rolls back, which is the safe
             * direction for an image that can't even write otadata. */
            ESP_LOGE(OTA_TAG, "health gate passed but mark-valid failed: %s — "
                     "next reboot rolls back", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(OTA_TAG, "health gate TIMED OUT (%d s, no bot round-trip) — "
                 "rolling back to the previous image", OTA_HEALTH_TIMEOUT_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(200));  /* let the log line leave */
        boot_diag_mark("ota-health-timeout");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();  /* not reached on success; a plain reboot also rolls back */
    }
    s_health_task = NULL;
    vTaskDelete(NULL);
}

void pipecat_ota_health_begin(void) {
    /* Boot-time: READ-ONLY (state query + task spawn) — no flash write
     * here, so the pre-WiFi call site stays exactly as safe as before. */
    rejected_version_load();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  /* validated image, or a bootloader without rollback */
    }
    s_pending_verify = true;
    ESP_LOGW(OTA_TAG, "fresh OTA image PENDING_VERIFY — health gate armed (%d s): "
             "a bot round-trip over the data channel marks it valid; any reboot "
             "before that rolls back", OTA_HEALTH_TIMEOUT_MS / 1000);
    static StaticTask_t health_buf;
    static StackType_t health_stack[4096 / sizeof(StackType_t)];
    s_health_task = xTaskCreateStaticPinnedToCore(
        ota_health_task, "ota_health", 4096 / sizeof(StackType_t), NULL, 4,
        health_stack, &health_buf, 0);
}

bool pipecat_ota_pending_verify(void) { return s_pending_verify; }

void pipecat_ota_health_ok(void) {
    TaskHandle_t t = s_health_task;
    if (s_pending_verify && t != NULL) xTaskNotifyGive(t);
}

bool pipecat_ota_rejected_version(char *out, size_t out_len) {
    /* The version the bootloader rolled back FROM, if the other slot holds
     * one (reported in fw_push so the bot can surface a silent rollback). */
    if (s_rejected[0] == '\0') return false;
    strncpy(out, s_rejected, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

/* true = OK to download `remote`; false = refuse (rolled back too often).
 * Read-mostly: *spend is the retry count to save once the image is flashed
 * (0 = remote isn't the rolled-back version, nothing to count). The only
 * write here records the rolled-back version's name the first time it is
 * seen, before any download can erase the slot header that holds it. */
static bool rollback_retry_allowed(const char *remote, uint8_t *spend) {
    *spend = 0;
    if (s_rejected[0] == '\0') return true;  /* nothing rolled back */
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        if (strcmp(s_rejected, remote) != 0) return true;
        ESP_LOGW(OTA_TAG, "rollback guard: NVS unavailable — refusing %s (fail safe)", remote);
        return false;
    }
    char seen[32] = {0};
    size_t len = sizeof(seen);
    uint8_t n = 0;
    if (nvs_get_str(h, OTA_NVS_KEY_RB_VER, seen, &len) == ESP_OK &&
        strcmp(seen, s_rejected) == 0) {
        nvs_get_u8(h, OTA_NVS_KEY_RB_N, &n);
    } else if (s_rejected_from_slot) {
        /* First sighting of this rollback: a fresh count for it. */
        nvs_set_str(h, OTA_NVS_KEY_RB_VER, s_rejected);
        nvs_set_u8(h, OTA_NVS_KEY_RB_N, 0);
        nvs_commit(h);
    }
    nvs_close(h);
    if (strcmp(s_rejected, remote) != 0) {
        return true;  /* remote isn't the version that rolled back */
    }
    bool allowed = n < OTA_ROLLBACK_MAX_RETRIES;
    if (allowed) {
        *spend = (uint8_t)(n + 1);
        ESP_LOGW(OTA_TAG, "%s was rolled back before — retry %u/%u (the gate can fail "
                 "for bot-side reasons)", remote, n + 1, OTA_ROLLBACK_MAX_RETRIES);
    } else {
        ESP_LOGE(OTA_TAG, "refusing %s: rolled back %u times — ship a NEW version "
                 "(or USB) to move this node", remote, OTA_ROLLBACK_MAX_RETRIES + 1);
    }
    return allowed;
}

/* Save retry `n` for `remote`. Runs on the ota_check task (internal-RAM
 * stack) after esp_ota_end, so the NVS write is safe. */
static bool rollback_retry_spend(const char *remote, uint8_t n) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(OTA_TAG, "rollback guard: NVS unavailable — not switching to %s", remote);
        return false;
    }
    esp_err_t err = nvs_set_str(h, OTA_NVS_KEY_RB_VER, remote);
    if (err == ESP_OK) err = nvs_set_u8(h, OTA_NVS_KEY_RB_N, n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "rollback guard: saving retry %u failed (%s) — not switching to %s",
                 n, esp_err_to_name(err), remote);
        return false;
    }
    return true;
}

#endif // LINUX_BUILD