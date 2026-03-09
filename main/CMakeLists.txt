idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        config_manager
        wifi_manager
        diagnostics
        websocket_manager
        display_manager
        security_manager
        ota_manager
        power_manager
        resilience_manager
        pairing_manager
        timezone_manager
        board
        nvs_flash
        esp_event
        esp_pm
        esp_wifi
        esp_hw_support
        esp_timer
        esp_http_client
        driver
)

# Make firmware version available for diagnostics WiFi-fail path.
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION="${PROJECT_VER}")
