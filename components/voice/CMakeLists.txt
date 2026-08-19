if(IDF_TARGET STREQUAL "esp32s3")
    idf_component_register(
        SRCS "voice.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_driver_i2s)
else()
    # No ESP-SR on this target; voice.c compiles to the stub.
    idf_component_register(
        SRCS "voice.c"
        INCLUDE_DIRS "include")
endif()
