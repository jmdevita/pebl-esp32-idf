idf_component_register(
    SRCS "websocket_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_websocket_client json esp_http_client config_manager security_manager resilience_manager display_manager wifi_manager
)
