# OpenXMB 0.4.0 Beta 4 development

Development happens on `dev`. The published Beta 3 and `master` are unchanged.
Beta 4 has not been released. Additional visual work from section 8 of the
release brief remains gated on the user's testing and explicit go-ahead.

## Preparation

- Base: `c4bccc8ee6069e1f1f5a3f0a6351e2600bb61eaf`, the fetched `origin/master`.
- Preserved all twelve visual commits through `330de62` with a merge commit.
  The original visual worktree was clean and remains intact.
- Both local `RSX-26` (`b3d1fab`) and remote `RSX-26` (`723d551`) were ancestors
  of `master`; neither had a checked-out worktree. Both branch refs were removed.
- Version, configuration defaults, package manifest and packaging assertions
  use `0.4.0-beta.4`. Packaging filenames derive from `VERSION`; About, System
  Information and command-line version derive from `OPENXMB_VERSION_STRING`.
- The integration pins AuroreEngine `e7abb65`, which includes the Opus visual
  work, minimized lifecycle hook, native-size artwork upload and offscreen frame
  presentation, primary-face-preserving script fallback and bounded incremental
  glyph uploads, retained renderer extent updates and bounded command-buffer
  reuse on its `dev` branch. The engine prefers a shared graphics/present queue
  and uses concurrent swapchain sharing when those queue families differ.
  Separate-family hardware acceptance remains open. Engine `master` and
  the original visual worktree remain unchanged.

## Ownership and implementation sequence

1. **Initial setup.** `openxmb::setup::Model` owns the uncommitted draft, page
   navigation, date-field editing and time-zone rules. Shared native controls
   own rendering; the shell owns first-launch/reset entry, checked profile save
   and the existing boot handoff. No host clock, locale, networking, emulator
   configuration or personal files are changed. Existing saved Beta 3 profiles
   migrate as completed; missing profiles and explicit reset use the same flow.
2. **Game discovery and presentation.** Extend `desktop::Library` and its real
   RPCS3 SFO/native-media import rather than replacing it. A runner supplies
   discovery, metadata and literal executable/argument launch information.
   Registered `games.yml` paths and bootable SFO categories must be validated;
   refresh preserves favourites, history and user-selected artwork.
3. **Sessions and in-game XMB.** A profile-bound session service owns process
   identity, monotonic duration, exactly-once exit observation and screenshot
   attribution. Observation must continue while XMB is minimized: the current
   shell tick is called from rendering, which the engine skips when minimized.
   Platform adapters report overlay, focus and screenshot capabilities. Start
   with a real switch/resume path; enable transparent native overlays only
   after platform runtime checks. No protected-process injection or bypasses.
4. **Providers and YouTube.** Imported RPCS3/RetroAchievements records retain
   provider, user and game identity and cannot be manually unlocked. Existing
   editable local trophy records must not be presented as genuine provider data.
   SteamGridDB asset roles remain distinct, with native PS3 assets taking priority
   and explicit overrides preserved. VacuumTube is presented as **YouTube** in
   TV/Video Services; Information identifies the client. Installation and launch
   use supported upstream distributions and session tracking, with visible progress.
5. **Acceptance and readiness.** Further fidelity refinements require user
   go-ahead. Cross-platform builds, native game/application lifecycle and focus
   tests, real controller input and reference comparisons remain release gates.

## Evidence and references

The first-time setup source is the user's Terra report and 1080p reference
frames from [PS3 setup, 07:52–08:56](https://www.youtube.com/watch?v=0w6ybBQkxVI&t=472s).
Header/footer rules are near y=199/879 on a 1920×1080 canvas. Page transitions
are approximate because the recording contains edits. The daylight-saving
checkbox is an OpenXMB addition; it is not shown in this reference.

The existing `assets/shell/icons/branding/syndromatic.png` is byte-identical to
the user's **Syn new world BETA1.png**. Reuse it with its alpha and aspect ratio,
followed by **Syndromatic Limited Britannia**.

- [RPCS3 source](https://github.com/RPCS3/rpcs3): registered game list, SFO boot
  categories and per-user trophy XML/big-endian unlock data need bounded parsers.
- [RetroAchievements API](https://api-docs.retroachievements.org/getting-started.html)
  and [rc_client integration](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration):
  public progress reads do not authorize earning achievements. Hardcore runtime
  restrictions belong to a legitimate emulator integration.
- [SteamGridDB API](https://www.steamgriddb.com/api/v2) and
  [OpenAPI](https://www.steamgriddb.com/static/openapi.yml): Bearer authentication;
  separate grids, heroes, logos and icons; no HTML scraping.
- [VacuumTube](https://github.com/shy1132/VacuumTube): documented fullscreen,
  portable and deeplink interfaces. A forwarding single-instance launcher PID
  is not necessarily the application's lifetime. Version 2.0.1 was verified
  from the upstream release API during the initial research on 28 October 2026.
- [Windows foreground activation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)
  and Wayland activation policy can deny focus requests. Exclusive fullscreen,
  protected content and anti-cheat restrictions require honest capability results.

## Validation boundaries

On Windows, run `tools/run-beta4-preview.ps1` after building. It uses a separate
profile and media directory under `build/beta4-preview-profile`; the XMB reset
action affects that preview profile. Normal user configuration and libraries
are not used by this launcher. Setup completion persists between preview runs.
Pass `-FirstTimeSetup` to replay setup without deleting the preview profile or
its library. Reset preserves the host's launch-at-login preference.

Native integration smoke:

```powershell
python tests/setup/native_setup_smoke.py --app build/native-c/XMS.bin.exe
```

The initial setup model and configuration tests are registered with CTest as
`openxmb_setup_tests` and `openxmb_config_style_tests`. Capture presets include
setup pages and live French/Hindi/Japanese language changes. Captures use their
own profile and data roots.

Use isolated temporary profiles for migration, reset, capture and runtime tests.
Model tests cover Gregorian dates, segmented editing, GMT minute offsets,
seasonal rules, nonexistent/repeated local times, confirmation and cancellation.
Configuration tests cover Beta 3 migration, restart, unrelated-library preservation
and failed/read-only persistence. Native captures must be compared to original
frames at matching aspect ratios; tests alone cannot establish visual acceptance.

Future integration acceptance needs actual installed games/emulators, original
in-game XMB references, provider credentials supplied through supported settings,
and Windows/Linux/macOS runtime testing. Synthetic metadata fixtures do not prove
actual game boot, genuine unlock decoding, overlay compatibility or focus recovery.

## Session and launcher development increment

Native Game entries launch through seven reusable runner plans. Game Settings
exposes installed runner detection and executable overrides; existing explicit
per-game arguments remain literal. Native game artwork keeps its colour and
aspect ratio. Selection shares the library's PIC1/PAM/AT3 presentation service.
Background discovery produces read-only candidates and imports them on the UI
thread only when the originating profile is still active.

The session observer continues while OpenXMB is minimized. Its profile journal
records direct-child lifetime and actual exit status; a forwarding Steam launcher
does not establish game lifetime or accrue playtime. No process is adopted by PID
for termination. Closing OpenXMB leaves a running game alive and records detachment.

The Windows in-game path has a per-pixel layered compositor fed by AuroreEngine's
offscreen Vulkan frame sink. The controlled 1920x1080 desktop fixture verifies
transparency, navigation, repeated dismissal/reopening, clean screenshots and
game-exit restoration. Windows registers
the configurable Ctrl+Shift+F1 shortcut while a session is observed; SDL Guide
events can also wake the shell when the OS/Steam permits them. Ordinary controller
input is gated while OpenXMB is unfocused. OS foreground denial is reported.
The switch-to-XMB fallback remains available. Exclusive fullscreen, anti-cheat
compatibility and physical Guide/controller behavior are not verified by the
headless fixtures. The overlay does not pause a game or block its independent
controller polling.

The user's original PS3 references establish **Quit Game under Game** and
**Save Screenshot under Photo**. Applications use Quit App. The current Windows
screenshot adapter requires a live, identity-pinned, unique foreground window,
hidden/minimized OpenXMB and stable client bounds. Windows Graphics Capture
captures the exact HWND, excluding unrelated windows and overlays, with native
client cropping. Systems without that backend use conservative desktop sampling
only when capture permission was not denied and no occluder is detected. Capture
runs off the UI thread and writes to the launching profile's Photo folder,
grouped by game/application and title. A bounded settling period retries
transient restoration and geometry states without blocking navigation.
Complete PNGs are published atomically and existing files are never replaced.
Protected, black and unavailable surfaces fail with a message after applicable
retries. Window identity and display affinity are checked again before publication.
Graphics Capture keeps a process-lifetime MTA reference: releasing the final COM
reference on its capture worker can unload a DLL before that worker returns.
Windows builds require the capture SDK by default. Explicit development builds
can opt out with `OPENXMB_REQUIRE_WINDOWS_CAPTURE=OFF`; CMake then warns about
the weaker visible-screen backend. A denied capture permission never falls back.
Cancelled requests check their stop token before capture and atomic publication.
A five-second watchdog clears a stalled request from the UI, and a worker guard
prevents another capture from starting until that worker actually finishes.
Linux X11 now has identity-pinned
window discovery, EWMH focus requests, compositor-backed ARGB presentation and
visible-client PNG capture. It requires a local X11 session, an active compositor,
XRes client identity and libpng16. Wayland/Xwayland and macOS use the explicit
switch-to-XMB fallback. Linux desktop/game acceptance remains open; isolated
Xvfb/compositor fixtures are not a claim of parity on those desktops.

AuroreEngine `dev` includes the event-loop lifecycle hook introduced in `f7396a2` before
frame throttling/minimized skips and defers minimized/zero-sized swapchain
recreation. The actual Windows fixture found and then verified the minimize-during-
render defect. Both 900 ms and 1 ms session exit/return cases pass. The separate
window fixture verifies identity rejection, granted focus, PNG dimensions and
known client pixels. These fixtures are opt-in because they open desktop windows:

```powershell
cmake --build build/native-c --target openxmb_session_window_tests openxmb_session_window_child
& build/native-c/openxmb_session_window_tests.exe (Resolve-Path build/native-c/openxmb_session_window_child.exe).Path
python tests/desktop/native_session_smoke.py --app build/native-c/XMS.bin.exe --child build/native-c/openxmb_session_window_child.exe
python tests/desktop/native_game_smoke.py --app build/native-c/XMS.bin.exe --child build/native-c/openxmb_session_child.exe
python tests/desktop/native_overlay_smoke.py --app build/native-c/XMS.bin.exe --child build/native-c/openxmb_session_window_child.exe --output build/overlay-acceptance
```

## First discovery increment

Existing library scans now include RPCS3's registered `games.yml` paths and
native data roots without first adding those roots manually. This increment is
consumed by the existing **Scan game folders / Scan configured folders** actions;
background startup discovery and direct crossbar game entries also consume these
candidates. Discovery never modifies emulator configuration.
Bootable `DG`/`HG` titles require `USRDIR/EBOOT.BIN`; patches/data do not become
games. Canonical native-folder identity deduplicates path aliases and survives
an emulator path change. Refresh retains history, favourites and custom artwork.

The registry reader supports the upstream scalar-map format with plain, single
or double quoted paths and Unicode escapes. Limits are 4 MiB, 10,000 keys and
32,768 bytes per scalar. Unsupported YAML fails closed for that registry; other
registries and native roots can still be scanned. Candidate installations are
combined, including old Windows registry locations. Custom VFS mounts still
need manual roots; archives and relative registry entries are not imported.

The RPCS3 executable selected in Game Runners now feeds background discovery,
both manual rescan flows, detected game folders and automatic trophy-profile
lookup. Background results are discarded if the originating profile or selected
executable changes before completion. An invalid explicit executable does not
fall back to another installation; Settings displays the error without exiting.
The native Settings fixture covers Unicode portable installations, registered
discs, installed HDD games, game-data exclusion, repeat scans and source-file
preservation with environment/PATH detection removed.

Opt-in real-media acceptance has rendered G-Force's original ICON0/PIC1 assets,
matched unobscured PIC1 pixels and verified background restoration after leaving
the game. Its actual SND0.AT3 decoded through the native preview mixer pipeline
with dummy audio output. These checks preserve source files and do not boot an
emulator. This title has no ICON1.PAM, so it does not establish real PAM acceptance.

## Current integration and testing

Steam discovery reads registered libraries and fully installed app manifests. It
uses locally cached horizontal headers/icons and heroes before network artwork.
JPEG, PNG, WebP and TIFF are decoded by the packaged SDL2_image build; the runtime
codec fixture exercises actual pixels, rather than only image-container headers.
The engine preserves native image dimensions with bounded staging allocations.
A 2305 x 2049 texture fixture validates exact GPU pixels through four upload paths.

Steam game Triangle options include Launch Settings. A selected direct executable,
literal arguments and working folder survive discovery refresh without changing
the Steam identity or duplicating the game. Some titles require Steam even when
started directly; no authentication or DRM bypass is provided. Forwarding launcher
lifetime is not game lifetime. On Windows and native Linux X11, Steam launch observes a newly launched
game inside its registered installation directory using read-only process rights
and conservative association. An explicit executable override takes priority.
Preexisting, ambiguous, inaccessible and expired matches stay unavailable.
Observed playtime is separate from owned-child playtime; Quit Game sends only a
graceful window close request. Linux pins PID/start-time/executable identity and
requires a unique same-user XRes-associated window advertising WM_DELETE_WINDOW;
it never sends termination signals or invents a non-child exit code. Isolated
Xvfb fixtures pass; real Steam/Proton/container desktop acceptance remains open.
The executable selection remains available when
installation-root discovery cannot identify a unique game window. Steam starts
with `-silent -applaunch`; its client and any required authentication remain
necessary. OpenXMB does not alter Steam's controller mappings, inject a proxy DLL,
or disable its renderer.

The local Steam appinfo type excludes tools/framework products from the playable
view. Unknown games remain visible. Filtering preserves the stored records,
favourites, artwork and launch overrides; native imports with similar names stay
independent. An unobserved forwarding launcher cannot indefinitely block a new
game or application session.

Trophy Collection reads real RPCS3 trophy sets by RPCS3 user and offers cache-first
RetroAchievements account reads. SteamGridDB uses official search/role APIs with
explicit artwork selection, caching and native PS3 priority. Live account/API-key
acceptance remains deferred at the user's request. API keys are read from named
launch-environment variables and are not written into the library or settings.
The explicit **Match library games** action hashes raw GB/GBC/GBA ROMs locally
and matches only exact hashes in official `API_GetGameList` hash lists. It retains
manual IDs and account/profile ownership, limits each run to 64 ROMs and caches
lists for one day, including unmatched results. Offline/stale lists are identified.
Other formats require manual IDs; titles are never used to guess a match. ROM
bytes, paths and hashes are not uploaded, and OpenXMB does not award achievements
or implement a Hardcore evaluator.

RPCS3 managed launches prepare a bounded, owner-only configuration snapshot and
disable the native quit confirmation in that snapshot only. The original VFS
mounts, selected RPCS3 user, controller mappings and settings are preserved.
Windows retains a read-only exclusive lease on the original `RPCS3.buf`, including
an explicitly inherited child handle so the lease survives an OpenXMB exit.
Another RPCS3 instance using that root must close first. Linux retains the native
cache/instance-lock directory through `XDG_CACHE_HOME`. Unsupported portable/VFS
layouts, unavailable original instance locks and macOS retain the ordinary native
launch and confirmation. On Windows, a validated private cache junction reuses an
existing original recompiler cache on supported local filesystems. Preparation
does not create, enumerate or modify the original cache. Missing/unsafe cache
layouts retain a cold private cache with a reported reason; RPCS3 can update a
shared cache normally during gameplay. Cleanup must remove the junction without
traversing it. Fixture tests cover original settings/cache preservation and
cross-volume cleanup.
Snapshot size/IO failures also fall back to an ordinary native launch; unsafe or
invalid paths and an in-use original configuration block launch with an error.
Preparation failures and failed child launches roll back only newly owned private
storage. After launch, storage is retained until the exact session is confirmed
exited, then removed after root/parent identity and owner-marker validation.
Cleanup removes a cache junction without traversing its target. Shutdown while
the game still runs preserves its configuration. Crash/forced-exit residues and
failed/refused cleanup remain in place; no prefix-based startup sweep is performed.

A background trophy watcher associates only the active game's exact `TROPDIR`
set IDs with the launched RPCS3 user and HDD. Historical unlocks form a silent
baseline; stable, known locked-to-unlocked changes produce provider-labelled
events. Session/profile/user changes discard stale events. Windows and composited
native X11 can display a passive, input-transparent banner while the game retains
focus; the same renderer draws it inside XMB. Guide promotes it into interactive
XMB on the first press, and screenshots suppress it. Banner-sized offscreen
rendering stops when the queue expires. RPCS3's own trophy popups remain enabled.
The banner identifies RPCS3 and uses genuine title/art/grade data. Its geometry
has not been calibrated against an original PS3 trophy popup. A real earned
in-game unlock and physical input during that notification remain acceptance gates.
An outstanding focus request alone does not start a banner's visible hold time.

YouTube has a packaged menu icon, an installed-client detector and a native setup
progress view. Information identifies the VacuumTube client. Windows managed
installation uses verified release metadata, SHA-256 and bounded archive extraction;
other platforms detect installed clients but do not yet offer automatic setup.
The isolated live Windows acceptance downloaded the official 155 MB v2.0.1
portable release, verified its hash, extracted and detected it, and validated its
fullscreen launch plan. A separate real-client acceptance launches YouTube with
its supported isolated portable profile, opens XMB above it, validates the
darkened app pixels, saves an undimmed screenshot, confirms Quit App, relaunches,
then manually closes the app while XMB is open. It verifies both exits restore
the shell without a stale session or a spurious window-loss dialog. Personal
YouTube login data is not used by this fixture.

The preview launchers now default to fullscreen. Use `-Windowed` explicitly for
windowed testing. `run-beta4-test.ps1` selects an immutable snapshot; it may lag the
source build. Do not replace a snapshot's binaries while a tester is running it.

Windows verification includes automated CTests (one reference-frame
comparison remains explicitly disabled), refreshed setup/reset and provider UI
smokes, and native-size GPU artwork upload. Real provider credentials,
physical controllers and real Linux/macOS desktops remain
release gates. Windows real G-Force, Bomb Rush Cyberfunk through Steam and YouTube
have passed overlay/capture/graceful-exit lifecycle checks with isolated profiles.
These development results do not declare Beta 4 release-ready.

The in-game Vulkan frame smoke verifies a 65-percent black veil, a transparent
clock/date frame interior and independent text/icon opacity. The veil was darkened
at the user's request; the low-resolution reference cannot calibrate its opacity.
A hidden native compositor test exercises
UpdateLayeredWindow without showing a host or changing foreground focus. The
visible controlled fixture also verifies actual keyboard-shortcut activation and
focus return. Windows foreground ownership is read directly because SDL's cached
focus flag can lag after restoring ordinary Vulkan presentation. The Quit Game/
Quit App glyph was reconstructed from the reference's two outlined circular
segments after auditing the available assets. Raw captured layout was compared with the
user's PS3 references; these camera/low-resolution sources do not calibrate exact
opacity or transition timing. The current reversible envelope fades/zooms in over
240 ms and out over 180 ms, starting on the first presented frame. Reduced motion
is respected. Session action rows retain their textures and menu bodies so
opening XMB does not rebuild the full shell. Screenshots wait until the native
overlay has dismissed and the app has regained focus before capture.

Windows additionally has an optional System32 GameInput v0 Guide listener during
active sessions, supplementing SDL and the configurable keyboard shortcut. The
adapter bounds its callback queue, confirms cancellation before releasing callback
state, and releases the runtime on a worker because final Release can take seconds.
Only one runtime owner may be active or tearing down at a time. Deterministic
boundary tests and an opt-in installed-runtime lifecycle test cover these paths;
physical Guide delivery remains unverified. A focused exclusive Guide owner or
operating-system policy can suppress background delivery. Exclusive fullscreen,
protected processes and Linux/macOS desktop overlay parity remain separate acceptance
gates; no injection or protection bypass is used.

The Linux X11 keyboard shortcut uses checked passive root grabs with lock-key
variants and per-client detectable autorepeat. It never requests focus, rolls
back partial grabs on conflict, and stops with the session. Independent Xvfb/XTest
tests cover held keys, 300 rapid taps, focus preservation, conflict rollback and
unchanged global keyboard settings. Wayland/Xwayland fail explicitly rather than
claiming a global shortcut that cannot be delivered.
Interactive mouse input maps buttons 1/2/3 explicitly; wheel and side-button
events are dropped until a corresponding input action is supported.

The setup fidelity pass uses the original 1080p frames for luminous text selection,
fixed-width date cells, 13x7 spinner marks, name-field chrome and confirmation
shading. A trace of the 25 fps recording supplies the page arrows' 2.52-second
cycle. Native captures and human acceptance remain necessary, especially for
font metrics, glow strength and transitions.

Setup now follows the configured theme font: the original theme uses Play for
Latin text, with persistent Noto fallback faces for other scripts. Every language
option keeps its autonym, including British English, regardless of the selected
interface language. Setup preloads a bounded translated subset for all twelve
locales. A language selection changes gettext once and retains fonts and hidden
menus; six live native changes measured at most 0.063 ms in the locale callback.
This measures the synchronous update, not controller-to-display latency.

Date digits use an 80 ms vertical crossfade: increases enter from above and
decreases enter from below, including wraps. Markers and separators stay fixed.
The recorded source shows roughly 40–80 ms transitions, with uncertain exact
travel/easing; automatic/manual mode switching is an OpenXMB extension.

Setup instructions contain logical control tokens whose glyphs come from the
active keyboard/controller bindings. Auto prompts follow the last used input
device. Keyboard prompts use transparent white SVG key artwork at a 4x viewport,
preserving wide-key proportions; keys absent from the pack retain an outlined
label fallback. All 65 imported vectors pass native SVG decode/transparency checks.

The on-screen keyboard reacts to physical key presses without moving the
controller selection. Held Backspace, forward Delete, mapped deletion and
Confirm on the Backspace cell share a time-based repeat: 450 ms initial delay,
the configured initial repeat pace, then gradual acceleration after 1.2 seconds.
Release, loss of focus, disconnect and dismissal cancel the hold. A stalled frame
can delete at most eight codepoints. Editing preserves UTF-8 codepoints; full
grapheme editing, host keyboard layouts and IME composition remain open work.

Suggestions complete the current word from the shipped locale vocabulary,
optional caller context and the current editor buffer. They are local, bounded
and cached, with no network request or saved typing history. This is word
completion rather than a natural-language prediction model. Navigate Right from
the final character column into suggestions, then Up/Down and Confirm. The
default physical Tab binding still saves; an unbound Tab can focus suggestions.
Accepting a suggestion and submitting are separate presses.

Controller L2 toggles accents, R2 toggles case, L1/R1 move the caret, Select
changes input mode, Triangle inserts a space and Start submits. Semantic remaps
take precedence over unbound controller shortcuts. SDL trigger axes become
logical button edges at half pressure, releasing at quarter pressure, so analog
jitter cannot repeatedly toggle a mode. Native keyboard smoke exercises this
axis path and saves its resulting entry through first-time setup.

```powershell
python tests/dialogs/native_keyboard_smoke.py --app build/native-c/XMS.bin.exe --output build/beta4-native-keyboard
```
