#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Play the startup chime via MAX98357A I2S DAC.
 *
 * Self-contained: initializes I2S, enables DAC, plays PCM data,
 * disables DAC, and fully tears down I2S. No persistent resources.
 * No-op on platforms where CONFIG_AUDIO_ENABLED is not set.
 */
esp_err_t audio_manager_play_startup(void);

#ifdef __cplusplus
}
#endif
