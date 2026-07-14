#include "main.h"

#ifndef LINUX_BUILD

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *OTA_TAG = "ota";

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

static void build_firmware_url(char *out, size_t out_len, const char *suffix) {
    const char *base = PIPECAT_SMALLWEBRTC_URL;
    const char *api_off = strstr(base, "/api/offer");
    size_t prefix_len = api_off ? (size_t)(api_off - base) : strlen(base);
    snprintf(out, out_len, "%.*s/firmware/%s", (int)prefix_len, base, suffix);
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
static esp_err_t download_and_flash_ota(const char *firmware_url) {
    esp_http_client_config_t config = {};
    config.url = firmware_url;
    config.timeout_ms = 30000;
    config.keep_alive_enable = true;

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

void pipecat_ota_mark_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(OTA_TAG, "Marking current OTA app as valid");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) {
                ESP_LOGW(OTA_TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            }
        }
    }
}

#endif // LINUX_BUILD