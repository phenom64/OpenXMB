# Startup playback and timing

The startup cue plays once at its original duration. Identity fades, warning
dismissal, and menu reveal do not fade or stop the mixer channel. If a longer
cue outlasts the visual intro, the startup component remains transparent and
passes input through until its own channel finishes. Channel ownership is
checked before cleanup so a reused mixer channel is not stopped accidentally.

Enter (including keypad Enter) or controller A/Cross dismisses a visible
photosensitivity warning. Earlier input is consumed without skipping the
identity. Dismissal advances the visual timeline to the normal one-second menu
reveal, releases backdrop blur, and leaves the wave clock unchanged. The
dismissing input is consumed until release to prevent held-button repeats from
activating the menu.

Wave motion starts at 15% speed when the wave emerges at 0.8 seconds, ramps to
180% over three seconds, then settles to normal speed by the warning's arrival
at 7.665 seconds. The smooth velocity curve is integrated into motion phase;
changing a time multiplier directly would jump the ribbon. Steady speed and
explicit fixed-wave capture timestamps retain their original meaning.

MaXMB was inspected at `a746f77906d11390f6a3e12e4f2310a4c45fd399`:
[wavegl.js](https://github.com/MaxiBono/MaXMB/blob/a746f77906d11390f6a3e12e4f2310a4c45fd399/src/js/wavegl.js)
advances wave time by elapsed time at constant speed, while
[intro.js](https://github.com/MaxiBono/MaXMB/blob/a746f77906d11390f6a3e12e4f2310a4c45fd399/src/js/intro.js)
animates brightness. The startup speed envelope here implements the requested
motion; it is not represented as a measured firmware or MaXMB speed curve.

## Verification

The layout/timeline tests cover velocity, phase continuity, warning boundaries,
repeat dismissal, reset, and the one-second reveal. Run the native startup smoke
separately on Windows or Linux:

```text
python tests/xmb/startup_smoke.py --app <built-executable> --assets <build>/shell --output <new-evidence-directory>
```

It measures real SDL mixer playback with a dummy audio sink and checks natural
startup plus Enter dismissal, including an early press and a held repeat. On
Windows it uses native SDL windows because engine stdin polling is Linux-only.
This does not verify physical speaker output or a connected controller.
