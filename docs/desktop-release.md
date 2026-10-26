# RSX-26 desktop workflows

This release develops the native Vulkan desktop shell. OpenGL ES, H700/RG34XX
support and a handheld distribution remain deferred. The bundled fallback artwork
is replaceable in `assets/shell`; the existing fonts, boot identity and sounds are
retained. Original now renders a procedural ribbon and sparkle field without a
private captured-wave asset directory. Classic remains available in settings.

## What is connected to the shell

- Theme Library imports folders, ZIP and P3T packs; previews, component switches,
  custom-theme capture, export, presets, apply, restart restoration and revert are
  available from native controls. [Theme format and limits](../include/openxmb/themes/FORMAT.md).
- Music supports metadata/artwork, playlist queues, seek, shuffle/repeat, volume,
  visualizers and continued playback after returning to the crossbar.
- Photos support folder navigation, zoom/pan, rotate, slideshow, metadata,
  wallpaper, crop/save-copy and confirmed trash. PDF and CBZ/CBT documents use
  page navigation, zoom and print actions. PDF rendering requires Poppler tools.
- Video has timestamped decoding, audio, pause/seek/repeat/speed, audio and text
  subtitle selection, frame stepping, scene search and display modes. Background
  video uses a bounded decoder worker; audio playback and background video do
  not perform decoding on the shell's render thread.
- Local profiles own settings, themes and libraries. Game folders/manifests,
  desktop applications and RPCS3 folders can be scanned, edited and launched;
  selections can preview artwork, video and audio. Favorites and history persist.
  Custom sections add media folders to the horizontal crossbar.
- Browser, software libretro hosting, mounted-disc playback, screenshots and
  login startup use the [desktop integrations](desktop-integrations.md).
- Account, friend and trophy screens explicitly represent local data. They do
  not implement an online console network. Console-specific catalog labels do
  not establish that equivalent operating-system services exist.

`--open-media /path/to/file` opens a registered native viewer. `--open-panel`
accepts `settings`, `themes`, `users`, `games`, `custom`, `friends`, `account`,
`trophies`, `browser`, `emulation`, `discs` or `system`.

## Rendering and resource policy

The procedural wave uploads 7424 bytes of spline/control data per frame, with a
retained index buffer and one indexed ribbon draw. Sparkles use one instanced
quad draw with a bounded count. Font shaping has a bounded cache shared between
measurement and rendering. Theme meshes use retained mapped vertex arenas and
batched draw calls, including depth and additive materials.

Blur textures are allocated per frame slot on first use. The six RGBA16F targets
previously allocated at startup represented about 125 MiB at 1920x1080 with three
slots, even when blur was unused. Single-sample rendering also avoids the unused
MSAA colour targets. Imported archives, texture caches, decoder queues, scripts
and generated geometry have explicit bounds.

These policies reduce work and memory; they do not establish a frame-rate target
on a physical GPU. Software Vulkan measurements include CPU rendering and must
not be presented as handheld or native GPU performance.

## Reproduce validation

Use Clang 19 with clang-scan-deps, CMake/Ninja and the development dependencies in
the README. The default build fetches an immutable engine revision; local engine
work can use `-DDREAMRENDER_LOCAL=/path/to/AuroreEngine`.

```sh
cmake --preset default
cmake --build --preset default
ctest --test-dir build/default --output-on-failure
python3 scripts/desktop-smoke.py --app build/default/XMS.bin --keep-going
python3 scripts/desktop-smoke.py --app build/default/XMS.bin --native-window --keep-going
```

The desktop smoke creates isolated generated images, audio, video, subtitles,
documents and a theme archive. It checks decoded pixels, motion, native controls
and persistence, retaining screenshots/logs and `report.json`. It does not use the
tracked default configuration for writes. Pillow and FFmpeg command-line tools
are required; Tesseract adds visible text checks. The native-window mode also
requires Xvfb, XTest and ImageMagick. Headless media tests explicitly opt into the
engine's `DREAMRENDER_HEADLESS_AUDIO=1` mixer path.

The optional engine `aurore_textured_triangles_smoke` verifies actual textured,
additive and depth-tested pixels, upload counters, and draining a pending upload
from a phase callback. Its generated scene submits 8196 triangles in three mesh
draws. Enable it with `-DDREAMS_BUILD_RENDER_TESTS=ON` and build that target.

## Compatibility boundaries

Dynamic P3T support covers the implemented RAF/model/VSMX scene subset and reports
unsupported features in Theme Library. Universal firmware theme compatibility is
not claimed. Hardware-only libretro cores, bitmap video subtitles, protected disc
playback, native Wayland browser input, Windows/macOS runtime behavior and actual
GPU decoding require their respective implementations or platform/device tests.
A browser must be installed and its sandbox must function in the host environment.
