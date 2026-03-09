idf_component_register(
    SRCS "pairing_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp_timer json config_manager display_manager mbedtls
)
