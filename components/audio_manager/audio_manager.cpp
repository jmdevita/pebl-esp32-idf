/**
 * Audio Manager — plays a startup sound via MAX98357A I2S DAC.
 *
 * The startup sound is a "water droplet" — a sine wave that sweeps downward
 * in frequency with exponential amplitude decay, evoking a pebble dropping
 * into water. On-brand for Pebl.
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

#define SAMPLE_RATE     16000

/* Water droplet synthesis parameters.
 * Models a pebble hitting water: sine wave with downward frequency sweep
 * and exponential amplitude decay. Two droplets — a primary "plop" and
 * a quieter secondary ripple — give it a natural feel. */

#define DROP1_SAMPLES   2400    /* Primary droplet: 150ms */
#define DROP1_FREQ_START 800.0f /* Start frequency (Hz) — initial impact */
#define DROP1_FREQ_END   200.0f /* End frequency (Hz) — pitch drops as energy dissipates */
#define DROP1_DECAY      6.0f   /* Exponential decay rate — fast fadeout */
#define DROP1_AMPLITUDE  10000  /* Volume (max 32767) */

#define DROP2_SAMPLES   1600    /* Secondary ripple: 100ms */
#define DROP2_FREQ_START 600.0f /* Slightly higher pitch — smaller bubble */
#define DROP2_FREQ_END   150.0f
#define DROP2_DECAY      8.0f   /* Faster decay — quieter, shorter */
#define DROP2_AMPLITUDE  5000   /* Half volume of primary */

#define GAP_SAMPLES     1200    /* 75ms silence between droplets */
#define TAIL_SAMPLES    1600    /* 100ms trailing silence for DMA flush */

#define TOTAL_SAMPLES   (DROP1_SAMPLES + GAP_SAMPLES + DROP2_SAMPLES + TAIL_SAMPLES)

/**
 * Generate a single water droplet sound: a sine wave with downward
 * frequency sweep and exponential amplitude decay.
 *
 * The frequency sweeps logarithmically from freq_start to freq_end,
 * while amplitude decays exponentially. This models the physics of
 * a bubble oscillating and losing energy after a pebble breaks the
 * water surface.
 */
static int generate_droplet(int16_t *buf, int offset, int num_samples,
                            float freq_start, float freq_end,
                            float decay_rate, int amplitude)
{
    float phase = 0.0f;

    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / num_samples;  /* 0.0 → 1.0 */

        /* Logarithmic frequency sweep: starts fast, slows down */
        float freq = freq_start * powf(freq_end / freq_start, t);

        /* Accumulate phase for continuous waveform (avoids clicks from
         * changing frequency mid-cycle) */
        phase += 2.0f * M_PI * freq / SAMPLE_RATE;

        /* Exponential amplitude decay */
        float envelope = expf(-decay_rate * t);

        buf[offset + i] = (int16_t)(sinf(phase) * envelope * amplitude);
    }
    return offset + num_samples;
}

esp_err_t audio_manager_play_startup(void)
{
    ESP_LOGI(TAG, "Playing startup sound (I2S DOUT=%d BCLK=%d LRCLK=%d SD_MODE=%d)",
             CONFIG_AUDIO_I2S_DOUT, CONFIG_AUDIO_I2S_BCLK,
             CONFIG_AUDIO_I2S_LRCLK, CONFIG_AUDIO_SD_MODE_GPIO);

    /* Generate PCM droplet data */
    int16_t *pcm = (int16_t *)calloc(TOTAL_SAMPLES, sizeof(int16_t));
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer (%d bytes)", (int)(TOTAL_SAMPLES * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    int pos = 0;
    pos = generate_droplet(pcm, pos, DROP1_SAMPLES,
                           DROP1_FREQ_START, DROP1_FREQ_END,
                           DROP1_DECAY, DROP1_AMPLITUDE);
    pos += GAP_SAMPLES;  /* silence gap (already zeroed by calloc) */
    pos = generate_droplet(pcm, pos, DROP2_SAMPLES,
                           DROP2_FREQ_START, DROP2_FREQ_END,
                           DROP2_DECAY, DROP2_AMPLITUDE);
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

    ESP_LOGI(TAG, "Startup sound complete");
    return ESP_OK;
}

#else /* !CONFIG_AUDIO_ENABLED */

esp_err_t audio_manager_play_startup(void) { return ESP_OK; }

#endif /* CONFIG_AUDIO_ENABLED */
