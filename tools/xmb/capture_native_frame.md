# Native Frame Capture (Headless)

Use this workflow to produce deterministic PNGs for visual comparison against xmb-web.

## Prerequisites

- `windows-native` build with compat pack imported
- PowerShell 7+ (`pwsh`)
- Python 3 + Pillow (for `compare_frames.py`)

## Quick capture (background-only)

```powershell
cd $env:USERPROFILE/Developer/OpenXMB
cmake --preset windows-native
cmake --build --preset windows-native
.\tools\xmb\capture_background_audit.ps1
```

Output: `build/audit-captures/native-original-explicit/original-00000.png` (etc.)

## Environment variables

| Variable | Purpose |
|----------|---------|
| `DREAMRENDER_HEADLESS=1` | Offscreen render (AuroreEngine) |
| `DREAMRENDER_HEADLESS_WIDTH/HEIGHT` | Output resolution |
| `DREAMRENDER_HEADLESS_FRAMES` | Number of PNGs to write |
| `DREAMRENDER_HEADLESS_OUTPUT_DIR` | Output directory |
| `DREAMRENDER_HEADLESS_OUTPUT_PATTERN` | Filename pattern (`original-{:05d}.png`) |
| `OPENXMB_FIXED_UNIX_SECONDS` | Freeze month gradient / clock |
| `OPENXMB_FIXED_WAVE_SECONDS` | Freeze wave animation phase |
| `OPENXMB_HEADLESS_STARTUP=1` | Skip boot overlay (full UI captures) |
| `OPENXMB_FIXED_BOOT_SECONDS=17.0` | Freeze boot timeline for UI scenes |

## Compare against reference

```powershell
python tools/xmb/compare_frames.py `
  --reference <path-to-reference.png> `
  --actual build/audit-captures/native-original-explicit/original-00003.png `
  --thresholds assets/xmb/manifests/visual-thresholds.json `
  --scene root `
  --heatmap build/audit-captures/root-heatmap.png
```

Registered scenes: `root`, `settings`, `theme`, `theme-panel` (see `visual-thresholds.json`).

## Full UI capture (future Phase 1)

For crossbar/settings scenes, omit `--background-only` and set `OPENXMB_HEADLESS_STARTUP=1` with appropriate `OPENXMB_FIXED_BOOT_SECONDS`.