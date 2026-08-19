idf_component_register(
    SRCS "hue.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client json lwip esp_timer)
