/**
 * @file app_audio.h
 * @brief Audible alert chime through the onboard ES8311 codec.
 *
 * Deliberately does not use the BSP's audio setup. `bsp_audio_init()` creates both speaker and
 * microphone I2S channels whose DMA buffers do not fit in internal memory once WiFi is running,
 * and the BSP aborts on that failure. This component creates a speaker-only channel with small
 * DMA buffers instead. See the audio section of docs/performance.md.
 *
 * Failure is never fatal: if audio cannot start, alerts stay visual only.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the speaker path and synthesise the chime.
 *
 * Call **before** WiFi starts, so the I2S DMA buffers are allocated while internal memory is
 * still available. Returns an error rather than aborting if anything fails.
 */
esp_err_t app_audio_init(void);

/** @brief Whether the speaker path came up. */
bool app_audio_available(void);

/**
 * @brief Play the alert chime, without blocking.
 *
 * Ignored when audio is unavailable, when the user has turned alert sound off, while a chime is
 * already playing, or within the cooldown after the previous one -- a value hovering around a
 * threshold must not machine-gun the speaker.
 */
void app_audio_chime(void);

#ifdef __cplusplus
}
#endif
