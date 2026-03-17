idf_component_register(
    SRCS "weather_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client mbedtls json
)