idf_component_register(
    SRCS "timezone_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client mbedtls json config_manager
)
