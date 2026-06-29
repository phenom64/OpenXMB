# Native Frame Capture (Headless)

Deterministic PNG capture for xmb-web visual parity. Reference goldens live in
`.workflow/rsx-26-xmb-web-parity-rebuild/results/goldens/` (local-only); install
copies into `tests/xmb/reference_frames/` via `install_reference_frames.ps1`.

## Registered scenes (1920×1080)

| Scene | Harness env | Description |
|-------|-------------|-------------|
| `root` | `OPENXMB_INITIAL_CATEGORY=users` | Users root after boot (headless skips overlay, settles at 16.9s) |
| `settings` | `OPENXMB_INITIAL_SETTINGS_MENU=category.settings`, `OPENXMB_INITIAL_SETTINGS_SELECTION=5` | Settings → System Settings focused |
| `theme` | `OPENXMB_INITIAL_SETTINGS_MENU=category.settings.theme.settings` | Theme Settings submenu |
| `theme-panel` | above + `OPENXMB_INITIAL_SETTINGS_SELECTION=0`, `OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE=1` | Theme dialog panel open |

Presets: `tools/xmb/scene_presets.json`

## Quick workflow

```powershell
cd $env:USERPROFILE/Developer/OpenXMB

# 1. Build with compat pack
cmake --preset windows-native
cmake --build --preset windows-native

# 2. Install xmb-web reference PNGs (local, gitignored)
.\tools\xmb\install_reference_frames.ps1

# 3. Verify reference SHA-256 pins
python tools/xmb/verify_reference_frames.py

# 4. Capture one scene or all scenes
.\tools\xmb\capture_frame.ps1 -Scene root
.\tools\xmb\capture_frame.ps1 -Scene all

# 5. Full regression (capture + compare + heatmaps)
.\tools\xmb\run_visual_regression.ps1 -Heatmap
```

Output captures: `build/visual-regression/captures/<scene>/<scene>.png`  
Reports: `build/visual-regression/reports/<scene>-comparison.json`

## Background-only capture (wave tuning)

```powershell
.\tools\xmb\capture_background_audit.ps1
```

## Environment variables

| Variable | Purpose |
|----------|---------|
| `DREAMRENDER_HEADLESS=1` | Offscreen render (AuroreEngine) |
| `DREAMRENDER_HEADLESS_WIDTH/HEIGHT` | Output resolution (default 1920×1080) |
| `DREAMRENDER_HEADLESS_FRAMES` | Frames to write (canonical uses last frame) |
| `DREAMRENDER_HEADLESS_OUTPUT_DIR` | Output directory |
| `DREAMRENDER_HEADLESS_OUTPUT_PATTERN` | Filename pattern per scene preset |
| `OPENXMB_FIXED_UNIX_SECONDS` | Freeze month gradient / clock (default: June 15 2026 noon UTC) |
| `OPENXMB_FIXED_WAVE_SECONDS` | Freeze wave animation phase |
| `OPENXMB_HEADLESS_STARTUP=1` | Force boot overlay in headless (boot milestone captures) |
| `OPENXMB_FIXED_BOOT_SECONDS=17.0` | Freeze boot timeline when startup overlay enabled |
| `OPENXMB_INITIAL_CATEGORY` | Root category (`users`, `settings`, …) |
| `OPENXMB_INITIAL_SETTINGS_MENU` | Catalog settings menu id |
| `OPENXMB_INITIAL_SETTINGS_SELECTION` | Row index within current settings menu |
| `OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE` | Open choice/dialog panel on harness init |

## Compare single frame

```powershell
python tools/xmb/compare_frames.py `
  --reference tests/xmb/reference_frames/root.png `
  --actual build/visual-regression/captures/root/root.png `
  --thresholds assets/xmb/manifests/visual-thresholds.json `
  --scene root `
  --heatmap build/visual-regression/reports/root-heatmap.png
```

Thresholds: MAE ≤ 0.06, RMSE ≤ 0.12, P95 ≤ 0.20, fraction_pixels_over_32 ≤ 0.18.

## CTest integration

```powershell
cmake --preset windows-native -DOPENXMB_VISUAL_REGRESSION=ON
cmake --build --preset windows-native
.\tools\xmb\install_reference_frames.ps1
ctest --test-dir build/native -R "openxmb_visual" --output-on-failure
```

`openxmb_verify_reference_frames` is enabled when reference PNGs exist.  
`openxmb_visual_regression` compares pre-captured frames (`-SkipCapture` mode).