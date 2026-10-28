# RSX-26 desktop workflows

This release develops the native Vulkan desktop shell. OpenGL ES, H700/RG34XX
support and a handheld distribution remain deferred. The bundled fallback artwork
is replaceable in `assets/shell`; the existing fonts, boot identity and sounds are
retained. Original now renders a procedural ribbon and sparkle field without a
private captured-wave asset directory. Classic remains available in settings.

## Desktop presentation and status

The shell includes a native 1920×1080 XMB layout, baked translucent material icons,
a shader-drawn clock frame, glyph-atlas shadow, glow and fade, and an aligned
boot presentation. Native dialogs, the on-screen keyboard, theme hierarchy and
music, photo and video overlays are implemented. Menu selection spacing, the
music wave and playback format badges have also been revised.

These are implementation descriptions. Visual review and end-to-end desktop
validation are ongoing; they do not establish complete fidelity or a performance
target. The current milestone status is tracked in [milestones.txt](../milestones.txt).

## What is connected to the shell

- Settings → Theme Settings imports folders, ZIP and P3T packs; previews, component switches,
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

`--force-dir "E:\Test Media"` uses the specified directory as the root of Photo,
Music and Video for the current run. Each category retains its file filters and
folder navigation. Saved library paths remain unchanged; the option also accepts
`--force-dir="E:\Test Media"`. The directory must already exist.

`--open-media /path/to/file` opens a registered native viewer. `--open-panel`
accepts `settings`, `themes`, `users`, `games`, `custom`, `friends`, `account`,
`trophies`, `browser`, `emulation`, `discs` or `system`.

## Rendering and resource policy

The procedural wave uploads 7424 bytes of spline/control data per frame, with a
retained index buffer and one indexed ribbon draw. Sparkles use one instanced
quad draw with a bounded count. Font shaping has a bounded cache shared between
measurement and rendering. Text shadows and selection glow are baked into padded
glyph coverage channels, so each text draw samples a single atlas. The background
uses compatible UNORM attachment views of its existing sRGB images for encoded
wave blending; this adds no full-screen image or render pass. Theme meshes use retained mapped vertex arenas and
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
work can use `-DAUROREENGINE_LOCAL=/path/to/AuroreEngine`.

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
engine's `AUROREENGINE_HEADLESS_AUDIO=1` mixer path.

The optional engine `aurore_textured_triangles_smoke` verifies actual textured,
additive and depth-tested pixels, upload counters, and draining a pending upload
from a phase callback. Its generated scene submits 8196 triangles in three mesh
draws. Enable it with `-DAUROREENGINE_BUILD_RENDER_TESTS=ON` and build that target.

## Publishing a validated release

The master push workflow builds and validates the Windows portable ZIP, Ubuntu
DEB, portable Linux archive and universal macOS DMG; it does not create a tag or release. Compilation
uses the runner's detected CPU count unless `CMAKE_BUILD_PARALLEL_LEVEL` is
explicitly set. Native regression tests remain serialized.

After an authorized push, select a successful **Build, test and release** push
run whose head is the exact master commit intended for release. Obtain explicit
approval for that full SHA before dispatching **Publish validated release**:

```powershell
gh run list --workflow release.yml --branch master --limit 20
gh run view <RUN_ID>
gh workflow run publish-validated-release.yml --ref master -f run_id=<RUN_ID> -f approved_sha=<FULL_APPROVED_SHA>
```

The publication workflow requires the successful push run, its required platform
jobs, its own checkout and current master to match the approved full SHA. It
verifies the four checksums, reserves the tag for that commit, finds an existing
draft if necessary, uploads the verified assets and publishes only after the
draft contains exactly those assets. If master advances, validate and approve the
new master commit before publication. Dispatching publication is an external write.

The portable Linux archive bundles application libraries and helpers while using
the host's glibc and graphics driver. Its baseline is glibc 2.39, matching the
Ubuntu 24.04 build host. Ubuntu, Fedora and Arch jobs test the same extracted
archive, including Vulkan rendering and the sandboxed browser. These are runner
checks; they do not establish compatibility with every distribution or physical
GPU. See [portable packaging details](linux-portable-packet.md).

## Compatibility boundaries

Dynamic P3T support covers the implemented RAF/model/VSMX scene subset and reports
unsupported features in Theme Settings. Universal firmware theme compatibility is
not claimed. Hardware-only libretro cores, bitmap video subtitles, protected disc
playback and native Wayland browser input require further implementation. Windows
and macOS runtime behavior and hardware video decoding need platform/device tests.
On Linux/X11, optional WebKitGTK 4.1 and GTK3 development packages build the
`openxmb-web` child surface. The Vulkan shell draws the browser header and footer,
and WebKit runs actual pages in sandboxed processes inside the central viewport.
Without that optional helper, the system-browser fallback requires an installed
browser and a working host sandbox.
