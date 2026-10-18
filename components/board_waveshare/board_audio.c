/* board_audio.c — Waveshare ESP32-S3-AUDIO mic capture (ES7210 4-mic ADC).
 *
 * Bring-up order, pins and esp_codec_dev usage follow the known-working
 * waveshare-v1 firmware (Waveshare factory_01 reference):
 *   I2C0 (SCL 10, SDA 11, external pull-ups) -> TCA9555 codec power/reset
 *   lines high -> I2S1 RX (master, stereo 32-bit, 16 kHz) -> ES7210.
 * The ES8311 DAC / speaker is not brought up in this phase; the PA stays off.
 *
 * Capture is stereo 32-bit; board_audio_read() hands over the left channel's
 * high 16 bits as 16 kHz mono int16. Init fails soft: any failure leaves
 * the module "dead" and reads deliver paced silence. */

#include <string.h>

#include "board_audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "board_audio"

/* ── Pins / addresses (Waveshare bsp_board.h) ──────────────────────────── */
#define I2C_PORT      I2C_NUM_0
#define I2C_SCL_PIN   GPIO_NUM_10
#define I2C_SDA_PIN   GPIO_NUM_11

#define I2S_PORT      I2S_NUM_1
#define I2S_MCLK_PIN  GPIO_NUM_12
#define I2S_BCLK_PIN  GPIO_NUM_13
#define I2S_LRCK_PIN  GPIO_NUM_14
#define I2S_DIN_PIN   GPIO_NUM_15

#define ES7210_ADDR   0x40

/* TCA9555: 0,1,5,6 = codec power/reset (codecs NACK I2C until high); 8 = PA */
#define EXIO_CODEC_EN (IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | \
                       IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6)
#define EXIO_PA_EN    IO_EXPANDER_PIN_NUM_8

#define SAMPLE_RATE   16000
#define MIC_GAIN_DB   40.0f
/* Largest frame read at once: 320 mono samples = 20 ms */
#define MAX_FRAMES    320

static i2c_master_bus_handle_t  s_i2c_bus;
static esp_io_expander_handle_t s_io_exp;
static i2s_chan_handle_t        s_rx_chan;
static esp_codec_dev_handle_t   s_mic_dev;
static int32_t                 *s_stereo32;   /* MAX_FRAMES * 2 slots */
static bool                     s_dead = true;

#ifdef MIC_LEVEL_LOG
static TickType_t s_lvl_until;   /* 0 = logging window over */
static uint64_t   s_lvl_sumsq;
static uint32_t   s_lvl_count;
static int32_t    s_lvl_peak;
static int        s_lvl_reads;
#endif

static esp_err_t fail(const char *step, esp_err_t err) {
    ESP_LOGE(TAG, "board_audio: %s failed: %s", step, esp_err_to_name(err));
    s_dead = true;
    /* Whatever failed, never leave the speaker PA on (best effort). */
    if (s_io_exp) esp_io_expander_set_level(s_io_exp, EXIO_PA_EN, 0);
    return err;
}

/* Silence at the real frame rate: the capture loop adds its own delay after
 * each frame, so pace to an absolute 20 ms schedule instead of sleeping a
 * fixed 20 ms on top of it (that gave ~28 frames/s, not 50). */
static void pace_silence(void) {
    static TickType_t last;
    TickType_t now = xTaskGetTickCount();
    if (last == 0 || now - last > pdMS_TO_TICKS(100)) last = now;  /* (re)start the schedule */
    vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
}

esp_err_t board_audio_init(void) {
    esp_err_t err;
    /* One-shot: a second call must not tear down a working mic (bus and
     * channel already exist). After a failed init it stays dead (no
     * cleanup of partially created handles; a reboot is the retry). */
    static bool attempted;
    if (attempted) return s_dead ? ESP_ERR_INVALID_STATE : ESP_OK;
    attempted = true;
    s_dead = true;

    s_stereo32 = heap_caps_malloc(MAX_FRAMES * 2 * sizeof(int32_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_stereo32 == NULL) s_stereo32 = malloc(MAX_FRAMES * 2 * sizeof(int32_t));
    if (s_stereo32 == NULL) return fail("buffer alloc", ESP_ERR_NO_MEM);

    /* I2C master bus — the board has external pull-ups */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 0,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) return fail("i2c_new_master_bus", err);

    /* TCA9555 IO expander: codec power/reset lines */
    err = esp_io_expander_new_i2c_tca95xx_16bit(
        s_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &s_io_exp);
    if (err != ESP_OK) return fail("tca9555 new", err);
    /* The driver's reset leaves the OUTPUT register at 0xFFFF, so switching
     * pin 8 to output would turn the PA on until the next write. Write the
     * register with PA low FIRST (set_level refuses input pins), then
     * switch directions. */
    err = s_io_exp->write_output_reg(s_io_exp, 0xFFFF & ~(uint32_t)EXIO_PA_EN);
    if (err != ESP_OK) return fail("tca9555 PA low (pre-dir)", err);
    err = esp_io_expander_set_dir(s_io_exp, EXIO_CODEC_EN | EXIO_PA_EN,
                                  IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) return fail("tca9555 set_dir", err);
    err = esp_io_expander_set_level(s_io_exp, EXIO_CODEC_EN, 1);
    if (err != ESP_OK) return fail("tca9555 codec enable", err);
    /* Speaker PA OFF: there is no speaker path in this phase, and the PA
     * MUST stay off until the ES8311 output is brought up deliberately. */
    err = esp_io_expander_set_level(s_io_exp, EXIO_PA_EN, 0);
    if (err != ESP_OK) return fail("tca9555 PA off", err);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* I2S RX only: ESP32 is master and drives MCLK/BCLK/WS */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) return fail("i2s_new_channel", err);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN_PIN,
        },
    };
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) return fail("i2s_channel_init_std_mode", err);
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) return fail("i2s_channel_enable", err);

    /* esp_codec_dev data interface: input-only, no TX handle */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_PORT,
        .rx_handle = s_rx_chan,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (data_if == NULL) return fail("audio_codec_new_i2s_data", ESP_FAIL);

    /* ES7210: ONLY addr + bus_handle — setting .port broke writes */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES7210_ADDR << 1,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) return fail("audio_codec_new_i2c_ctrl", ESP_FAIL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);
    if (codec_if == NULL) return fail("es7210_codec_new", ESP_FAIL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_mic_dev = esp_codec_dev_new(&dev_cfg);
    if (s_mic_dev == NULL) return fail("esp_codec_dev_new", ESP_FAIL);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 32,
        .channel = 2,
        .sample_rate = SAMPLE_RATE,
    };
    int rc = esp_codec_dev_open(s_mic_dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) return fail("esp_codec_dev_open", ESP_FAIL);
    rc = esp_codec_dev_set_in_channel_gain(s_mic_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), MIC_GAIN_DB);
    if (rc != ESP_CODEC_DEV_OK) return fail("set_in_channel_gain(0)", ESP_FAIL);
    rc = esp_codec_dev_set_in_channel_gain(s_mic_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), MIC_GAIN_DB);
    if (rc != ESP_CODEC_DEV_OK) return fail("set_in_channel_gain(1)", ESP_FAIL);

    s_dead = false;
#ifdef MIC_LEVEL_LOG
    s_lvl_until = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    if (s_lvl_until == 0) s_lvl_until = 1;
#endif
    ESP_LOGI(TAG, "board_audio: ES7210 up (16 kHz, 4 mics, gain 40 dB, PA off)");
    return ESP_OK;
}

#ifdef MIC_LEVEL_LOG
/* Bench aid: rms/peak of the mono stream, once per 50 reads, first 30 s. */
static void mic_level_accumulate(const int16_t *s, int n) {
    if (s_lvl_until == 0) return;
    if ((int32_t)(xTaskGetTickCount() - s_lvl_until) > 0) { s_lvl_until = 0; return; }
    for (int i = 0; i < n; i++) {
        int32_t v = s[i];
        s_lvl_sumsq += (uint64_t)(v * v);
        int32_t a = v < 0 ? -v : v;
        if (a > s_lvl_peak) s_lvl_peak = a;
    }
    s_lvl_count += n;
    if (++s_lvl_reads < 50) return;
    uint64_t mean = s_lvl_count ? s_lvl_sumsq / s_lvl_count : 0;
    uint32_t rms = 0;   /* integer sqrt */
    while ((uint64_t)(rms + 1) * (rms + 1) <= mean) rms++;
    ESP_LOGI(TAG, "mic: rms=%d peak=%d", (int)rms, (int)s_lvl_peak);
    s_lvl_sumsq = 0;
    s_lvl_count = 0;
    s_lvl_peak = 0;
    s_lvl_reads = 0;
}
#endif

bool board_audio_read(int16_t *out, int n) {
    if (s_dead || n <= 0 || n > MAX_FRAMES) {
        memset(out, 0, (size_t)(n > 0 ? n : 0) * sizeof(int16_t));
        pace_silence();
        return false;
    }
    int rc = esp_codec_dev_read(s_mic_dev, s_stereo32, n * 2 * (int)sizeof(int32_t));
    if (rc != ESP_CODEC_DEV_OK) {
        static bool warned;
        static TickType_t last_warn;
        TickType_t now = xTaskGetTickCount();
        if (!warned || now - last_warn >= pdMS_TO_TICKS(1000)) {
            warned = true;
            last_warn = now;
            ESP_LOGW(TAG, "board_audio: esp_codec_dev_read failed: %d", rc);
        }
        memset(out, 0, (size_t)n * sizeof(int16_t));
        /* Keep the 20 ms pacing: a read that fails instantly must not turn
         * the capture loop (prio 7) into a busy spin -> task-WDT reboot loop. */
        pace_silence();
        return false;
    }
    /* stereo32 -> mono16: left channel, high 16 bits */
    for (int i = 0; i < n; i++) out[i] = (int16_t)(s_stereo32[2 * i] >> 16);
#ifdef MIC_LEVEL_LOG
    mic_level_accumulate(out, n);
#endif
    return true;
}
