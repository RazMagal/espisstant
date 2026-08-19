/* Switcher discovery cache — see devices.h. */
#include "devices.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

static const char *TAG = "devices";

/* Entries older than this may be evicted when the table is full. */
#define STALE_US (5LL * 60 * 1000000)

static seen_dev_t s_seen[DEVICES_SEEN_MAX];
static int s_seen_count;
static SemaphoreHandle_t s_lock;

void devices_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
}

/* Update a role slot: claim it if empty, refresh the IP if the id matches. */
static bool sync_slot(sw_slot_t *slot, const sw_discovered_t *dev)
{
    bool changed = false;
    if (slot->id[0] == '\0') {
        strlcpy(slot->id, dev->device_id, sizeof(slot->id));
        changed = true;
    }
    if (strcmp(slot->id, dev->device_id) != 0) return false;
    if (strcmp(slot->ip, dev->ip) != 0) {
        strlcpy(slot->ip, dev->ip, sizeof(slot->ip));
        changed = true;
    }
    if (!dev->is_type2 && strcmp(slot->key, dev->device_key) != 0) {
        strlcpy(slot->key, dev->device_key, sizeof(slot->key));
        changed = true;
    }
    return changed;
}

void devices_on_discovered(const sw_discovered_t *dev, void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Refresh the cache entry, append, or evict the stalest full slot. */
    int i;
    for (i = 0; i < s_seen_count; i++)
        if (strcmp(s_seen[i].info.device_id, dev->device_id) == 0) break;
    if (i == s_seen_count) {
        if (s_seen_count < DEVICES_SEEN_MAX) {
            s_seen_count++;
        } else {
            int oldest = 0;
            for (int j = 1; j < DEVICES_SEEN_MAX; j++)
                if (s_seen[j].last_seen_us < s_seen[oldest].last_seen_us)
                    oldest = j;
            i = now - s_seen[oldest].last_seen_us > STALE_US
                    ? oldest
                    : DEVICES_SEEN_MAX; /* table full of live devices */
        }
    }
    if (i < DEVICES_SEEN_MAX) {
        bool is_new =
            strcmp(s_seen[i].info.device_id, dev->device_id) != 0 ||
            s_seen[i].last_seen_us == 0;
        s_seen[i].info = *dev;
        s_seen[i].last_seen_us = now;
        if (is_new)
            ESP_LOGI(TAG, "heard %s '%s' (model %s) at %s", dev->device_id,
                     dev->name, dev->type_hex, dev->ip);
    }

    /* Route into a role slot by model. */
    settings_t *cfg = settings();
    bool changed;
    if (dev->is_type2)
        changed = strcmp(dev->type_hex, "0e01") == 0 /* breeze */
                      ? sync_slot(&cfg->breeze, dev)
                      : false; /* runners etc: web-UI only, no role */
    else if (strcmp(dev->type_hex, "01a8") == 0) /* power plug */
        changed = sync_slot(&cfg->plug, dev);
    else /* heater / boiler family */
        changed = sync_slot(&cfg->heater, dev);

    xSemaphoreGive(s_lock);

    /* NVS commit can take hundreds of ms — never inside the lock. */
    if (changed) settings_save();
}

int devices_seen(seen_dev_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_seen_count < max ? s_seen_count : max;
    memcpy(out, s_seen, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}
