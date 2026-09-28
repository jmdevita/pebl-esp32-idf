# ESP32 IDF Client

ESP-IDF firmware for e-paper Slack and Discord reaction displays. Same hardware and user-facing behavior as the [Arduino client](https://github.com/jmdevita/pebl-esp32), with auto light sleep over WiFi DTIM (~1–3 mA average) instead of deep-sleep polling. Part of [Pebl](https://pebl.ink).

## Supported Hardware

| Board                 | Chip       | Display variant Kconfig | Panel        |
|-----------------------|------------|-------------------------|--------------|
| LilyGo T5 V2.3        | ESP32      | `DISPLAY_DEPG0213BN`    | DEPG0213BN   |
| LilyGo T5 V2.3 (4G)   | ESP32      | `DISPLAY_GDEW0213I5F`   | GDEW0213I5F  |
| Custom PCB v1.1       | ESP32-S3   | `DISPLAY_GDEY0213B74`   | GDEY0213B74  |

Match the Kconfig display variant to the panel actually fitted to your board.

## Quick Start

### 1. Install ESP-IDF (v5.5+)

Follow [Espressif's getting-started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/). Confirmed working on IDF v5.5.3.

### 2. Clone

```bash
git clone https://github.com/jmdevita/pebl-esp32-idf.git
cd pebl-esp32-idf
```

### 3. Pick board target and display variant

```bash
source ~/esp/esp-idf/export.sh

# LilyGo T5 (ESP32):
idf.py set-target esp32

# Custom PCB (ESP32-S3):
idf.py set-target esp32s3

idf.py menuconfig   # Display Manager → display variant, board pins, etc.
```

### 4. Build and flash

```bash
./flash.sh             # Interactive — prompts for device name, variant, rotation
./flash.sh --dry-run   # Preview config without flashing
./flash.sh --erase     # Erase NVS first (factory reset)
```

`flash.sh` writes `data/config.json` (gitignored), builds, flashes firmware + LittleFS, then opens a serial monitor.

### 5. First-boot setup (on the device)

1. Power on. The display shows a Wi-Fi setup QR.
2. Join the device's Wi-Fi `pebl-setup` from your phone, pick your home network in the captive portal.
3. The display shows a pairing QR + 8-character code. Visit [pebl.ink/connect](https://pebl.ink/connect), sign in with Slack or Discord, and enter the code.
4. Reactions appear in real time.

## Boot Button

The button (default GPIO 39 on LilyGo T5, configurable per board) supports three actions based on hold duration:

| Hold              | Action                                                |
|-------------------|-------------------------------------------------------|
| Short press       | Wake from light sleep                                 |
| 3–14 seconds      | Add platform — re-enter pairing to link another account |
| 15+ seconds       | Wi-Fi setup — relaunch captive portal to switch networks |

At the 3-second mark the e-paper shows feedback so you know what release vs. continued hold will do.

## Configuration (`data/config.json`)

`flash.sh` builds this from `data/config.json.example`. Key fields:

| Section              | Field                          | Notes                                                   |
|----------------------|--------------------------------|---------------------------------------------------------|
| `device`             | `name`, `display_variant`      | Display variant must match the Kconfig build            |
| `wifi`               | `seed_networks[]`              | Optional — pre-seeded SSIDs tried before captive portal |
| `server`             | `host`, `path`                 | Defaults to `pebl.ink` and `/ws-stream`                 |
| `power`              | `sleep_enabled`, `sleep_duration_min` | Light-sleep tuning                               |
| `quiet_hours`        | `start_hour`, `end_hour`       | Reduces refresh cadence overnight                       |
| `display_policy`     | `skip_refresh_on_no_message`   | Skips e-paper refresh when nothing changed              |
| `ota`                | `enabled`                      | OTA on boot + every 24h                                 |

`auth_token` and `device.id` are populated at runtime — never flash them by hand.

## Project Structure

```
esp32_idf_client/
├── main/app_main.cpp                  # FreeRTOS task wiring
├── components/
│   ├── board/                         # Per-chip HAL (ESP32 / ESP32-S3)
│   ├── config_manager/                # LittleFS JSON config
│   ├── wifi_manager/                  # Connect + captive portal
│   ├── pairing_manager/               # /api/pairing/* device flow
│   ├── websocket_manager/             # wss://pebl.ink/ws-stream
│   ├── display_manager/               # CalEPD + GFX rendering
│   ├── security_manager/              # ECDH P-256 keypair, /upload
│   ├── ota_manager/                   # Boot + 24h OTA check
│   ├── power_manager/                 # Light sleep, battery
│   ├── resilience_manager/            # Connection health monitor
│   ├── timezone_manager/              # GeoIP-based TZ sync
│   ├── CalEPD/                        # E-paper drivers (vendored)
│   └── ...
├── data/config.json.example           # Configuration template
├── flash.sh                           # Interactive build + flash
└── partitions.csv                     # OTA-compatible partition table
```

Partition table is identical to the Arduino client, so an Arduino device can OTA onto this
firmware. The reverse is refused: this firmware only installs images with its own project name.

### OTA safety

- **Verified before switching:** image header (project name, version), SHA-256 and ECDSA signature
  are checked before the new slot becomes bootable. Downgrades are refused.
- **Probation:** the first boot of an update is not marked valid until it reaches the server (boot
  firmware check or WebSocket registration). A crash, watchdog reset or power loss before that boots
  the previous firmware; so does a 10-minute self-test timeout.
- **Crash-loop guard:** for an hour after confirmation, 3 panic/watchdog resets switch back to the
  previous slot.
- **Failure memory:** an image that fails twice (crashed or timed out on first boot, or bad
  header/hash/signature) is not retried. Memory is keyed on version + sha256, so publishing a
  corrected build (same or higher version) installs normally. Power-related rollbacks (brownout,
  power loss) are reported but not counted.
- **Reporting:** every outcome is POSTed to `/api/firmware/stats` and lands in `update_logs` as
  `update_success`, `update_rollback` or `update_failed` (with the reason).

## Troubleshooting

| Problem                    | Quick check                                                    |
|----------------------------|----------------------------------------------------------------|
| Wrong display panel        | Kconfig display variant matches the actual panel               |
| Won't connect to Wi-Fi     | 2.4 GHz network; password correct in captive portal            |
| Pairing QR never appears   | `data/config.json` `server.host` reachable; check serial logs  |
| Build fails, missing venv  | `source ~/esp/esp-idf/export.sh` before `idf.py`               |
| OTA stalls                 | `display_variant` in config matches a published firmware lane; `update_logs` for `update_failed` / `update_rollback` |

Serial logs are the fastest path — `idf.py monitor` after a flash.

## License

MIT — see [LICENSE](LICENSE).
