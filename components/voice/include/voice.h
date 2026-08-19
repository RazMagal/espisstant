/*
 * Offline voice pipeline: I2S mic -> AFE -> WakeNet ("Hi ESP") ->
 * MultiNet English command recognition (ESP-SR, ESP32-S3 only).
 *
 * On other targets voice_start() returns ESP_ERR_NOT_SUPPORTED.
 */
#ifndef VOICE_H
#define VOICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the voice task with the intent name of the recognized
 * command (see voice_commands.h). Keep it quick or hand off to a queue. */
typedef void (*voice_intent_cb_t)(const char *intent_name);

esp_err_t voice_start(voice_intent_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_H */
