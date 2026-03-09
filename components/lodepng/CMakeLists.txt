idf_component_register(
    SRCS "lodepng.c"
    INCLUDE_DIRS "."
)

# lodepng uses a few compiler features that produce warnings on ESP-IDF's strict settings.
# Suppress warnings for this third-party code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)

# ESP32 memory optimization: disable features we don't need.
# We only need PNG decode (not encode), and only 32-bit RGBA output.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LODEPNG_NO_COMPILE_ENCODER    # We only decode PNG, never encode
    LODEPNG_NO_COMPILE_DISK       # No filesystem access (we pass buffers)
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS  # Skip tEXt, iTXt, etc. metadata
    LODEPNG_NO_COMPILE_CPP        # Pure C mode
)
