idf_component_register(
    SRCS "display_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES diagnostics
    PRIV_REQUIRES driver config_manager board power_manager lodepng CalEPD Adafruit-GFX
)

# Make firmware version available to splash screen as APP_VERSION macro.
# PROJECT_VER is set by ESP-IDF from version.txt in the project root.
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION="${PROJECT_VER}")
