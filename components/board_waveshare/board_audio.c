/* board_audio.c — Waveshare ESP32-S3-AUDIO mic capture (ES7210 4-mic ADC)
 * and built-in speaker (ES8311 DAC + PA).
 *
 * Bring-up order, pins and esp_codec_dev usage follow the known-working
 * waveshare-v1 firmware (Waveshare factory_01 reference):
 *   I2C0 (SCL 10, SDA 11, external pull-ups) -> TCA9555 codec power/reset
 *   lines high -> I2S1 full duplex (master, stereo 32-bit, 16 kHz) ->
 *   ES7210 (mic) -> ES8311 (speaker, DAC mode, clocked from BCLK).
 *
 * Capture is stereo 32-bit; board_audio_read() hands over the left channel's
 * high 16 bits as 16 kHz mono int16. Init fails soft: any failure leaves
 * the module "dead" and reads deliver paced silence. A speaker failure only
 * leaves the speaker dead (PA off); the mic keeps working.
 *
 * Playback: board_audio_play() (libpeer's task, non-blocking) fills a 200 ms
 * ring; a playback task below capture priority drains it in 20 ms frames
 * straight into the I2S TX channel. The PA (TCA9555 pin 8) is on only while
 * real audio plays: it comes on with the first non-silent frame and goes off
 * after 1 s of silence/underrun, or at once when the speaker is disabled. */

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
#include "freertos/semphr.h"
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
#define I2S_DOUT_PIN  GPIO_NUM_16

#define ES7210_ADDR   0x40
#define ES8311_ADDR   0x18

/* TCA9555: 0,1,5,6 = codec power/reset (codecs NACK I2C until high); 8 = PA */
#define EXIO_CODEC_EN (IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | \
                       IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6)
#define EXIO_PA_EN    IO_EXPANDER_PIN_NUM_8

#define SAMPLE_RATE   16000
#define MIC_GAIN_DB   40.0f
/* Largest frame read at once: 320 mono samples = 20 ms */
#define MAX_FRAMES    320

/* Speaker playback (all at 16 kHz mono) */
#define SPK_RING_SAMPLES 3200   /* 200 ms ring */
#define SPK_FRAME        320    /* 20 ms written per I2S call */
#define SPK_PREBUF       960    /* 60 ms buffered before playback starts */
#define SPK_REARM_MS     100    /* dry this long -> wait for the prebuffer again */
#define SPK_PA_HOLD_MS   1000   /* silence/underrun this long -> PA off */
#define SPK_WRITE_TO_MS  100    /* i2s_channel_write timeout (one frame is 20 ms) */
#define SPK_TASK_PRIO    5      /* below the capture task (7) */
#define SPK_TASK_STACK   4096   /* LOG (vfprintf) inside the I2C PA write path */
#define SPK_DEFAULT_VOL  40
#define SPK_MIC_HOLD_MS  300    /* mic stays muted this long after audible playback */
#define SPK_IDLE_FLUSH_MS 200   /* a sub-prebuffer fragment this stale is dropped */
#define SPK_PA_RETRY_MS  500    /* back-off after a failed PA write */

static i2c_master_bus_handle_t  s_i2c_bus;
static esp_io_expander_handle_t s_io_exp;
static i2s_chan_handle_t        s_rx_chan;
static i2s_chan_handle_t        s_tx_chan;
static esp_codec_dev_handle_t   s_mic_dev;
static esp_codec_dev_handle_t   s_spk_dev;
static int32_t                 *s_stereo32;   /* MAX_FRAMES * 2 slots */
static bool                     s_dead = true;

static bool              s_speaker_ok;
static volatile bool     s_spk_enabled;                 /* default: disabled */
static volatile int      s_spk_volume = SPK_DEFAULT_VOL;
static int16_t          *s_ring;                        /* SPK_RING_SAMPLES */
static int               s_ring_head;                   /* next write index */
static int               s_ring_count;                  /* samples buffered */
static uint32_t          s_spk_overflow;                /* samples dropped: ring full */
static portMUX_TYPE      s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static int32_t          *s_spk_frame;                   /* SPK_FRAME * 2 slots */
static int16_t          *s_spk_mono;                    /* SPK_FRAME (off the task stack) */
static SemaphoreHandle_t s_spk_wake;                    /* play/set -> task */
static SemaphoreHandle_t s_pa_lock;                     /* serialises PA writes */
static bool              s_pa_on;
static bool              s_pa_dirty;    /* last PA write failed: real level unknown */
static TickType_t        s_pa_fail_at;  /* when it failed (retry back-off) */
static volatile TickType_t s_spk_last_real;            /* last audible frame PLAYED */

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
    /* Whatever failed, never leave the speaker PA on (best effort). Write the
     * output register directly: works whatever the pin direction is. */
    if (s_io_exp) s_io_exp->write_output_reg(s_io_exp, 0xFFFF & ~(uint32_t)EXIO_PA_EN);
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

/* ── Speaker ───────────────────────────────────────────────────────────── */

/* PA on/off, serialised between the playback task and speaker_set's caller.
 * It only turns on while the speaker is enabled and up; a failed off-write
 * falls back to writing the whole output register (codec lines high, PA low). */
static void pa_set(bool on) {
    if (s_io_exp == NULL || s_pa_lock == NULL) return;
    xSemaphoreTake(s_pa_lock, portMAX_DELAY);
    if (on && (!s_spk_enabled || !s_speaker_ok)) on = false;
    TickType_t now = xTaskGetTickCount();
    /* A failed write leaves the real level unknown (it may have latched):
     * OFF is then always re-written, straight to the output register
     * (the driver skips writes that match its cache). ON backs off. */
    bool want = on != s_pa_on || (!on && s_pa_dirty);
    if (want && on && s_pa_dirty && now - s_pa_fail_at < pdMS_TO_TICKS(SPK_PA_RETRY_MS)) want = false;
    if (want) {
        esp_err_t err = on || !s_pa_dirty
            ? esp_io_expander_set_level(s_io_exp, EXIO_PA_EN, on ? 1 : 0)
            : ESP_FAIL;
        if (err != ESP_OK && !on)
            err = s_io_exp->write_output_reg(s_io_exp, 0xFFFF & ~(uint32_t)EXIO_PA_EN);
        if (err == ESP_OK) {
            s_pa_on = on;
            s_pa_dirty = false;
            ESP_LOGI(TAG, "board_audio: PA %s (ring overflow %u)", on ? "on" : "off",
                     (unsigned)s_spk_overflow);
        } else {
            bool first = !s_pa_dirty;
            s_pa_dirty = true;
            s_pa_fail_at = now;
            if (first)   /* once per failure streak — no 50/s flood */
                ESP_LOGW(TAG, "board_audio: PA %s failed: %s", on ? "on" : "off",
                         esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_pa_lock);
}

static void ring_flush(void) {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_count = 0;
    portEXIT_CRITICAL(&s_ring_mux);
}

static int ring_avail(void) {
    portENTER_CRITICAL(&s_ring_mux);
    int n = s_ring_count;
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

/* Pop up to n samples (oldest first); returns how many were popped. */
static int ring_pop(int16_t *out, int n) {
    portENTER_CRITICAL(&s_ring_mux);
    int take = n < s_ring_count ? n : s_ring_count;
    int tail = (s_ring_head - s_ring_count + SPK_RING_SAMPLES) % SPK_RING_SAMPLES;
    int first = take < SPK_RING_SAMPLES - tail ? take : SPK_RING_SAMPLES - tail;
    memcpy(out, &s_ring[tail], (size_t)first * sizeof(int16_t));
    memcpy(out + first, s_ring, (size_t)(take - first) * sizeof(int16_t));
    s_ring_count -= take;
    portEXIT_CRITICAL(&s_ring_mux);
    return take;
}

/* Top the TX DMA up with silence until a write would block. Called on every
 * idle -> playing transition: while idle the task writes nothing and the
 * DMA (auto_clear) drains, so the first writes would otherwise return at once,
 * empty the 60 ms prebuffer into DMA and leave a hole right after the first
 * 60 ms of every reply. With the DMA full, i2s_channel_write paces the task
 * and the ring keeps its slack (~75 ms extra latency, by design). */
static void dma_fill_silence(void) {
    memset(s_spk_frame, 0, SPK_FRAME * 2 * sizeof(int32_t));
    for (int i = 0; i < 8; i++) {
        size_t written = 0;
        if (i2s_channel_write(s_tx_chan, s_spk_frame, SPK_FRAME * 2 * sizeof(int32_t),
                              &written, 0) != ESP_OK || written == 0) break;
    }
}

/* Playback: blocks on the wake semaphore while idle, on i2s_channel_write
 * (DMA-paced, 20 ms frames) while playing. Underruns play silence; after
 * SPK_REARM_MS dry it waits for the prebuffer again. */
static void spk_task(void *arg) {
    int16_t *mono = s_spk_mono;
    bool primed = false;        /* prebuffer reached, draining the ring */
    int dry_ms = 0;             /* consecutive underrun while primed */
    TickType_t last_real = 0;   /* last non-silent frame (PA hold) */
    TickType_t last_warn = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t lr = s_spk_last_real;
        if (lr != 0 && now - lr >= pdMS_TO_TICKS(SPK_MIC_HOLD_MS))
            s_spk_last_real = 0;    /* hold over: no tick-wrap ghost mute later */
        if (!s_spk_enabled) {
            primed = false;
            ring_flush();
            pa_set(false);
            xSemaphoreTake(s_spk_wake, pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_pa_dirty && !s_pa_on) pa_set(false);   /* retry a failed OFF */
        if (!primed && ring_avail() >= SPK_PREBUF) {
            primed = true;
            dry_ms = 0;
            if (!s_pa_on) dma_fill_silence();   /* cold start: DMA paces from here */
        }
        if (!primed && !s_pa_on) {
            /* Idle: nothing to play and the PA is off — sleep until
             * board_audio_play() or speaker_set wakes us. A fragment that
             * never reaches the prebuffer is dropped once stale, so it
             * can't play in front of the next reply. */
            bool pending = ring_avail() > 0;
            if (xSemaphoreTake(s_spk_wake, pdMS_TO_TICKS(pending ? SPK_IDLE_FLUSH_MS : 1000)) != pdTRUE
                && pending && ring_avail() < SPK_PREBUF)
                ring_flush();
            continue;
        }
        int got = primed ? ring_pop(mono, SPK_FRAME) : 0;
        if (got < SPK_FRAME) memset(mono + got, 0, (size_t)(SPK_FRAME - got) * sizeof(int16_t));
        if (primed) {
            if (got > 0) {
                dry_ms = 0;
            } else if ((dry_ms += SPK_FRAME / (SAMPLE_RATE / 1000)) >= SPK_REARM_MS) {
                primed = false;
            }
        }
        bool real = false;
        for (int i = 0; i < got; i++) {
            if (mono[i] > 1 || mono[i] < -1) { real = true; break; }
        }
        if (real) {
            last_real = now;
            s_spk_last_real = now ? now : 1;
            if (!s_pa_on) pa_set(true);
        }
        /* mono16 -> stereo32: same sample on both slots, in the high 16 bits */
        for (int i = 0; i < SPK_FRAME; i++) {
            int32_t v = (int32_t)mono[i] * 65536;
            s_spk_frame[2 * i] = v;
            s_spk_frame[2 * i + 1] = v;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, s_spk_frame,
                                          SPK_FRAME * 2 * sizeof(int32_t), &written,
                                          pdMS_TO_TICKS(SPK_WRITE_TO_MS));
        if (err != ESP_OK) {
            if (last_warn == 0 || now - last_warn >= pdMS_TO_TICKS(1000)) {
                last_warn = now ? now : 1;
                ESP_LOGW(TAG, "board_audio: i2s_channel_write failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(20));   /* never spin on an instant failure */
        }
        if (s_pa_on && now - last_real >= pdMS_TO_TICKS(SPK_PA_HOLD_MS)) pa_set(false);
    }
}

static void speaker_fail(const char *step) {
    ESP_LOGE(TAG, "board_audio: speaker: %s failed — speaker off, mic unaffected", step);
    s_speaker_ok = false;
    if (s_io_exp) s_io_exp->write_output_reg(s_io_exp, 0xFFFF & ~(uint32_t)EXIO_PA_EN);
    s_pa_on = false;
}

/* ES8311 DAC + playback task, on the mic's (duplex) data interface. Opening
 * the OUT device reconfigures only the TX channel with the format the mic
 * already runs (same rate, 2 ch, 32-bit); RX is not touched. */
static void speaker_init(const audio_codec_data_if_t *data_if) {
    s_speaker_ok = false;
    s_ring = heap_caps_malloc(SPK_RING_SAMPLES * sizeof(int16_t),
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_spk_frame = heap_caps_malloc(SPK_FRAME * 2 * sizeof(int32_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_spk_mono = heap_caps_malloc(SPK_FRAME * sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_spk_wake = xSemaphoreCreateBinary();
    s_pa_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_spk_frame || !s_spk_mono || !s_spk_wake || !s_pa_lock) {
        speaker_fail("alloc");
        return;
    }

    /* ES8311: ONLY addr + bus_handle, like the ES7210 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_ADDR << 1,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) { speaker_fail("audio_codec_new_i2c_ctrl(es8311)"); return; }
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) { speaker_fail("audio_codec_new_gpio"); return; }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,         /* PA is on the TCA9555, driven here */
        .use_mclk = false,    /* the board clocks the ES8311 from BCLK */
        /* With use_mclk=false the driver derives the codec clock as BCLK x8.
         * Our slots are 32-bit stereo: BCLK = 64 fs = 1.024 MHz -> 8.192 MHz
         * = 512 fs. mclk_div 512 picks the 8.192 MHz coefficient row
         * (pre_div 2 -> 256 fs internally); the default 256 would set the
         * dividers for half the real clock. */
        .mclk_div = 512,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) { speaker_fail("es8311_codec_new"); return; }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_spk_dev = esp_codec_dev_new(&dev_cfg);
    if (s_spk_dev == NULL) { speaker_fail("esp_codec_dev_new(out)"); return; }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 32,
        .channel = 2,
        .sample_rate = SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_spk_dev, &fs) != ESP_CODEC_DEV_OK) {
        speaker_fail("esp_codec_dev_open(out)");
        return;
    }
    if (esp_codec_dev_set_out_vol(s_spk_dev, s_spk_volume) != ESP_CODEC_DEV_OK) {
        speaker_fail("set_out_vol");
        return;
    }
    if (xTaskCreatePinnedToCore(spk_task, "spk", SPK_TASK_STACK, NULL, SPK_TASK_PRIO,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        speaker_fail("spk task");
        return;
    }
    s_speaker_ok = true;
    ESP_LOGI(TAG, "board_audio: ES8311 up (16 kHz, vol %d, PA off until playback)",
             s_spk_volume);
}

void board_audio_play(const int16_t *pcm, int n) {
    if (pcm == NULL || n <= 0) return;
    if (!s_speaker_ok || !s_spk_enabled) return;   /* replies may play elsewhere */
    portENTER_CRITICAL(&s_ring_mux);
    int space = SPK_RING_SAMPLES - s_ring_count;
    int take = n < space ? n : space;
    int first = take < SPK_RING_SAMPLES - s_ring_head ? take : SPK_RING_SAMPLES - s_ring_head;
    memcpy(&s_ring[s_ring_head], pcm, (size_t)first * sizeof(int16_t));
    memcpy(s_ring, pcm + first, (size_t)(take - first) * sizeof(int16_t));
    s_ring_head = (s_ring_head + take) % SPK_RING_SAMPLES;
    s_ring_count += take;
    s_spk_overflow += (uint32_t)(n - take);
    portEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s_spk_wake);
}

void board_audio_speaker_set(bool enabled, int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_spk_volume = volume;
    s_spk_enabled = enabled;
    if (!enabled) {
        ring_flush();
        pa_set(false);
    }
    if (s_speaker_ok && esp_codec_dev_set_out_vol(s_spk_dev, volume) != ESP_CODEC_DEV_OK)
        ESP_LOGW(TAG, "board_audio: set_out_vol(%d) failed", volume);
    if (s_spk_wake) xSemaphoreGive(s_spk_wake);
    ESP_LOGI(TAG, "board_audio: speaker %s, vol %d%s", enabled ? "enabled" : "disabled",
             volume, s_speaker_ok ? "" : " (speaker not up)");
}

int board_audio_speaker_volume(void) { return s_spk_volume; }

bool board_audio_speaker_ok(void) { return s_speaker_ok; }

/* Half-duplex on this board: the speaker sits centimetres from the mics.
 * The node-side turn lock (media.cpp is_playing) follows audio ARRIVING; the
 * speaker plays it ~60 ms (prebuffer) + up to ~90 ms (DMA) later, and the lock
 * reopens in every pause between words. So the mic also stays silent while
 * the speaker is AUDIBLY playing, plus SPK_MIC_HOLD_MS for the DMA queue and
 * room tail. Keyed on audible frames played — never on the ring being
 * non-empty: decoded silence can stream continuously and would pin the mic
 * shut. */
static bool spk_audible(void) {
    if (!s_speaker_ok || !s_spk_enabled) return false;
    TickType_t t = s_spk_last_real;
    return t != 0 && xTaskGetTickCount() - t < pdMS_TO_TICKS(SPK_MIC_HOLD_MS);
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
    /* Speaker PA OFF: only the playback task turns it on, while real audio
     * plays (speaker enabled and ES8311 up). */
    err = esp_io_expander_set_level(s_io_exp, EXIO_PA_EN, 0);
    if (err != ESP_OK) return fail("tca9555 PA off", err);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* I2S full duplex: ESP32 is master and drives MCLK/BCLK/WS; RX (mic) and
     * TX (speaker) share the clocks. auto_clear (TX only): when the playback
     * task stops writing, the DMA sends zeros instead of looping stale audio. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
        },
    };
    /* In full duplex the RX side is slaved to TX's clocks, so a TX problem
     * would take the mic down with it. Any duplex step failing -> tear it
     * down and fall back to the mic-only (RX simplex) setup; the speaker
     * stays dead. NEVER disable TX, or close/reopen s_spk_dev, at runtime:
     * the mic would stall with it. */
    bool duplex = true;
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "board_audio: full duplex failed (%s) — mic only, speaker off",
                 esp_err_to_name(err));
        if (s_tx_chan) {
            i2s_channel_disable(s_tx_chan);   /* may not be enabled: ignore */
            i2s_del_channel(s_tx_chan);
            s_tx_chan = NULL;
        }
        if (s_rx_chan) {
            i2s_del_channel(s_rx_chan);
            s_rx_chan = NULL;
        }
        duplex = false;
        std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
        if (err != ESP_OK) return fail("i2s_new_channel", err);
        err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
        if (err != ESP_OK) return fail("i2s_channel_init_std_mode", err);
    }
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) return fail("i2s_channel_enable", err);

    /* esp_codec_dev data interface, shared by the mic (IN) and speaker (OUT)
     * devices (as waveshare-v1). With a duplex pair it would pick up the TX
     * handle from the RX one anyway (audio_codec_data_i2s.c, pair_chan). */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_PORT,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
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

    /* Speaker after the mic is up: its failure never touches the mic */
    if (duplex)
        speaker_init(data_if);
    else
        speaker_fail("full duplex");
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
    if (spk_audible()) memset(out, 0, (size_t)n * sizeof(int16_t));
#ifdef MIC_LEVEL_LOG
    mic_level_accumulate(out, n);
#endif
    return true;
}
