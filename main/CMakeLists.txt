idf_component_register(
    SRCS "app_main.c" "settings.c" "devices.c" "intent.c" "wifi_mgr.c"
         "webui.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                  esp_timer spiffs json hue switcher voice)
