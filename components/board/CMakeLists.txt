idf_component_register(
    SRCS "board.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_adc esp_hw_support esp_pm
)
