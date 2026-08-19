/* The intent layer: one enum every input path (voice, web UI, HTTP API)
 * funnels into, dispatched to Hue / Switcher actions. */
#ifndef INTENT_H
#define INTENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTENT_LIGHTS_ON,
    INTENT_LIGHTS_OFF,
    INTENT_LIGHTS_DIM,
    INTENT_LIGHTS_BRIGHT,
    INTENT_HEATER_ON,
    INTENT_HEATER_OFF,
    INTENT_PLUG_ON,
    INTENT_PLUG_OFF,
    INTENT_AC_ON,
    INTENT_AC_OFF,
    INTENT_AC_WARMER,
    INTENT_AC_COOLER,
    INTENT_MAX,
} intent_t;

const char *intent_name(intent_t intent);

/* Returns INTENT_MAX for an unknown name. */
intent_t intent_from_name(const char *name);

esp_err_t intent_execute(intent_t intent);

#ifdef __cplusplus
}
#endif

#endif /* INTENT_H */
