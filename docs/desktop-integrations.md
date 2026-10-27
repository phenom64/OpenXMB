# Desktop integrations

The shell exposes Browser, Emulation, Optical Media, and Desktop Tools panels.
These panels use native shell controls and controller/keyboard text entry.
Emulation runs in its own native window and returns to the shell when closed.

## Browser

Linux/X11 builds with WebKitGTK 4.1 and GTK3 provide the `openxmb-web` helper.
The shell draws the themed header, page address and hovered-link footer around a
centered native WebKit child surface. Pages render and accept input through
WebKit's sandboxed processes. Install `libwebkit2gtk-4.1-dev` and `libgtk-3-dev`
before configuring to build that optional helper.

The browser opens the saved HTTP/HTTPS address when entered. Ctrl+L opens the
shell address entry; F5 reloads; Alt+Left/Right moves through history; Escape
returns to the crossbar. Page titles and hovered links come from the live view.

Without the helper, the integration prefers a dedicated Chromium/Chrome/Firefox
profile. Closing this session leaves an existing personal browser session alone.
`OPENXMB_BROWSER` selects an explicit executable, either a command on `PATH` or
an absolute path on Linux. A system default browser provides its own navigation
when no supported dedicated browser is installed.

Dedicated sessions support native controller input:

- Left stick moves the system pointer within the browser client area.
- Right stick and Up/Down scroll; A clicks; Back opens the context menu.
- Left/Right navigate backward/forward; X presses Tab; the left shoulder presses
  Shift+Tab; the right shoulder presses Enter.
- Y opens the shell keyboard and types into the focused browser field.
- Start opens address entry and navigates the current window.
- B closes an embedded page and returns to the crossbar. External sessions can
  restore the shell while leaving their dedicated browser window open.

Linux controller injection targets the owned browser window through X11/XTest.
Native Wayland applications may reject injection. macOS requires Accessibility
permission for the native event API. Windows uses normal window focus and
`SendInput` and reports when the foreground-input policy prevents delivery.
The embedded view accepts UTF-8 through the shell keyboard. External Linux
window injection accepts Latin/ASCII; other writing systems use a physical keyboard. Full browser operation requires a
working browser sandbox: OpenXMB never adds flags to disable it.

## Emulation

Install a compatible **software-rendering libretro core** and provide a game file
and any BIOS files that core requires. Cores are discovered in the user data
`OpenXMB/cores` directory, standard system libretro directories, the executable's
`cores` directory, and `OPENXMB_CORE_DIR`. Core and content paths can also be
entered directly. Left/Right cycles discovered cores in the core field.

The companion `openxmb-retro` host provides software frames, stereo PCM audio,
aspect-correct resizing, four gamepads, analog sticks, keyboard input, hotplug,
pause/reset, screenshots, save states and save RAM. Hardware-context-only cores
are rejected with an explicit error rather than silently showing a blank window.

Keyboard controls are arrows plus Z/X/A/S for face buttons, Shift/Enter for
Select/Start, and Q/E for shoulders. Space pauses, F2 resets, F5 saves a state, F8
loads it, F12 saves a screenshot, and Escape exits. On a controller, Guide exits;
hold Back with X to pause, Y to reset, L/R to save/load, A to capture a screenshot,
or Start to exit. Save RAM is persisted on graceful exit, including a requested
session stop on Linux/macOS. States and RAM are namespaced by core/content path.

Runtime files live under the platform user-data directory (`XDG_DATA_HOME` on
Linux, Local App Data on Windows, Application Support on macOS). The `retro/system`
subdirectory is the core's BIOS/system directory. Use user-supplied game/BIOS files;
no cores, ROMs or firmware are bundled.

```sh
openxmb-retro --core /path/to/core.so --content /path/to/game.rom
openxmb-retro --core /path/to/core.so --content /path/to/game.rom \
  --state-directory /path/to/state --load-state /path/to/game.state
```

For reproducible headless verification, the host also supports `--headless`,
`--frames N`, `--save-on-exit`, and `--screenshot frame.bmp`. The test core verifies
real video/audio/input callbacks and serialization; hardware support is a
separate compatibility boundary.

## Optical media and desktop tools

Optical scanning recognizes audio CD devices and mounted DVD `VIDEO_TS` or
Blu-ray `BDMV/STREAM` structures. Linux also reads the existing udev media
classification for unmounted drives. A directory/device may be entered manually.
Playback uses installed mpv or VLC for disc navigation; FFplay can play a
representative unencrypted stream from a mounted disc when neither is installed.
Encrypted media requires a supported decoder and any user-provided keys. Physical
hardware detection and commercial encrypted discs require device testing.

Desktop Tools captures a PNG through ImageMagick on X11, the system capture tool
on macOS, or GDI+ on Windows. The login-startup toggle writes only the current
user's desktop autostart file, LaunchAgent, or Run registry value. Failures are
reported in the panel. In a cloud sandbox with read-only home directories, set
writable `XDG_DATA_HOME` and `XDG_CONFIG_HOME` before starting the shell.
