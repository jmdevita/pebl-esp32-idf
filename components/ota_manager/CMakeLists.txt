idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_https_ota app_update esp_http_client esp_partition mbedtls json config_manager nvs_flash
)
