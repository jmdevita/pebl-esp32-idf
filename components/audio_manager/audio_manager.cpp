/**
 * Audio Manager — plays a startup chime via MAX98357A I2S DAC.
 *
 * Self-contained: initializes I2S, plays, tears down. No persistent resources.
 * Called once during boot before light sleep is enabled, so no sleep impact.
 *
 * The MAX98357A is kept in shutdown (SD_MODE LOW) except during playback
 * to save ~2mA quiescent current.
 *
 * Entire component compiles to no-op when CONFIG_AUDIO_ENABLED is not set
 * (i.e., on ESP32 / LilyGo T5 builds).
 */

#include "audio_manager.h"

#if CONFIG_AUDIO_ENABLED

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "AUDIO";

/* Chime parameters: 3 ascending notes, short and pleasant */
#define SAMPLE_RATE     16000
#define NOTE_DURATION   2400    /* samples per note = 150ms at 16kHz */
#define SILENCE_GAP     800     /* samples between notes = 50ms */
#define FADE_SAMPLES    400     /* fade in/out to avoid clicks = 25ms */
#define AMPLITUDE       8000    /* volume level (max 32767 for int16_t) */

/* Three ascending notes: C5, E5, G5 (major chord) */
static const float NOTE_FREQS[] = { 523.25f, 659.25f, 783.99f };
#define NUM_NOTES (sizeof(NOTE_FREQS) / sizeof(NOTE_FREQS[0]))

/* Total PCM buffer size: 3 notes + 2 gaps + trailing silence */
#define TOTAL_SAMPLES ((NOTE_DURATION * NUM_NOTES) + (SILENCE_GAP * (NUM_NOTES - 1)) + SAMPLE_RATE / 10)

/**
 * Generate a sine wave note with fade-in/fade-out envelope.
 * Writes samples starting at buf[offset].
 */
static int generate_note(int16_t *buf, int offset, float freq_hz)
{
    for (int i = 0; i < NOTE_DURATION; i++) {
        /* Sine wave */
        float sample = sinf(2.0f * M_PI * freq_hz * (float)i / SAMPLE_RATE);

        /* Fade envelope to avoid clicks */
        float envelope = 1.0f;
        if (i < FADE_SAMPLES) {
            envelope = (float)i / FADE_SAMPLES;
        } else if (i > NOTE_DURATION - FADE_SAMPLES) {
            envelope = (float)(NOTE_DURATION - i) / FADE_SAMPLES;
        }

        buf[offset + i] = (int16_t)(sample * envelope * AMPLITUDE);
    }
    return offset + NOTE_DURATION;
}

esp_err_t audio_manager_play_startup(void)
{
    ESP_LOGI(TAG, "Playing startup chime (I2S DOUT=%d BCLK=%d LRCLK=%d SD_MODE=%d)",
             CONFIG_AUDIO_I2S_DOUT, CONFIG_AUDIO_I2S_BCLK,
             CONFIG_AUDIO_I2S_LRCLK, CONFIG_AUDIO_SD_MODE_GPIO);

    /* Generate PCM chime data on the stack/heap */
    int16_t *pcm = (int16_t *)calloc(TOTAL_SAMPLES, sizeof(int16_t));
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer (%d bytes)", (int)(TOTAL_SAMPLES * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    int pos = 0;
    for (int n = 0; n < (int)NUM_NOTES; n++) {
        pos = generate_note(pcm, pos, NOTE_FREQS[n]);
        if (n < (int)NUM_NOTES - 1) {
            pos += SILENCE_GAP;  /* silence gap (already zeroed by calloc) */
        }
    }
    /* Trailing silence already zeroed by calloc */

    /* Configure I2S TX channel */
    i2s_chan_handle_t tx_handle = NULL;

    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        free(pcm);
        return ret;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)CONFIG_AUDIO_I2S_BCLK;
    std_cfg.gpio_cfg.ws = (gpio_num_t)CONFIG_AUDIO_I2S_LRCLK;
    std_cfg.gpio_cfg.dout = (gpio_num_t)CONFIG_AUDIO_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        free(pcm);
        return ret;
    }

    /* Enable MAX98357A DAC (SD_MODE HIGH) */
    gpio_config_t sd_cfg = {};
    sd_cfg.pin_bit_mask = 1ULL << CONFIG_AUDIO_SD_MODE_GPIO;
    sd_cfg.mode = GPIO_MODE_OUTPUT;
    ret = gpio_config(&sd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        free(pcm);
        return ret;
    }
    gpio_set_level((gpio_num_t)CONFIG_AUDIO_SD_MODE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));  /* DAC startup time */

    /* Start I2S and write PCM data */
    i2s_channel_enable(tx_handle);

    size_t bytes_written = 0;
    size_t total_bytes = TOTAL_SAMPLES * sizeof(int16_t);
    ret = i2s_channel_write(tx_handle, pcm, total_bytes, &bytes_written, pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_write returned %s (wrote %d/%d bytes)",
                 esp_err_to_name(ret), (int)bytes_written, (int)total_bytes);
    }

    /* Flush DMA: write silence to push remaining samples through */
    int16_t silence[240] = {};
    i2s_channel_write(tx_handle, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(500));

    /* Wait for DMA to drain before shutting down */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Tear down: disable DAC, stop I2S, free resources */
    i2s_channel_disable(tx_handle);
    gpio_set_level((gpio_num_t)CONFIG_AUDIO_SD_MODE_GPIO, 0);  /* DAC shutdown */
    i2s_del_channel(tx_handle);
    free(pcm);

    ESP_LOGI(TAG, "Startup chime complete");
    return ESP_OK;
}

#else /* !CONFIG_AUDIO_ENABLED */

esp_err_t audio_manager_play_startup(void) { return ESP_OK; }

#endif /* CONFIG_AUDIO_ENABLED */
