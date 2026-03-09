idf_component_register(
    SRCS "diagnostics.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config_manager wifi_manager power_manager board esp_wifi esp_netif esp_http_client esp_websocket_client lwip
)

# Make firmware version available as APP_VERSION macro.
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION="${PROJECT_VER}")
