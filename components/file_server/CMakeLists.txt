idf_component_register(
    SRCS "file_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server vfs
)