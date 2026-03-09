idf_component_register(
    SRCS "config_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash littlefs json esp_hw_support
)
