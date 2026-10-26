# Native theme packs

Open Theme Library from Theme Settings. Import a `.p3t`, `.zip`, or theme/icon-set folder, inspect its preview and diagnostics, choose components, and apply. Imported originals stay unchanged. An inbox under the profile's theme-data directory supports copying several packs before selecting **Import inbox**. Create Custom Theme captures the current appearance and selected pack assets; Export writes a portable ZIP to a new filename. Presets remember the selected pack and component switches. Restore Original Appearance restores the saved pre-theme settings.

A native ZIP contains `theme.json` at its root (one enclosing folder is also accepted), plus its assets. Paths are relative to the pack; MaXMB `file:` paths are accepted. Example:

```json
{
  "format": "openxmb-theme",
  "version": 1,
  "name": "Blue Night",
  "author": "Your name",
  "background": "media/background.png",
  "preview": "media/preview.png",
  "icons": {"icon_game.png": "icons/icon_game.png"},
  "sounds": {"cursor": "sounds/cursor.ogg"},
  "settings": {"backgroundColor": "#102040", "iconColor": "#ffffff"},
  "animation": {
    "frames": [
      {"file": "media/background.png", "duration_ms": 5000},
      {"file": "media/second.png", "duration_ms": 5000}
    ],
    "layers": [{
      "file": "media/glow.png",
      "duration": 10,
      "keys": [
        {"time": 0, "x": 0, "y": 0, "opacity": 0},
        {"time": 5, "x": 0.1, "y": 0, "opacity": 1},
        {"time": 10, "x": 0, "y": 0, "opacity": 0}
      ]
    }]
  }
}
```

Layer positions and sizes use normalized screen coordinates. Width, height, and opacity default to one. Keyframes interpolate linearly; `duration` loops a layer. `show_start`/`show_end` can restrict visibility within the loop. Background frames loop using their durations; video paths are handed to the native background video player. MaXMB custom theme ZIPs, named background/overlay playlists, icon sets and sound-effect sets are recognized. Icon sets can include Music Player, Video Player, and Photo Viewer subfolders.

PS3 themes use a bounded P3TF parser, including zlib payloads, GIM images, theme sound effects, static backgrounds, and RAF scene conversion. The RAF renderer reports unsupported texture/model/script features and uses an available still background rather than silently inventing animation. Compatibility depends on the assets and scene features used by a theme; its diagnostics remain visible in the library.

Default import limits are 128 MiB compressed input, 512 MiB expanded resources, 64 MiB per resource, 4096 entries, and 4096 pixels per image dimension. Archive links, traversal, duplicate archive names, invalid references, corrupt metadata, and oversized resources are rejected before a pack is published to the library. Limits can be supplied through the native API for a different deployment. Persistent selections use a last-good backup, and failed imports do not replace installed themes.
