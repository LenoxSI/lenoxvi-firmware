#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <string.h>

#include "main.h"
#include "boot_diag.h"
#include "settings.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Every body byte the server sent, kept or not. The copy below stops at the
 * buffer size, so a total above it means the answer was cut short. */
static int answer_total_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  static int output_len;
  switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
      /* Only raised when auto-redirect is off (it isn't here: the client
       * follows redirects itself). The bot never redirects; log only. */
      ESP_LOGW(LOG_TAG, "HTTP_EVENT_REDIRECT");
      break;
    case HTTP_EVENT_ERROR:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
               evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA: {
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      if (esp_http_client_is_chunked_response(evt->client)) {
        ESP_LOGE(LOG_TAG, "Chunked HTTP response not supported");
#ifndef LINUX_BUILD
        boot_diag_restart("offer-chunked");
#endif
      }

      if (output_len == 0 && evt->user_data) {
        memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
      }

      // If user_data buffer is configured, copy the response into the buffer
      int copy_len = 0;
      if (evt->user_data) {
        // The last byte in evt->user_data is kept for the NULL character in
        // case of out-of-bound access.
        copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
        if (copy_len) {
          memcpy(((char *)evt->user_data) + output_len, evt->data, copy_len);
        }
      }
      output_len += copy_len;
      answer_total_len += evt->data_len;

      break;
    }
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_FINISH");
      output_len = 0;
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(LOG_TAG, "HTTP_EVENT_DISCONNECTED");
      output_len = 0;
      break;
  }
  return ESP_OK;
}

void pipecat_http_request(char *offer, char *answer) {
  esp_http_client_config_t config;
  memset(&config, 0, sizeof(esp_http_client_config_t));

  config.url = PIPECAT_SMALLWEBRTC_URL;
  config.event_handler = http_event_handler;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.user_data = answer;
  /* Self-signed HTTPS is handled by sdkconfig (ESP_TLS_SKIP_SERVER_CERT_VERIFY,
   * same as v1) — no cert bundle needed here. */

  ESP_LOGI(LOG_TAG, "Connecting to %s", config.url);

  cJSON *j_offer = cJSON_CreateObject();
  if (j_offer == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }
  if (cJSON_AddStringToObject(j_offer, "sdp", offer) == NULL) {
    cJSON_Delete(j_offer);
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }
  if (cJSON_AddStringToObject(j_offer, "type", "offer") == NULL) {
    cJSON_Delete(j_offer);
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }
  /* Room name = the zone routing key (LenoxVI v2 contract). 1.1.0:
   * the value is NVS-backed (dashboard-renamable) — the bake is the
   * fallback only (settings.cpp). */
  if (cJSON_AddStringToObject(j_offer, "room", settings_get_node_name()) == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to add room to offer");
  }

  ESP_LOGD(LOG_TAG, "OFFER\n%s", offer);

  /* Unformatted: the bot doesn't need the offer pretty-printed. An
   * allocation failure here gets a tagged reboot, not a panic. */
  char *j_offer_str = cJSON_PrintUnformatted(j_offer);

  cJSON_Delete(j_offer);
  if (j_offer_str == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to print JSON offer (out of memory)");
#ifndef LINUX_BUILD
    boot_diag_restart("offer-oom");
#endif
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(LOG_TAG, "esp_http_client_init failed (out of memory)");
#ifndef LINUX_BUILD
    boot_diag_restart("offer-oom");
#endif
  }
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, j_offer_str, strlen(j_offer_str));

  answer_total_len = 0;
  esp_err_t err = esp_http_client_perform(client);
  int status_code = esp_http_client_get_status_code(client);
  /* One tag for both cases, kept stable (the bot or docs may key on it);
   * the log says which: no answer at all (bot down or unreachable), or an
   * HTTP error status from the bot (it refused the offer). */
  if (err != ESP_OK) {
    ESP_LOGE(LOG_TAG, "offer: no answer from the bot: %s", esp_err_to_name(err));
#ifndef LINUX_BUILD
    boot_diag_restart("offer-http-error");
#endif
  }
  if (status_code != 200) {
    ESP_LOGE(LOG_TAG, "offer: the bot answered HTTP %d (%d body bytes)",
             status_code, answer_total_len);
#ifndef LINUX_BUILD
    boot_diag_restart("offer-http-error");
#endif
  }

  /* A 200 here proves only TCP to the bot, so it does NOT pass a pending
   * Wi-Fi trial: a network that lets TCP through but blocks UDP would pass,
   * drop the old creds, then never connect. The trial passes on the first
   * bot message over the data channel instead (cmd_note_activity). */

  /* A cut-short answer can never parse; give it its own tag so it is not
   * mistaken for a malformed one. */
  if (answer_total_len > MAX_HTTP_OUTPUT_BUFFER) {
    ESP_LOGE(LOG_TAG, "HTTP answer too big: %d bytes, buffer holds %d",
             answer_total_len, MAX_HTTP_OUTPUT_BUFFER);
#ifndef LINUX_BUILD
    boot_diag_restart("offer-too-big");
#endif
  }

  cJSON *j_response = cJSON_Parse((const char *)answer);
  if (j_response == NULL) {
    ESP_LOGE(LOG_TAG, "Error parsing HTTP response (%d bytes, not JSON)",
             answer_total_len);
#ifndef LINUX_BUILD
    boot_diag_restart("offer-bad-json");
#endif
  }

  /* Must be a string: a non-string "sdp" has no valuestring to copy. */
  cJSON *j_answer = cJSON_GetObjectItem(j_response, "sdp");
  if (!cJSON_IsString(j_answer)) {
    ESP_LOGE(LOG_TAG, "Unable to find `sdp` string in response");
#ifndef LINUX_BUILD
    boot_diag_restart("offer-no-sdp");
#endif
  }

  memset(answer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
  memcpy(answer, j_answer->valuestring, strlen(j_answer->valuestring));

  ESP_LOGD(LOG_TAG, "ANSWER\n%s", answer);

  cJSON_Delete(j_response);
  cJSON_free(j_offer_str);  // free cJSON_Print allocation

  esp_http_client_cleanup(client);
}
