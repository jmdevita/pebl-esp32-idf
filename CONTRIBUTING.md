# Contributing

Thanks for taking a look. This repo is a git subtree mirror of a private monorepo, so substantive PRs may be merged upstream and pushed back here with attribution rather than landing as direct merges. Either way, please open a PR or issue first so we can avoid duplicate work.

## Setup

1. Install ESP-IDF v5.5+ — see [Espressif's getting-started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
2. Activate the toolchain: `source ~/esp/esp-idf/export.sh`
3. Pick a target: `idf.py set-target esp32` (LilyGo T5) or `idf.py set-target esp32s3` (Custom PCB).
4. `idf.py menuconfig` → Display Manager → select the panel you have.
5. `idf.py build`.

## Testing changes

```bash
./flash.sh --dry-run     # Preview config without writing anything
./flash.sh               # Interactive build + flash + serial monitor
./flash.sh --erase       # Erase NVS first (factory reset)
```

For UI changes, flash a real device and verify on the e-paper — the simulator doesn't catch refresh artifacts or partial-update bugs.

## Code style

- Match the surrounding code. C++ in IDF style, FreeRTOS task patterns as in `main/app_main.cpp`.
- Comments explain *why*, not *what*. Skip narration of obvious code; flag subtle invariants, timing dependencies, or hardware quirks.
- New components go under `components/<name>/` with their own `CMakeLists.txt` and a public header in `components/<name>/include/`.
- Keep board-specific logic behind the HAL in `components/board/` — don't sprinkle `#if CONFIG_IDF_TARGET_*` across the codebase.

## PR checklist

- [ ] Builds clean for both `esp32` and `esp32s3` targets where applicable
- [ ] Tested on at least one real device
- [ ] No new files committed under `build/`, `data/config.json`, `.claude/`, or other gitignored paths
- [ ] Comments and `README.md` updated if behavior or build steps changed
