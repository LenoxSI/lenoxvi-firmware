/* media.cpp — reSpeaker XVF3800 + XIAO ESP32S3 (lenoxvi Node B, bedroom)
 *
 * Port of the Waveshare media.cpp (v1 firmware, b4d1d28) for the reSpeaker:
 *   - I2C: SDA=5 SCL=6 (XIAO D4/D5), XVF3800 control @ 0x2C (probe only —
 *     the flashed I2S-slave firmware v1.0.8 needs no register setup)
 *   - I2S: ESP32 MASTER, STEREO, 32-bit, 16 kHz — XVF3800 is the I2S slave
 *     providing processed beamformed audio on DIN (GPIO43)
 *   - No codec driver: the XVF3800 IS the "codec" — raw i2s_channel_read
 *     replaces esp_codec_dev+ES7210
 *   - No local speaker path: replies come out of the room's Sonos; incoming
 *     Opus is decoded ONLY to drive is_playing (node-side turn lock: the mic
 *     is zeroed while the bot speaks — same protection v1 had)
 *   - Pins from Seeed's Xiaozhi reSpeaker porting config (verified 2026-09-15)
 *
 * Pipecat frame math unchanged: 320 mono int16 = 20ms @ 16kHz; hardware
 * capture is stereo 32-bit, so 320 frames = 2560 bytes, downconvert takes
 * the left channel's high 16 bits.
 */

#include <opus.h>
#include <atomic>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "main.h"

/* ── Pins / addresses (XIAO-integrated reSpeaker, Seeed porting config) ── */
#define I2C_PORT      I2C_NUM_0
#define I2C_SCL_PIN   GPIO_NUM_6
#define I2C_SDA_PIN   GPIO_NUM_5
#define XVF3800_ADDR  0x2C

#define I2S_PORT      I2S_NUM_1
/* XIAO D9/D8/D7/D6; 43/44 are UART0 pins repurposed for I2S */
#define I2S_BCLK_PIN  GPIO_NUM_8
#define I2S_LRCK_PIN  GPIO_NUM_7
#define I2S_DOUT_PIN  GPIO_NUM_44
#define I2S_DIN_PIN   GPIO_NUM_43

/* ── Audio constants (unchanged from v1) ────────────────────────────────── */
#define SAMPLE_RATE              16000
#define OPUS_BUFFER_SIZE         1276
#define OPUS_ENCODER_BITRATE     40000
#define OPUS_ENCODER_COMPLEXITY  0
#define PCM_MONO_SAMPLES         320
#define PCM_MONO_BYTES           (PCM_MONO_SAMPLES * 2)
#define HW_STEREO32_BYTES        (PCM_MONO_SAMPLES * 2 * 4)
#define I2S_READ_TIMEOUT_MS      100

/* ── Handles ────────────────────────────────────────────────────────────── */
static i2c_master_bus_handle_t i2c_bus  = NULL;
static i2s_chan_handle_t        tx_chan  = NULL;
static i2s_chan_handle_t        rx_chan  = NULL;
static std::atomic<bool>        is_playing = false;

static void update_is_playing(int16_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] > 1 || buf[i] < -1) { is_playing = true; return; }
    }
    is_playing = false;
}

/* ── Hardware init ──────────────────────────────────────────────────────── */
void pipecat_init_audio_capture() {
    /* I2C master bus — for the XVF3800 control interface + diagnostics */
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                     = I2C_PORT;
    bus_cfg.sda_io_num                   = I2C_SDA_PIN;
    bus_cfg.scl_io_num                   = I2C_SCL_PIN;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* XVF3800 probe — proves the flashed firmware is alive on the bus */
    i2c_device_config_t xvf_dev = {};
    xvf_dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    xvf_dev.device_address  = XVF3800_ADDR;
    i2c_master_dev_handle_t xvf_handle;
    if (i2c_master_bus_add_device(i2c_bus, &xvf_dev, &xvf_handle) == ESP_OK) {
        uint8_t dummy;
        if (i2c_master_receive(xvf_handle, &dummy, 1, 10) == ESP_OK)
            ESP_LOGI(LOG_TAG, "XVF3800 detected @ 0x%02x", XVF3800_ADDR);
        else
            ESP_LOGE(LOG_TAG, "XVF3800 NOT responding @ 0x%02x!", XVF3800_ADDR);
        i2c_master_bus_rm_device(xvf_handle);
    }

    /* I2S scan (diagnostics, same as v1) */
    ESP_LOGI(LOG_TAG, "I2C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_device_config_t scan_dev = {};
        scan_dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        scan_dev.device_address  = addr;
        i2c_master_dev_handle_t scan_handle;
        if (i2c_master_bus_add_device(i2c_bus, &scan_dev, &scan_handle) != ESP_OK) continue;
        uint8_t dummy;
        esp_err_t e = i2c_master_receive(scan_handle, &dummy, 1, 10);
        i2c_master_bus_rm_device(scan_handle);
        if (e == ESP_OK) ESP_LOGI(LOG_TAG, "  I2C device @ 0x%02x", addr);
    }
    ESP_LOGI(LOG_TAG, "I2C scan done");

    /* I2S — STEREO, 32-bit, 16 kHz; ESP32 master, XVF3800 slave.
     * No MCLK: the XVF3800 slave firmware clocks from BCLK/WS only. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                           I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = GPIO_NUM_NC;   /* XVF3800 needs no MCLK */
    std_cfg.gpio_cfg.bclk = I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = I2S_LRCK_PIN;
    std_cfg.gpio_cfg.dout = I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din  = I2S_DIN_PIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(LOG_TAG, "Audio ready: XVF3800 I2S-slave stereo32 @ %d Hz", SAMPLE_RATE);
}

/* ── Opus decoder — incoming bot audio (decoded for turn-lock only) ─────── */
static opus_int16 *decoder_buffer = NULL;
static OpusDecoder *opus_decoder  = NULL;

void pipecat_init_audio_decoder() {
    int err;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK) { ESP_LOGE(LOG_TAG, "opus_decoder_create: %d", err); return; }
    decoder_buffer = (opus_int16 *)malloc(PCM_MONO_SAMPLES * sizeof(opus_int16));
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
    /* No local speaker (Sonos carries the reply) — decode only to track
     * is_playing, which zeroes the mic while the bot speaks (node-side
     * turn lock; the room mic still hears the Sonos audio acoustically,
     * so the appliance-side gates stay armed too). */
    int n = opus_decode(opus_decoder, data, size, decoder_buffer, PCM_MONO_SAMPLES, 0);
    if (n <= 0) return;
    update_is_playing(decoder_buffer, n);
}

/* ── Opus encoder (mic in) ──────────────────────────────────────────────── */
static OpusEncoder *opus_encoder          = NULL;
static uint8_t     *encoder_output_buffer = NULL;
static int32_t     *mic_stereo32          = NULL;
static int16_t     *mic_mono16            = NULL;

void pipecat_init_audio_encoder() {
    int err;
    opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { ESP_LOGE(LOG_TAG, "opus_encoder_create: %d", err); return; }
    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
    opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
    opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    mic_stereo32          = (int32_t *)heap_caps_malloc(HW_STEREO32_BYTES, MALLOC_CAP_DEFAULT);
    mic_mono16            = (int16_t *)malloc(PCM_MONO_BYTES);
    encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);
}

void pipecat_send_audio(PeerConnection *peer_connection) {
    size_t bytes_read = 0;
    if (i2s_channel_read(rx_chan, mic_stereo32, HW_STEREO32_BYTES,
                          &bytes_read, pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "i2s_channel_read failed");
        return;
    }
    if (bytes_read < HW_STEREO32_BYTES) return;  /* partial frame: drop */
    /* stereo32 -> mono16: left channel, high 16 bits (unchanged from v1) */
    for (int i = 0; i < PCM_MONO_SAMPLES; i++) {
        mic_mono16[i] = (int16_t)(mic_stereo32[2 * i] >> 16);
    }
    if (is_playing) { memset(mic_mono16, 0, PCM_MONO_BYTES); }
    auto n = opus_encode(opus_encoder, (const opus_int16 *)mic_mono16,
                         PCM_MONO_SAMPLES, encoder_output_buffer, OPUS_BUFFER_SIZE);
    if (n > 0) peer_connection_send_audio(peer_connection, encoder_output_buffer, n);
}