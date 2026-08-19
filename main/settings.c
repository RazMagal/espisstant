/* NVS-backed settings — see settings.h. */
#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
static const char *NVS_NS = "espisstant";
static const char *NVS_KEY = "settings_v1";

static settings_t s_settings;

void settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_settings);
        if (nvs_get_blob(h, NVS_KEY, &s_settings, &len) != ESP_OK ||
            len != sizeof(s_settings)) {
            memset(&s_settings, 0, sizeof(s_settings));
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "wifi=%s hue=%s heater=%s plug=%s breeze=%s",
             s_settings.wifi_ssid[0] ? s_settings.wifi_ssid : "(unset)",
             s_settings.hue_ip[0] ? s_settings.hue_ip : "(unset)",
             s_settings.heater.id[0] ? s_settings.heater.id : "-",
             s_settings.plug.id[0] ? s_settings.plug.id : "-",
             s_settings.breeze.id[0] ? s_settings.breeze.id : "-");
}

settings_t *settings(void)
{
    return &s_settings;
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, &s_settings, sizeof(s_settings));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}
