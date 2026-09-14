/*
 * Alert chime. See app_audio.h.
 *
 * Memory is the whole design constraint here. On this board, once WiFi runs, the largest free
 * internal DMA block has measured under 3KB. So:
 *   - one speaker-only I2S channel, two 512-byte DMA buffers (~1KB), not the BSP's
 *     speaker-plus-microphone pair of six 480-byte buffers each (~5.8KB);
 *   - initialised before WiFi claims its share of internal memory;
 *   - the chime is synthesised once into PSRAM, never into a static (internal) array;
 *   - the playback task's stack lives in PSRAM where possible.
 */

#include "app_audio.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "bsp/esp-bsp.h"

#include "app_settings.h"

static const char *TAG = "app_audio";

#define SAMPLE_RATE_HZ     16000
#define DMA_DESC_NUM       2
#define DMA_FRAME_NUM      256
#define OUTPUT_VOLUME      70
#define CHIME_COOLDOWN_MS  3000
#define CHIME_AMPLITUDE    7000.0f

static struct {
    bool                   ready;
    i2s_chan_handle_t      tx;
    esp_codec_dev_handle_t spk;
    int16_t               *pcm;       /**< PSRAM. */
    size_t                 pcm_bytes;
    TaskHandle_t           task;
    int64_t                last_chime_us;
    volatile bool          playing;
} s;

/* Two descending notes with a fast attack and exponential decay, so it reads as a chime rather
 * than a beep, and starts and ends at zero to avoid clicks. */
static esp_err_t synthesise_chime(void)
{
    const struct {
        float freq_hz;
        float dur_s;
    } notes[] = {
        {1318.5f, 0.12f}, /* E6 */
        {0.0f,    0.03f}, /* gap */
        {987.8f,  0.22f}, /* B5 */
    };

    size_t total = 0;
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        total += (size_t)(notes[n].dur_s * SAMPLE_RATE_HZ);
    }

    s.pcm_bytes = total * sizeof(int16_t);
    s.pcm       = heap_caps_calloc(total, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s.pcm != NULL, ESP_ERR_NO_MEM, TAG, "no PSRAM for chime");

    size_t pos = 0;
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        size_t count  = (size_t)(notes[n].dur_s * SAMPLE_RATE_HZ);
        size_t attack = SAMPLE_RATE_HZ / 200; /* 5ms */

        for (size_t i = 0; i < count; i++, pos++) {
            if (notes[n].freq_hz <= 0.0f) {
                continue; /* calloc left it silent */
            }
            float t        = (float)i / SAMPLE_RATE_HZ;
            float envelope = (i < attack) ? (float)i / (float)attack
                                          : exp(-4.0f * (float)(i - attack) / (float)count);
            /* Taper the last 5ms to zero regardless of the decay curve. */
            if (count - i < attack) {
                envelope *= (float)(count - i) / (float)attack;
            }
            s.pcm[pos] = (int16_t)(CHIME_AMPLITUDE * envelope *
                                   sinf(2.0f * (float)M_PI * notes[n].freq_hz * t));
        }
    }

    return ESP_OK;
}

static void chime_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s.playing = true;
        ESP_LOGI(TAG, "chime");
        int rc = esp_codec_dev_write(s.spk, s.pcm, (int)s.pcm_bytes);
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "chime write failed (%d)", rc);
        }
        s.playing = false;
    }
}

esp_err_t app_audio_init(void)
{
    if (s.ready) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    size_t   internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t dma_before      = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    /* The display and touch have already brought up the shared I2C bus; this is idempotent. */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");

    /* --- speaker-only I2S channel with small DMA buffers --- */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num      = DMA_DESC_NUM;
    chan.dma_frame_num     = DMA_FRAME_NUM;
    chan.auto_clear        = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan, &s.tx, NULL), fail, TAG, "i2s_new_channel");

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s.tx, &std), fail, TAG, "i2s std mode");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s.tx), fail, TAG, "i2s enable");

    /* --- ES8311 codec over the shared I2C bus, output only --- */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = s.tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_GOTO_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, fail, TAG, "i2s data interface");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, fail, TAG, "gpio interface");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BSP_I2C_NUM,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_GOTO_ON_FALSE(ctrl_if != NULL, ESP_ERR_NO_MEM, fail, TAG, "i2c control interface");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage        = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_GOTO_ON_FALSE(codec_if != NULL, ESP_FAIL, fail, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s.spk = esp_codec_dev_new(&dev_cfg);
    ESP_GOTO_ON_FALSE(s.spk != NULL, ESP_ERR_NO_MEM, fail, TAG, "codec device");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE_HZ,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(s.spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, fail, TAG,
                      "codec open");
    esp_codec_dev_set_out_vol(s.spk, OUTPUT_VOLUME);

    ESP_GOTO_ON_ERROR(synthesise_chime(), fail, TAG, "synthesise");

    /* Stack in PSRAM: internal memory is the scarce resource. Fall back to internal if needed. */
    if (xTaskCreatePinnedToCoreWithCaps(chime_task, "chime", 3072, NULL, 2, &s.task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "PSRAM task stack unavailable; using internal memory");
        ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(chime_task, "chime", 2560, NULL, 2, &s.task, 0)
                              == pdPASS,
                          ESP_ERR_NO_MEM, fail, TAG, "chime task");
    }

    s.ready = true;

    ESP_LOGI(TAG, "speaker ready: %d Hz, chime %u bytes in PSRAM | internal %u -> %u B, "
                  "largest DMA block %" PRIu32 " -> %u B",
             SAMPLE_RATE_HZ, (unsigned)s.pcm_bytes, (unsigned)internal_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), dma_before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return ESP_OK;

fail:
    /*
     * Leave the gauge running without sound. The codec interface objects are small and the
     * failure is expected to be permanent for this boot, so only the large resources are
     * released.
     */
    if (s.tx != NULL) {
        i2s_channel_disable(s.tx);
        i2s_del_channel(s.tx);
        s.tx = NULL;
    }
    if (s.pcm != NULL) {
        heap_caps_free(s.pcm);
        s.pcm = NULL;
    }
    s.spk = NULL;
    ESP_LOGW(TAG, "audio unavailable (%s); alerts will be visual only", esp_err_to_name(ret));
    return ret;
}

bool app_audio_available(void)
{
    return s.ready;
}

void app_audio_chime(void)
{
    if (!s.ready || s.playing || !app_settings_get()->alert_sound) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s.last_chime_us != 0 && (now - s.last_chime_us) < (int64_t)CHIME_COOLDOWN_MS * 1000) {
        return;
    }

    s.last_chime_us = now;
    xTaskNotifyGive(s.task);
}
