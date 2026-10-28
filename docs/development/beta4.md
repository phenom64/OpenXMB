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
- The visual integration uses AuroreEngine visual commit `19d2fc1`, preserved
  on the engine's `dev` branch and pinned in FetchContent. Engine `master` and
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
