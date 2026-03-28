idf_component_register(
    SRCS "led_manager.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver espressif__led_strip
)
