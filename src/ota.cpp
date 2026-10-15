#include "main.h"

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
static bool rollback_retry_allowed(const char *remote);  /* defined below */

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

/* OTA track key (design note, 2026-10-07): the bot serves per-model
 * manifests. The array node (XVF3800 mic-array board, 1.1.x) keeps the
 * legacy shared manifest URL — the DEPLOYED 1.1.3 fleet requests
 * /firmware/version and the server serves it; an unset track here
 * must stay byte-identical to that. The flex node (reSpeaker Flex,
 * 1.0.x track) builds with -DXVF_OTA_TRACK="flex" and reads
 * /firmware/flex/version — a bump on one track can NEVER cross-flash
 * the other. */
#ifdef XVF_OTA_TRACK
#define FW_URL_FMT "%.*s/firmware/" XVF_OTA_TRACK "/%s"
#else
#define FW_URL_FMT "%.*s/firmware/%s"
#endif

static void build_firmware_url(char *out, size_t out_len, const char *suffix) {
    const char *base = PIPECAT_SMALLWEBRTC_URL;
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
void pipecat_ota_defer_drop(void) { s_drop_deferred = true; }

static esp_err_t download_and_flash_ota_impl(const char *firmware_url);

/* Download retry (2026-10-15): the check runs ONCE per boot, so one
 * transient failure (WiFi blip, bot restarting, a truncated read) used to
 * leave the node on the old image until its next reboot. Each attempt is
 * self-contained (esp_ota_abort releases the partition), so a retry is a
 * clean restart of the download. */
#define OTA_DOWNLOAD_ATTEMPTS 3
static const uint32_t k_ota_retry_delay_ms[OTA_DOWNLOAD_ATTEMPTS - 1] = {10000, 30000};

static esp_err_t download_and_flash_ota(const char *firmware_url) {
    s_ota_in_flight = true;  // webrtc disconnect handler must NOT reboot now
    esp_err_t rc = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_DOWNLOAD_ATTEMPTS; attempt++) {
        rc = download_and_flash_ota_impl(firmware_url);
        if (rc == ESP_OK) break;
        if (attempt < OTA_DOWNLOAD_ATTEMPTS) {
            uint32_t delay_ms = k_ota_retry_delay_ms[attempt - 1];
            ESP_LOGW(OTA_TAG, "OTA attempt %d/%d failed (%s) — retrying in %lu s",
                     attempt, OTA_DOWNLOAD_ATTEMPTS, esp_err_to_name(rc),
                     (unsigned long)(delay_ms / 1000));
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
            esp_restart();
        }
    }
    return rc;
}

static esp_err_t download_and_flash_ota_impl(const char *firmware_url) {
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
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* A 404/500 body used to be streamed into the OTA partition until
         * esp_ota_write rejected the magic byte — fail cleanly instead. */
        ESP_LOGE(OTA_TAG, "Firmware download HTTP %d — not flashing", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length <= 0) {
        ESP_LOGE(OTA_TAG, "No content-length header");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ESP_LOGI(OTA_TAG, "Firmware size: %d bytes", content_length);

    // Get the next inactive OTA partition
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(OTA_TAG, "No OTA partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(OTA_TAG, "Writing to partition %s at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    char *buf = (char *)malloc(OTA_DOWNLOAD_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(OTA_TAG, "Failed to allocate download buffer");
        esp_ota_abort(update_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    bool error = false;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buf, OTA_DOWNLOAD_BUF_SIZE);
        if (read_len <= 0) {
            ESP_LOGE(OTA_TAG, "HTTP read error at %d/%d", total_read, content_length);
            error = true;
            break;
        }
        err = esp_ota_write(update_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(OTA_TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            error = true;
            break;
        }
        total_read += read_len;
        if (total_read % (64 * 1024) == 0) {
            ESP_LOGI(OTA_TAG, "Progress: %d/%d (%d%%)", total_read, content_length,
                     total_read * 100 / content_length);
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (error || total_read != content_length) {
        ESP_LOGE(OTA_TAG, "Download incomplete: %d/%d", total_read, content_length);
        esp_ota_abort(update_handle);
        return ESP_FAIL;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
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
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(OTA_TAG, "Version check failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(OTA_TAG, "Version check returned HTTP %d", status);
        return;
    }

    if (strlen(response_buf) == 0) {
        ESP_LOGW(OTA_TAG, "Version check: empty response");
        return;
    }

    char remote_version[32] = {0};
    char remote_filename[128] = {0};

    if (parse_version_from_json(response_buf, remote_version, sizeof(remote_version),
                                remote_filename, sizeof(remote_filename)) != 0) {
        ESP_LOGW(OTA_TAG, "Could not parse JSON: %s", response_buf);
        return;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *local_version = app_desc->version;
    ESP_LOGI(OTA_TAG, "Local version: %s, remote version: %s", local_version, remote_version);

    if (compare_versions(local_version, remote_version) >= 0) {
        ESP_LOGI(OTA_TAG, "Firmware is up to date");
        return;
    }

    ESP_LOGI(OTA_TAG, "Update available: %s -> %s", local_version, remote_version);

    /* never reflash-loop a version the bootloader already rolled
     * back from (bounded retries, then refuse). */
    if (!rollback_retry_allowed(remote_version)) {
        return;
    }

    char firmware_url[256];
    if (strlen(remote_filename) > 0) {
        build_firmware_url(firmware_url, sizeof(firmware_url), remote_filename);
    } else {
        build_firmware_url(firmware_url, sizeof(firmware_url), "latest");
    }

    ESP_LOGI(OTA_TAG, "Downloading firmware from %s", firmware_url);

    err = download_and_flash_ota(firmware_url);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "OTA update failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(OTA_TAG, "OTA update successful, rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* ── OTA health gate + rollback (design note H1, 2026-10-13) ────────
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
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();  /* not reached on success; a plain reboot also rolls back */
    }
    s_health_task = NULL;
    vTaskDelete(NULL);
}

void pipecat_ota_health_begin(void) {
    /* Boot-time: READ-ONLY (state query + task spawn) — no flash write
     * here, so the pre-WiFi call site stays exactly as safe as before. */
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
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad == NULL || esp_ota_get_partition_description(bad, &d) != ESP_OK) return false;
    strncpy(out, d.version, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

/* true = OK to download `remote`; false = refuse (rolled back too often). */
static bool rollback_retry_allowed(const char *remote) {
    char bad_ver[32];
    if (!pipecat_ota_rejected_version(bad_ver, sizeof(bad_ver)) ||
        strcmp(bad_ver, remote) != 0) {
        return true;  /* remote isn't the version that rolled back */
    }
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(OTA_TAG, "rollback guard: NVS unavailable — refusing %s (fail safe)", remote);
        return false;
    }
    char seen[32] = {0};
    size_t len = sizeof(seen);
    uint8_t n = 0;
    if (nvs_get_str(h, OTA_NVS_KEY_RB_VER, seen, &len) == ESP_OK && strcmp(seen, remote) == 0) {
        nvs_get_u8(h, OTA_NVS_KEY_RB_N, &n);
    }
    bool allowed = n < OTA_ROLLBACK_MAX_RETRIES;
    if (allowed) {
        nvs_set_str(h, OTA_NVS_KEY_RB_VER, remote);
        nvs_set_u8(h, OTA_NVS_KEY_RB_N, (uint8_t)(n + 1));
        nvs_commit(h);
        ESP_LOGW(OTA_TAG, "%s was rolled back before — retry %u/%u (the gate can fail "
                 "for bot-side reasons)", remote, n + 1, OTA_ROLLBACK_MAX_RETRIES);
    } else {
        ESP_LOGE(OTA_TAG, "refusing %s: rolled back %u times — ship a NEW version "
                 "(or USB) to move this node", remote, OTA_ROLLBACK_MAX_RETRIES + 1);
    }
    nvs_close(h);
    return allowed;
}

#endif // LINUX_BUILD