idf_component_register(
    SRCS "power_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board config_manager wifi_manager timezone_manager driver esp_timer esp_hw_support esp_pm esp_wifi soc
)
