/* Host-test stub: logging and error codes for src/rgb.c. */
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
