# Trophy notification sound

OpenXMB's trophy sound uses an explicitly selected user-supplied WAV. No original
PS3 recording or replacement chime is included or downloaded. The default is Off,
which also avoids duplicating RPCS3's own notification audio. This setting does
not modify RPCS3, its audio, or its preferences.

## Original PS3 sound research

Research date: 29 October 2026. Identification and permission are separate:

- [Sony's Firmware v2.40 FAQ](https://blog.playstation.com/2008/06/30/firmware-v240-faq/),
  by Eric Lempel, confirms that unlocking a trophy has a sound effect. This is
  primary evidence for the behavior; the article does not supply an audio file.
- [PSDevWiki's firmware sound catalog](https://www.psdevwiki.com/ps3/RCOXML_ofw_sounds)
  lists `snd_trophy` in `system_plugin.rco`, one-channel VAG, with the internal
  name `Trophy_Mono`. It gives MD5 `42C68638FF8F42DAAD2A705F20938DCC` for
  `snd_trophy.ch0.vag` in firmware 4.83. This is secondary technical evidence
  identifying the original firmware resource. The checksum belongs to the
  encoded VAG, not to an arbitrary WAV or a recording of a console. OpenXMB has
  not obtained or independently authenticated that resource's bytes or waveform.
- [RPCS3's notification implementation at commit cd0c200b](https://github.com/RPCS3/rpcs3/blob/cd0c200b9f45427a2a55dc50c99993f2b100a987/rpcs3/Emu/RSX/Overlays/overlay_trophy_notification.cpp#L68)
  calls its trophy sound at the first scheduled notification tick. This supports
  coupling playback to presentation rather than to historical trophy loading.
  It does not authenticate any third-party sound download as original Sony audio.
- [RPCS3 contributor statement](https://github.com/RPCS3/rpcs3/pull/11085#issuecomment-955607843)
  identifies the RCO as part of PS3 firmware and declines to provide the sound
  files because they are Sony property. [Related discussion](https://github.com/RPCS3/rpcs3/issues/12536#issuecomment-1221178644)
  describes RPCS3's user-provided `snd_trophy.wav` option. Later commenters offer
  edited online samples; those posts establish neither original provenance nor
  redistribution permission and were not used as assets.
- [Sony's PS3 System Software License Agreement](https://www.playstation.com/en-us/legal/ps3-system-software-license-agreement/),
  sections 1–2, reserves intellectual-property rights and restricts publication
  of system software. No permission to redistribute the original trophy audio
  with OpenXMB was established by this research.

The original target resource has been identified in technical documentation; no
downloaded recording has been claimed authentic. No firmware or audio recording
was downloaded or extracted for this sound implementation. No original recording,
third-party repost or replacement chime was added to the repository or packaged.
An authenticated file and permission to ship it remain unresolved.

## Playback contract

Sound Settings exposes **Trophy Notification Sound** (Off / User-supplied) and
**Trophy Sound File**. Files remain at the user's chosen location and are read
only. The supported input is PCM-tag-1 WAV: 8/16-bit, mono/stereo, 8–96 kHz, at
most five seconds and 2 MiB. The list shows a bounded filename; its editor keeps
the full path.

Playback uses OpenXMB's existing SDL mixer and Master/Interface Volume. The
audio helper owns only its own chunk/channel. It does not create an audio device,
install a global music callback, change another sound's volume, or infer that
a user-selected sound is an authentic PS3 recording.

The shell connects sound to the genuine selected-session RPCS3 notice queue.
Presentation consumes a notice serial once, including when sound is disabled or
muted. Promotion from a passive banner to interactive XMB cannot replay it.
Profile/session cancellation and audio-device changes retire owned playback.
No synthetic unlock event is added to the application.

Automated generated-tone mixer fixtures are separate from original-sound
authenticity and from a real earned trophy. Actual earned in-game unlock/audio
acceptance remains unverified until tested with a genuine unlock.
