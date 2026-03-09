#!/bin/bash
# Flash ESP32 firmware and config to a device (ESP-IDF version)
# Usage: ./flash.sh [--dry-run] [--erase]
#
# Equivalent to esp32_arduino_client/flash.sh but uses idf.py instead of PlatformIO.
# Display variant is compiled in via Kconfig — changing variant requires a rebuild.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
CONFIG_FILE="$DATA_DIR/config.json"
CONFIG_EXAMPLE="$DATA_DIR/config.json.example"

# ============================================================================
# Editable defaults — change these for your setup
# ============================================================================
DEFAULT_NAME="pebl"
DEFAULT_ROTATION=1
# ============================================================================

# ESP-IDF environment
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

# Parse arguments
DRY_RUN=false
ERASE=false
FLASH_CONFIG=false
for arg in "$@"; do
    case "$arg" in
        --dry-run)      DRY_RUN=true ;;
        --erase)        ERASE=true; FLASH_CONFIG=true ;;
        --flash-config) FLASH_CONFIG=true ;;
        --help|-h)
            echo "Usage: $0 [--dry-run] [--erase] [--flash-config]"
            echo ""
            echo "  --dry-run       Preview config without flashing"
            echo "  --erase         Erase entire flash before programming (includes config)"
            echo "  --flash-config  Overwrite the LittleFS config partition (loses pairing/auth)"
            exit 0
            ;;
    esac
done

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }

echo ""
echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
if $DRY_RUN; then
echo -e "${BOLD}║  ESP32 E-Paper Flasher ${YELLOW}(DRY RUN)${NC}${BOLD}   ║${NC}"
elif $ERASE; then
echo -e "${BOLD}║  ESP32 E-Paper Flasher ${RED}(ERASE)${NC}${BOLD}      ║${NC}"
else
echo -e "${BOLD}║     ESP32 E-Paper Device Flasher     ║${NC}"
fi
echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
echo ""

# --- Check prerequisites ---
if [ ! -d "$IDF_PATH" ]; then
    error "ESP-IDF not found at $IDF_PATH"
    error "Install: git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf"
    error "Or set IDF_PATH to your installation."
    exit 1
fi

# Source ESP-IDF environment (makes idf.py, esptool.py, etc. available)
info "Loading ESP-IDF environment..."
. "$IDF_PATH/export.sh" > /dev/null 2>&1
success "ESP-IDF v$(idf.py --version 2>/dev/null | head -1 || echo '?') loaded"

# --- Detect USB port ---
info "Detecting USB serial port..."
PORTS=($(ls /dev/cu.usbserial-* 2>/dev/null || true))

if [ ${#PORTS[@]} -eq 0 ]; then
    if $DRY_RUN; then
        PORT="(no device connected)"
        warn "No USB serial devices found (dry run — continuing anyway)"
    else
        error "No USB serial devices found. Is the ESP32 plugged in?"
        exit 1
    fi
elif [ ${#PORTS[@]} -eq 1 ]; then
    PORT="${PORTS[0]}"
    success "Found port: $PORT"
else
    echo ""
    echo "Multiple USB serial devices found:"
    for i in "${!PORTS[@]}"; do
        echo "  $((i+1))) ${PORTS[$i]}"
    done
    echo ""
    read -p "Select port [1-${#PORTS[@]}]: " PORT_CHOICE
    PORT="${PORTS[$((PORT_CHOICE-1))]}"
    if [ -z "$PORT" ]; then
        error "Invalid selection"
        exit 1
    fi
fi

# --- Read current Kconfig display variant ---
# The display driver is compiled in via Kconfig — check what's currently configured.
SDKCONFIG="$SCRIPT_DIR/sdkconfig"
CURRENT_VARIANT="(not built yet)"
if [ -f "$SDKCONFIG" ]; then
    if grep -q "CONFIG_DISPLAY_DEPG0213BN=y" "$SDKCONFIG"; then
        CURRENT_VARIANT="DEPG0213BN (BW)"
        VARIANT_CONFIG_NAME="lilygo_t5_depg_bw"
    elif grep -q "CONFIG_DISPLAY_GDEW0213I5F=y" "$SDKCONFIG"; then
        CURRENT_VARIANT="GDEW0213I5F (4G grayscale)"
        VARIANT_CONFIG_NAME="lilygo_t5_gdew_4g"
    elif grep -q "CONFIG_DISPLAY_GDEY0213B74=y" "$SDKCONFIG"; then
        CURRENT_VARIANT="GDEY0213B74 (4G grayscale)"
        VARIANT_CONFIG_NAME="lilygo_t5_gdey_4g"
    fi
fi

# --- Prompt for config ---
echo ""
echo -e "${BOLD}Device Configuration${NC}"
echo "Press Enter to accept defaults shown in [brackets]."
echo ""

read -p "  Device name [$DEFAULT_NAME]: " INPUT_NAME
DEVICE_NAME="${INPUT_NAME:-$DEFAULT_NAME}"

echo ""
echo "  Display variant (compiled into firmware via Kconfig):"
echo -e "    Current build: ${CYAN}$CURRENT_VARIANT${NC}"
echo ""
echo "    1) DEPG0213BN    - 2.13\" black & white (SSD1680)"
echo "    2) GDEW0213I5F   - 2.13\" 4-level grayscale (flexible)"
echo "    3) GDEY0213B74   - 2.13\" 4-level grayscale (fast refresh)"
echo ""

VARIANT_OPTIONS=("DEPG0213BN" "GDEW0213I5F" "GDEY0213B74")
VARIANT_CONFIG_NAMES=("lilygo_t5_depg_bw" "lilygo_t5_gdew_4g" "lilygo_t5_gdey_4g")
VARIANT_KCONFIG=("CONFIG_DISPLAY_DEPG0213BN" "CONFIG_DISPLAY_GDEW0213I5F" "CONFIG_DISPLAY_GDEY0213B74")

# Detect current default from sdkconfig
DEFAULT_VARIANT_NUM=2
if [ -f "$SDKCONFIG" ]; then
    for i in "${!VARIANT_KCONFIG[@]}"; do
        if grep -q "${VARIANT_KCONFIG[$i]}=y" "$SDKCONFIG"; then
            DEFAULT_VARIANT_NUM=$((i+1))
            break
        fi
    done
fi

read -p "  Select variant [$DEFAULT_VARIANT_NUM]: " INPUT_VARIANT_NUM
VARIANT_NUM="${INPUT_VARIANT_NUM:-$DEFAULT_VARIANT_NUM}"
VARIANT_IDX=$((VARIANT_NUM-1))
DISPLAY_VARIANT="${VARIANT_OPTIONS[$VARIANT_IDX]}"
DISPLAY_VARIANT_CONFIG="${VARIANT_CONFIG_NAMES[$VARIANT_IDX]}"
DISPLAY_KCONFIG="${VARIANT_KCONFIG[$VARIANT_IDX]}"

if [ -z "$DISPLAY_VARIANT" ]; then
    error "Invalid variant selection"
    exit 1
fi

# Check if variant changed — requires rebuild
NEEDS_REBUILD=false
if [ -f "$SDKCONFIG" ]; then
    if ! grep -q "${DISPLAY_KCONFIG}=y" "$SDKCONFIG"; then
        NEEDS_REBUILD=true
        warn "Display variant changed — firmware will be rebuilt"
    fi
else
    NEEDS_REBUILD=true
fi

read -p "  Display rotation (0-3) [$DEFAULT_ROTATION]: " INPUT_ROTATION
DISPLAY_ROTATION="${INPUT_ROTATION:-$DEFAULT_ROTATION}"

# --- Prepare config.json ---
mkdir -p "$DATA_DIR"
if [ ! -f "$CONFIG_FILE" ]; then
    if [ -f "$CONFIG_EXAMPLE" ]; then
        cp "$CONFIG_EXAMPLE" "$CONFIG_FILE"
        warn "Created config.json from example template"
    else
        error "No config.json or config.json.example found in data/"
        exit 1
    fi
fi

# --- Summary ---
echo ""
echo -e "${BOLD}┌──────────────────────────────────────┐${NC}"
echo -e "${BOLD}│  Flash Summary                       │${NC}"
echo -e "${BOLD}├──────────────────────────────────────┤${NC}"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Port:" "$PORT"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Name:" "$DEVICE_NAME"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Variant:" "$DISPLAY_VARIANT"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Config name:" "$DISPLAY_VARIANT_CONFIG"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Rotation:" "$DISPLAY_ROTATION"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Device ID:" "(auto from MAC)"
if $NEEDS_REBUILD; then
printf "${BOLD}│${NC}  %-14s ${YELLOW}%-22s${NC}${BOLD}│${NC}\n" "Rebuild:" "YES (variant changed)"
fi
if $FLASH_CONFIG; then
printf "${BOLD}│${NC}  %-14s ${YELLOW}%-22s${NC}${BOLD}│${NC}\n" "Config:" "OVERWRITE (loses auth)"
else
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Config:" "preserve on-device"
fi
if $ERASE; then
printf "${BOLD}│${NC}  %-14s ${RED}%-22s${NC}${BOLD}│${NC}\n" "Erase:" "FULL FLASH WIPE"
fi
if $DRY_RUN; then
printf "${BOLD}│${NC}  %-14s ${YELLOW}%-22s${NC}${BOLD}│${NC}\n" "Mode:" "DRY RUN"
fi
echo -e "${BOLD}└──────────────────────────────────────┘${NC}"

# --- Write config.json ---
python3 -c "
import json, sys
with open('$CONFIG_FILE', 'r') as f:
    config = json.load(f)
config['device']['name'] = '$DEVICE_NAME'
config['device']['display_variant'] = '$DISPLAY_VARIANT_CONFIG'
config['device']['id'] = config['device'].get('id', '')
config['display']['rotation'] = $DISPLAY_ROTATION
if '$DRY_RUN' == 'true':
    print(json.dumps(config, indent=2))
else:
    with open('$CONFIG_FILE', 'w') as f:
        json.dump(config, f, indent=2)
        f.write('\n')
"

if $DRY_RUN; then
    echo ""
    info "Config that would be written (shown above)"
    echo ""
    success "Dry run complete. No changes were made."
    info "Run without --dry-run to flash for real."
    exit 0
fi

echo ""
read -p "Proceed with flash? [Y/n]: " CONFIRM
CONFIRM="${CONFIRM:-Y}"
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    info "Aborted."
    exit 0
fi

# --- Update Kconfig if variant changed ---
if $NEEDS_REBUILD; then
    echo ""
    info "Setting display variant to $DISPLAY_VARIANT in sdkconfig..."

    # Clear all variant configs, then set the selected one.
    # idf.py reconfigure will pick up the change on next build.
    for kc in "${VARIANT_KCONFIG[@]}"; do
        sed -i '' "s/^${kc}=y/# ${kc} is not set/" "$SDKCONFIG" 2>/dev/null || true
    done
    # Set the selected variant
    if grep -q "# ${DISPLAY_KCONFIG} is not set" "$SDKCONFIG"; then
        sed -i '' "s/# ${DISPLAY_KCONFIG} is not set/${DISPLAY_KCONFIG}=y/" "$SDKCONFIG"
    else
        echo "${DISPLAY_KCONFIG}=y" >> "$SDKCONFIG"
    fi
fi

# --- Build firmware ---
echo ""
info "Building firmware..."
cd "$SCRIPT_DIR"
idf.py build
success "Firmware built"

# --- Erase flash (if requested) ---
if $ERASE; then
    echo ""
    info "Erasing entire flash..."
    idf.py -p "$PORT" erase-flash
    success "Flash erased"
fi

# --- Create and flash LittleFS image with config.json ---
# Only flash the config partition on first flash, --erase, or --flash-config.
# Normal firmware updates preserve the on-device config (which contains the
# auth_token saved during pairing, WiFi credentials learned via captive portal, etc.).
if $FLASH_CONFIG; then
    echo ""
    info "Creating LittleFS image with config.json..."
    warn "This will overwrite on-device config (auth token, pairing state, etc.)"

    # The partition "littlefs" is at offset 0x3F0000, size 0x10000 (64KB).
    LITTLEFS_IMG="$SCRIPT_DIR/build/littlefs.bin"
    PARTITION_SIZE=$((0x10000))

    # Use the mklittlefs tool from ESP-IDF's littlefs component if available,
    # otherwise fall back to the Python-based littlefs image creator
    if command -v mklittlefs &> /dev/null; then
        mklittlefs -c "$DATA_DIR" -s $PARTITION_SIZE "$LITTLEFS_IMG"
    elif python3 -c "import littlefs" 2>/dev/null; then
        python3 -c "
import littlefs, os, struct

# Create a LittleFS image containing the data/ directory contents
block_size = 4096
block_count = $PARTITION_SIZE // block_size

fs = littlefs.LittleFS(block_size=block_size, block_count=block_count)

data_dir = '$DATA_DIR'
for fname in os.listdir(data_dir):
    fpath = os.path.join(data_dir, fname)
    if os.path.isfile(fpath) and not fname.startswith('.'):
        with open(fpath, 'rb') as src:
            content = src.read()
        with fs.open('/' + fname, 'wb') as dst:
            dst.write(content)
        print(f'  Added: {fname} ({len(content)} bytes)')

# Write the filesystem image
img = bytes(fs.context.buffer)
with open('$LITTLEFS_IMG', 'wb') as f:
    f.write(img)
print(f'  Image size: {len(img)} bytes')
"
    else
        warn "Neither mklittlefs nor Python littlefs module found."
        warn "Installing littlefs-python..."
        pip3 install littlefs-python
        # Re-run the same python block
        python3 -c "
import littlefs, os

block_size = 4096
block_count = $PARTITION_SIZE // block_size

fs = littlefs.LittleFS(block_size=block_size, block_count=block_count)

data_dir = '$DATA_DIR'
for fname in os.listdir(data_dir):
    fpath = os.path.join(data_dir, fname)
    if os.path.isfile(fpath) and not fname.startswith('.'):
        with open(fpath, 'rb') as src:
            content = src.read()
        with fs.open('/' + fname, 'wb') as dst:
            dst.write(content)
        print(f'  Added: {fname} ({len(content)} bytes)')

img = bytes(fs.context.buffer)
with open('$LITTLEFS_IMG', 'wb') as f:
    f.write(img)
print(f'  Image size: {len(img)} bytes')
"
    fi

    success "LittleFS image created"

    info "Flashing LittleFS partition..."
    esptool.py --port "$PORT" --baud 460800 write_flash 0x3F0000 "$LITTLEFS_IMG"
    success "Config partition flashed"
else
    echo ""
    info "Skipping config partition (preserving on-device config)"
    info "Use --flash-config to overwrite, or --erase for full wipe"
fi

# --- Flash firmware ---
echo ""
info "Flashing firmware..."
idf.py -p "$PORT" flash
success "Firmware flashed"

# --- Done ---
echo ""
echo -e "${GREEN}${BOLD}Device flashed successfully!${NC}"
echo ""
echo "  The device will boot and show the PEBL splash screen."
echo "  If no seed_networks are configured, it will start a WiFi"
echo "  captive portal (AP: pebl-setup) for network configuration."
echo ""

# --- Monitor ---
echo ""
info "Starting serial monitor (Ctrl+] to quit)..."
idf.py -p "$PORT" monitor
