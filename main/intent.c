/* Intent dispatch — see intent.h. */
#include "intent.h"

#include <string.h>

#include "esp_log.h"
#include "hue.h"
#include "settings.h"
#include "switcher.h"

static const char *TAG = "intent";

#define DIM_STEP 64          /* of 254 */
#define AC_DEFAULT_TEMP 24   /* °C when turning on with no known state */

static const char *NAMES[INTENT_MAX] = {
    [INTENT_LIGHTS_ON] = "lights_on",
    [INTENT_LIGHTS_OFF] = "lights_off",
    [INTENT_LIGHTS_DIM] = "lights_dim",
    [INTENT_LIGHTS_BRIGHT] = "lights_bright",
    [INTENT_HEATER_ON] = "heater_on",
    [INTENT_HEATER_OFF] = "heater_off",
    [INTENT_PLUG_ON] = "plug_on",
    [INTENT_PLUG_OFF] = "plug_off",
    [INTENT_AC_ON] = "ac_on",
    [INTENT_AC_OFF] = "ac_off",
    [INTENT_AC_WARMER] = "ac_warmer",
    [INTENT_AC_COOLER] = "ac_cooler",
};

const char *intent_name(intent_t intent)
{
    return intent < INTENT_MAX ? NAMES[intent] : "?";
}

intent_t intent_from_name(const char *name)
{
    for (int i = 0; i < INTENT_MAX; i++)
        if (strcmp(NAMES[i], name) == 0) return (intent_t)i;
    return INTENT_MAX;
}

static esp_err_t need_hue(settings_t *cfg)
{
    if (cfg->hue_ip[0] && cfg->hue_user[0]) return ESP_OK;
    ESP_LOGW(TAG, "hue not configured/paired yet");
    return ESP_ERR_INVALID_STATE;
}

/* Type-1 devices (need_key) also require the discovery-provided login key. */
static esp_err_t need_slot(const sw_slot_t *slot, bool need_key,
                           const char *what)
{
    if (slot->ip[0] && slot->id[0] && (!need_key || slot->key[0]))
        return ESP_OK;
    ESP_LOGW(TAG, "no %s discovered/configured yet", what);
    return ESP_ERR_INVALID_STATE;
}

/* Breeze: read the current state, tweak it, write it back. */
static esp_err_t ac_request(settings_t *cfg, bool on, int temp_delta)
{
    esp_err_t err = need_slot(&cfg->breeze, false, "breeze");
    if (err != ESP_OK) return err;

    sw_breeze_state_t st;
    bool have_state = switcher_breeze_get_state(cfg->breeze.ip,
                                                cfg->breeze.id, &st) == ESP_OK;

    br_request_t req = {
        .on = on,
        .mode = have_state && st.mode >= BR_MODE_AUTO &&
                        st.mode <= BR_MODE_HEAT
                    ? (br_mode_t)st.mode
                    : BR_MODE_COOL,
        .target_temp =
            (have_state && st.target_temp > 0 ? st.target_temp
                                              : AC_DEFAULT_TEMP) + temp_delta,
        .fan_level = have_state ? st.fan_level : '0',
        .swing = have_state && st.swing == '1',
    };
    return switcher_breeze_set(cfg->breeze.ip, cfg->breeze.id, &req);
}

esp_err_t intent_execute(intent_t intent)
{
    settings_t *cfg = settings();
    esp_err_t err;

    ESP_LOGI(TAG, "executing %s", intent_name(intent));

    switch (intent) {
    case INTENT_LIGHTS_ON:
    case INTENT_LIGHTS_OFF:
        if ((err = need_hue(cfg)) != ESP_OK) return err;
        return hue_set_group(cfg->hue_ip, cfg->hue_user, cfg->hue_group,
                             intent == INTENT_LIGHTS_ON);

    case INTENT_LIGHTS_DIM:
    case INTENT_LIGHTS_BRIGHT:
        if ((err = need_hue(cfg)) != ESP_OK) return err;
        return hue_group_dim(cfg->hue_ip, cfg->hue_user, cfg->hue_group,
                             intent == INTENT_LIGHTS_BRIGHT ? DIM_STEP
                                                            : -DIM_STEP);

    case INTENT_HEATER_ON:
    case INTENT_HEATER_OFF:
        if ((err = need_slot(&cfg->heater, true, "heater")) != ESP_OK)
            return err;
        return switcher_turn(cfg->heater.ip, cfg->heater.id, cfg->heater.key,
                             intent == INTENT_HEATER_ON, 0);

    case INTENT_PLUG_ON:
    case INTENT_PLUG_OFF:
        if ((err = need_slot(&cfg->plug, true, "plug")) != ESP_OK)
            return err;
        return switcher_turn(cfg->plug.ip, cfg->plug.id, cfg->plug.key,
                             intent == INTENT_PLUG_ON, 0);

    case INTENT_AC_ON:
        return ac_request(cfg, true, 0);
    case INTENT_AC_OFF:
        return ac_request(cfg, false, 0);
    case INTENT_AC_WARMER:
        return ac_request(cfg, true, +1);
    case INTENT_AC_COOLER:
        return ac_request(cfg, true, -1);

    default:
        return ESP_ERR_INVALID_ARG;
    }
}
