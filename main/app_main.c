/* Espisstant — application entry point. */
#include <stdio.h>

#include "breeze_remote.h"
#include "devices.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "intent.h"
#include "nvs_flash.h"
#include "settings.h"
#include "switcher.h"
#include "webui.h"
#include "wifi_mgr.h"

#if CONFIG_ESPISSTANT_VOICE
#include "voice.h"
#endif

static const char *TAG = "app";

static void mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "storage mount failed: %s", esp_err_to_name(err));
}

#if CONFIG_ESPISSTANT_VOICE
/* Voice commands are executed on a worker task: intent_execute() does
 * blocking network I/O (seconds), which must never stall the ESP-SR
 * detect task or audio frames get dropped. */
static QueueHandle_t s_intent_q;

static void intent_worker(void *pv)
{
    intent_t intent;
    for (;;) {
        if (xQueueReceive(s_intent_q, &intent, portMAX_DELAY) == pdTRUE)
            intent_execute(intent);
    }
}

static void on_voice_intent(const char *name)
{
    intent_t intent = intent_from_name(name);
    if (intent == INTENT_MAX) return;
    if (xQueueSend(s_intent_q, &intent, 0) != pdTRUE)
        ESP_LOGW(TAG, "intent queue full, dropping %s", name);
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_init();
    devices_init();
    mount_storage();
    br_load("/spiffs/breeze_remote.json"); /* optional until Breeze is used */

    wifi_mgr_start();
    ESP_ERROR_CHECK(webui_start());

    if (!wifi_in_ap_mode()) {
        ESP_ERROR_CHECK(switcher_start_discovery(devices_on_discovered, NULL));
#if CONFIG_ESPISSTANT_VOICE
        s_intent_q = xQueueCreate(4, sizeof(intent_t));
        configASSERT(s_intent_q);
        xTaskCreate(intent_worker, "intent_worker", 8192, NULL, 5, NULL);
        if (voice_start(on_voice_intent) != ESP_OK)
            ESP_LOGE(TAG, "voice pipeline failed to start");
#endif
    }

    ESP_LOGI(TAG, "espisstant ready");
}
