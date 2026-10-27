# Source provenance review

Source headers use the exact text of the applicable template in
`tools/licensing/headers/`. For XMBShell comparisons, the project owner's rule
selects `header_xmbshell_derived.txt` above 90% identical,
`header_xmbshell_modified.txt` above 51% through 90%, and
`header_openxmb_original.txt` below 50%. The 50% through 51% interval, including
both endpoints, is not specified by that rule and requires clarification. Other
templates cover RetroArch and MPL-licensed material.

Similarity depends on the version, comparison method and matching source file.
A score is not a legal determination or a substitute for checking provenance.
Renamed, split or unmatched paths need history review before a classification is
assigned. Historical notices remain available in version history and the header
templates; changing a header does not establish that its upstream ancestry or
third-party obligations have disappeared.

## Bounded review snapshot

The current files below were compared with XMBShell commit
`38bf4ee5c3b7ad7449e59edf4af96b9ae5a123cb`. The comparison strips each leading
block-comment header and uses Python `difflib.SequenceMatcher` with its default
settings on lines; similarity is `2 × matching lines / total lines in both files`.
This uses one upstream snapshot, exact line matches and the default automatic
junk heuristic. Different revisions or methods can produce different scores.
The shell paths correspond to upstream `src/app/xmbshell.cpp` and `.cppm`; Git
history records the OpenXMB rename in `fd364ca`.

| Current path | Similarity | Rule result | Applied template |
| --- | ---: | --- | --- |
| `src/app/components/main_menu.cpp` | 21.357% | original template | OpenXMB original |
| `src/app/shell.cpp` | 31.720% | original template | OpenXMB original |
| `src/app/shell.cppm` | 63.124% | XMBShell modified template | XMBShell modified |
| `src/programs/image_viewer.cppm` | 11.278% | original template | OpenXMB original |

The selected templates have been applied exactly. Each file above has an XMBShell-derived notice in repository history, including
commit `bd2bcdc`; the percentages do not erase that recorded provenance. The
review of these files is bounded and does not establish the correct header for
other source files. `src/integrations/web_view.cpp` and
`src/xmb/presentation/background.cpp` have no corresponding path in that upstream
snapshot. Their OpenXMB histories begin at `6534db1` and `94a2116`, respectively;
a matching-path comparison alone does not resolve their ancestry.

An earlier matching-path scan found `src/app/module.cppm` (75.0%),
`src/constants.cppm` (71.4%), `src/render/module.cppm` (55.6%) and
`src/render/shaders.cppm` (58.3%) with OpenXMB-original notices. Those results
need history and source review before applying a classification; common
boilerplate, moved code and upstream revisions can affect the scores.

## Header inventory snapshot

A tracked C++ source and header inventory identified 126 files without leading
license headers, 50 project files with exact template headers, two project files
with SPDX-only license headers and one separately classified bundled third-party
file. The two SPDX-only files, `src/programs/document_viewer.cppm` and
`src/render/components/procedural_wave_renderer.cppm`, were introduced in
`fd0bac8`; their GPL-3.0-or-later terms remain in the applied exact OpenXMB
original template. After that change the project counts are 52 exact headers,
zero identified nonmatching license headers and 126 without leading license
headers. Three of the headerless files are empty legacy placeholders. The
remaining files require individual provenance review; missing an upstream path
does not by itself establish original authorship. Bundled third-party material
remains separate from the XMBShell comparison.
