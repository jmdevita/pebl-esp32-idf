#!/bin/bash
# Setup display library dependencies for ESP-IDF build.
# Run once after cloning the repo:
#   cd esp32_idf_client && ./setup_display_deps.sh
#
# Downloads CalEPD (e-paper driver) and Adafruit-GFX (graphics)
# into components/ as local components. These are not in the ESP Component Registry
# so they can't be pulled via idf_component.yml.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COMPONENTS_DIR="$SCRIPT_DIR/components"

echo "=== Setting up display dependencies ==="

# CalEPD — ESP-IDF native e-paper library with Adafruit GFX API
if [ -d "$COMPONENTS_DIR/CalEPD" ]; then
    echo "CalEPD already exists, updating..."
    cd "$COMPONENTS_DIR/CalEPD" && git pull
else
    echo "Cloning CalEPD..."
    git clone --depth 1 https://github.com/martinberlin/CalEPD.git "$COMPONENTS_DIR/CalEPD"
fi

# Adafruit-GFX — CalEPD dependency (fonts, text rendering, GFX primitives)
if [ -d "$COMPONENTS_DIR/Adafruit-GFX" ]; then
    echo "Adafruit-GFX-Library already exists, updating..."
    cd "$COMPONENTS_DIR/Adafruit-GFX" && git pull
else
    echo "Cloning Adafruit-GFX..."
    git clone --depth 1 https://github.com/martinberlin/Adafruit-GFX.git "$COMPONENTS_DIR/Adafruit-GFX"
fi

# lodepng — PNG decoder (already included but verify)
if [ ! -f "$COMPONENTS_DIR/lodepng/lodepng.c" ]; then
    echo "Downloading lodepng..."
    cd "$COMPONENTS_DIR/lodepng" && ./download_lodepng.sh
fi

echo ""
echo "=== Display dependencies ready ==="
echo "  CalEPD:        $COMPONENTS_DIR/CalEPD"
echo "  Adafruit GFX:  $COMPONENTS_DIR/Adafruit-GFX"
echo "  lodepng:       $COMPONENTS_DIR/lodepng"
echo ""
echo "Next steps:"
echo "  1. idf.py menuconfig  → Configure 'PEBL Display' variant and 'Display Configuration' pins"
echo "  2. idf.py build"
