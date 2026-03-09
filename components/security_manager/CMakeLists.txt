idf_component_register(
    SRCS "security_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls nvs_flash esp_http_client json config_manager
)
