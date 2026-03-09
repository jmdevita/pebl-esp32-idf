idf_component_register(
    SRCS "wifi_manager.cpp" "wifi_credential_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash config_manager
)
