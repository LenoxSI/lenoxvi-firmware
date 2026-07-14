/* media.cpp — Waveshare ESP32-S3-AUDIO-Board
 *
 * Matches Waveshare's VERIFIED factory_01 ESP-IDF reference:
 *   - I2C bus: minimal config (external pull-ups on board), port 0, SCL=10 SDA=11
 *   - TCA9555 via esp_io_expander_tca95xx_16bit: pins 0,1,5,6,8 = OUTPUT
 *     (these are codec power/reset/PA lines — codecs WON'T ACK I2C writes
 *      until these are driven; that was the NAK root cause)
 *   - PA enable = IO expander pin 8 (IO_EXPANDER_PIN_NUM_8)
 *   - I2S: NUM_1, STEREO, 32-bit, 16 kHz, full-duplex (one channel pair)
 *   - ES7210: 4 mics, stereo 32-bit capture
 *   - ES8311: use_mclk=false, codec_mode=DAC, pa_pin=-1, gpio_if set
 *   - audio_codec_i2c_cfg_t: ONLY .addr + .bus_handle (no .port!)
 *
 * Pipecat needs mono 16-bit @ 16 kHz, so the read path downconverts
 * stereo-32 → mono-16 (take left channel, take high 16 bits), and the
 * write path upconverts mono-16 → stereo-32.
 */

#include <opus.h>
#include <atomic>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "main.h"

/* ── Pins / addresses (from Waveshare bsp_board.h) ─────────────────────── */
#define I2C_PORT      I2C_NUM_0
#define I2C_SCL_PIN   GPIO_NUM_10
#define I2C_SDA_PIN   GPIO_NUM_11

#define I2S_PORT      I2S_NUM_1
#define I2S_MCLK_PIN  GPIO_NUM_12
#define I2S_BCLK_PIN  GPIO_NUM_13
#define I2S_LRCK_PIN  GPIO_NUM_14
#define I2S_DIN_PIN   GPIO_NUM_15
#define I2S_DOUT_PIN  GPIO_NUM_16

#define ES8311_ADDR   0x18
#define ES7210_ADDR   0x40

/* IO expander pin masks (esp_io_expander uses 1<<n) */
#define EXIO_PA_EN    IO_EXPANDER_PIN_NUM_8

/* ── Audio constants ────────────────────────────────────────────────────── */
#define SAMPLE_RATE              16000
#define OPUS_BUFFER_SIZE         1276
#define OPUS_ENCODER_BITRATE     40000
#define OPUS_ENCODER_COMPLEXITY  0
/* Pipecat frame: 320 mono int16 samples = 20 ms @ 16 kHz = 640 bytes */
#define PCM_MONO_SAMPLES         320
#define PCM_MONO_BYTES           (PCM_MONO_SAMPLES * 2)
/* Hardware capture: stereo 32-bit, so 320 frames = 320*2ch*4B = 2560 bytes */
#define HW_STEREO32_BYTES        (PCM_MONO_SAMPLES * 2 * 4)

/* ── Handles ────────────────────────────────────────────────────────────── */
static i2c_master_bus_handle_t i2c_bus     = NULL;
static esp_io_expander_handle_t io_expander = NULL;
static i2s_chan_handle_t        tx_chan     = NULL;
static i2s_chan_handle_t        rx_chan     = NULL;
static esp_codec_dev_handle_t   mic_dev    = NULL;
static esp_codec_dev_handle_t   spk_dev    = NULL;
static std::atomic<bool>        is_playing  = false;

static void update_is_playing(int16_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] > 1 || buf[i] < -1) { is_playing = true; return; }
    }
    is_playing = false;
}

/* ── Hardware init ──────────────────────────────────────────────────────── */
void pipecat_init_audio_capture() {
    /* I2C master bus — minimal config like the reference ───────────────── */
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                     = I2C_PORT;
    bus_cfg.sda_io_num                   = I2C_SDA_PIN;
    bus_cfg.scl_io_num                   = I2C_SCL_PIN;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* TCA9555 IO expander — drive codec power/reset/PA lines ────────────── */
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca95xx_16bit(
        i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &io_expander));
    /* pins 0,1,5,6,8 = OUTPUT (codec enable lines + PA) */
    ESP_ERROR_CHECK(esp_io_expander_set_dir(
        io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5 |
        IO_EXPANDER_PIN_NUM_6 | IO_EXPANDER_PIN_NUM_8,
        IO_EXPANDER_OUTPUT));
    /* drive codec enable lines high, then PA on */
    esp_io_expander_set_level(io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
        IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(io_expander, EXIO_PA_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(LOG_TAG, "TCA9555 codec/PA lines enabled");

    /* I2C bus scan — logs all responding device addresses */
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

    /* Manual write test — ES8311 reset register at 50kHz */
    ESP_LOGI(LOG_TAG, "I2C write test to ES8311 @ 0x18:");
    {
        i2c_device_config_t wr_dev = {};
        wr_dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        wr_dev.device_address  = ES8311_ADDR;
        i2c_master_dev_handle_t wr_handle;
        esp_err_t wr_err = i2c_master_bus_add_device(i2c_bus, &wr_dev, &wr_handle);
        if (wr_err == ESP_OK) {
            uint8_t reset_cmd[] = {0x00, 0x80};
            esp_err_t e = i2c_master_transmit(wr_handle, reset_cmd, sizeof(reset_cmd), 100);
            ESP_LOGI(LOG_TAG, "  ES8311 write result: %s", esp_err_to_name(e));
            i2c_master_bus_rm_device(wr_handle);
        } else {
            ESP_LOGI(LOG_TAG, "  Failed to add device: %s", esp_err_to_name(wr_err));
        }
    }

    /* I2S — STEREO, 32-bit, 16 kHz, full-duplex (matches reference) ────── */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                           I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_MCLK_PIN;
    std_cfg.gpio_cfg.bclk = I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = I2S_LRCK_PIN;
    std_cfg.gpio_cfg.dout = I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din  = I2S_DIN_PIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    /* esp_codec_dev I2S data interface ─────────────────────────────────── */
    audio_codec_i2s_cfg_t i2s_data_cfg = {};
    i2s_data_cfg.port      = I2S_PORT;
    i2s_data_cfg.rx_handle = rx_chan;
    i2s_data_cfg.tx_handle = tx_chan;
    const audio_codec_data_if_t *i2s_data = audio_codec_new_i2s_data(&i2s_data_cfg);

    /* ── ES7210 mic ADC ─────────────────────────────────────────────────── */
    /* i2c_cfg: ONLY addr + bus_handle (no .port — that was breaking writes) */
    audio_codec_i2c_cfg_t adc_i2c = {};
    adc_i2c.addr       = (ES7210_ADDR << 1);
    adc_i2c.bus_handle  = i2c_bus;
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if      = es7210_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                              ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);

    esp_codec_dev_cfg_t mic_cfg = {};
    mic_cfg.codec_if = es7210_if;
    mic_cfg.data_if  = i2s_data;
    mic_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    mic_dev = esp_codec_dev_new(&mic_cfg);

    esp_codec_dev_sample_info_t mic_fs = {};
    mic_fs.bits_per_sample = 32;
    mic_fs.channel         = 2;
    mic_fs.sample_rate     = SAMPLE_RATE;
    ESP_ERROR_CHECK(esp_codec_dev_open(mic_dev, &mic_fs));
    esp_codec_dev_set_in_channel_gain(mic_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 40.0f);
    esp_codec_dev_set_in_channel_gain(mic_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), 40.0f);

    /* ── ES8311 speaker DAC ─────────────────────────────────────────────── */
    audio_codec_i2c_cfg_t dac_i2c = {};
    dac_i2c.addr       = (ES8311_ADDR << 1);
    dac_i2c.bus_handle  = i2c_bus;
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.ctrl_if    = es8311_ctrl;
    es8311_cfg.gpio_if    = gpio_if;
    es8311_cfg.pa_pin     = -1;       /* PA is on TCA9555, not GPIO */
    es8311_cfg.use_mclk   = false;    /* board clocks ES8311 from BCLK */
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t spk_cfg = {};
    spk_cfg.codec_if = es8311_if;
    spk_cfg.data_if  = i2s_data;
    spk_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    spk_dev = esp_codec_dev_new(&spk_cfg);

    esp_codec_dev_sample_info_t spk_fs = {};
    spk_fs.bits_per_sample = 32;
    spk_fs.channel         = 2;
    spk_fs.sample_rate     = SAMPLE_RATE;
    esp_codec_dev_set_out_vol(spk_dev, 70);
    ESP_ERROR_CHECK(esp_codec_dev_open(spk_dev, &spk_fs));

    ESP_LOGI(LOG_TAG, "Audio ready: ES8311+ES7210 stereo32 @ %d Hz", SAMPLE_RATE);
}

/* ── Opus decoder (speaker out) ─────────────────────────────────────────── */
static opus_int16 *decoder_buffer = NULL;   /* mono 16-bit from Opus */
static int32_t    *spk_stereo32   = NULL;   /* upconverted stereo 32-bit */
static OpusDecoder *opus_decoder  = NULL;

void pipecat_init_audio_decoder() {
    int err;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK) { ESP_LOGE(LOG_TAG, "opus_decoder_create: %d", err); return; }
    decoder_buffer = (opus_int16 *)malloc(PCM_MONO_SAMPLES * sizeof(opus_int16));
    spk_stereo32   = (int32_t *)heap_caps_malloc(PCM_MONO_SAMPLES * 2 * sizeof(int32_t),
                                                 MALLOC_CAP_DEFAULT);
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
    int n = opus_decode(opus_decoder, data, size, decoder_buffer, PCM_MONO_SAMPLES, 0);
    if (n <= 0) return;
    update_is_playing(decoder_buffer, n);
    /* mono16 → stereo32: duplicate to both channels, shift left 16 */
    for (int i = 0; i < n; i++) {
        int32_t s = ((int32_t)decoder_buffer[i]) << 16;
        spk_stereo32[2 * i]     = s;
        spk_stereo32[2 * i + 1] = s;
    }
    esp_codec_dev_write(spk_dev, spk_stereo32, n * 2 * sizeof(int32_t));
}

/* ── Opus encoder (mic in) ──────────────────────────────────────────────── */
static OpusEncoder *opus_encoder          = NULL;
static uint8_t     *encoder_output_buffer = NULL;
static int32_t     *mic_stereo32          = NULL;  /* raw stereo 32-bit */
static int16_t     *mic_mono16            = NULL;  /* downconverted mono 16-bit */

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
    if (esp_codec_dev_read(mic_dev, mic_stereo32, HW_STEREO32_BYTES) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "esp_codec_dev_read failed");
        return;
    }
    /* stereo32 → mono16: take left channel, high 16 bits */
    for (int i = 0; i < PCM_MONO_SAMPLES; i++) {
        mic_mono16[i] = (int16_t)(mic_stereo32[2 * i] >> 16);
    }
    if (is_playing) { memset(mic_mono16, 0, PCM_MONO_BYTES); }
    auto n = opus_encode(opus_encoder, (const opus_int16 *)mic_mono16,
                         PCM_MONO_SAMPLES, encoder_output_buffer, OPUS_BUFFER_SIZE);
    peer_connection_send_audio(peer_connection, encoder_output_buffer, n);
}
