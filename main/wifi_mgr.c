/* Wi-Fi manager — see wifi_mgr.h. */
#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "settings.h"

static const char *TAG = "wifi";

#define AP_SSID "Espisstant-Setup"
#define AP_PASS "espisstant"
/* Give up and open the setup portal only if we never managed to join. */
#define STA_INITIAL_RETRY 15
#define STA_JOIN_TIMEOUT_MS 90000
#define HOSTNAME "espisstant"

static EventGroupHandle_t s_events;
#define BIT_GOT_IP BIT0
#define BIT_STA_FAILED BIT1

static bool s_ap_mode;
static bool s_ever_connected;
static int s_retries;
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_ever_connected && s_retries >= STA_INITIAL_RETRY) {
            /* Never joined — likely bad credentials; let wifi_mgr_start()
             * fall back to the setup portal. */
            xEventGroupSetBits(s_events, BIT_STA_FAILED);
            return;
        }
        /* Once provisioned, retry forever with backoff (1 s .. 32 s).
         * Scheduled on a timer: never block the event-loop task. */
        int shift = s_retries < 5 ? s_retries : 5;
        s_retries++;
        uint64_t delay_us = (uint64_t)(1000 << shift) * 1000;
        esp_timer_start_once(s_reconnect_timer, delay_us);
        ESP_LOGI(TAG, "disconnected, retrying in %d s",
                 (int)(delay_us / 1000000));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_ever_connected = true;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static void start_mdns(void)
{
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set("Espisstant voice assistant");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "reachable at http://%s.local", HOSTNAME);
    }
}

static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_mode = true;
    ESP_LOGI(TAG, "setup portal: connect to '%s' (pass '%s'), open "
                  "http://192.168.4.1",
             AP_SSID, AP_PASS);
}

void wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();

    const esp_timer_create_args_t targs = {
        .callback = reconnect_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi_event, NULL));

    settings_t *cfg = settings();
    if (cfg->wifi_ssid[0] == '\0') {
        start_ap();
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta = { 0 };
    /* memcpy, not strlcpy: a full 32-char SSID / 64-char PSK is legal and
     * needs no terminator in wifi_sta_config_t. */
    memcpy(sta.sta.ssid, cfg->wifi_ssid,
           strnlen(cfg->wifi_ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, cfg->wifi_pass,
           strnlen(cfg->wifi_pass, sizeof(sta.sta.password)));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Bounded wait: associated-but-no-DHCP would otherwise hang us here
     * forever with no web UI to reconfigure. */
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_GOT_IP | BIT_STA_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(STA_JOIN_TIMEOUT_MS));

    if (bits & BIT_GOT_IP) {
        start_mdns();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    } else {
        ESP_LOGW(TAG, "could not join '%s', falling back to setup portal",
                 cfg->wifi_ssid);
        esp_timer_stop(s_reconnect_timer);
        ESP_ERROR_CHECK(esp_wifi_stop());
        start_ap();
    }
}

bool wifi_in_ap_mode(void)
{
    return s_ap_mode;
}
