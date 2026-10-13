# Save thumbnail and statistics checks

Build the game, then run under X11/Xvfb with Blood 1.21 assets:

```sh
python3 tests/save-preview/run.py \
  --binary ./nblood --assets /path/to/blood-1.21 \
  --output /tmp/netblood-save-preview-evidence
```

The output directory must not exist. The fixture uses the network tests' SDL
input driver and X11 screenshot helpers, so SDL2 headers, `pkg-config`, Pillow,
ImageMagick, `xwininfo` and `xprop` are required. It does not open a network
session. Software, Polymost and Vulkan are checked by default;
`--renderers 0 3` can select only the built renderers. Polymer is not tested.

A generated room and ART file contain only static grayscale patterns. The
fixture saves an unchanged camera first without overlays and then with a
reduced viewport, HUD, message, crosshair and first-person weapon. Both embedded
320×200 previews must be byte-identical and contain only world pixels. Ordinary
camera movement followed by overwriting and quicksaving must refresh the
image. Screenshot comparisons verify the selected slot's actual menu thumbnail,
empty-slot fallback, and cache refresh. Slots zero and five deliberately leave
holes in the save list.

The process exits and relaunches from the same working directory with its saved
configuration. Both thumbnails must persist, and selecting Load must restore
the saved player position. A damaged optional preview and a save without a
preview must show a fallback while the game state remains loadable. Supply
`--legacy-save /path/to/game0000.sav` to exercise an untouched save made with
the prior executable; otherwise the fixture removes only the new trailer from
one of its saves. With a prior save supplied, a noncanonical backup filename
also verifies that startup cannot replace a real slot's title or thumbnail
with a backup's embedded slot number.

The optional `NBSTAT01` statistics block is independently decoded and checked
against actual simulation time and UTC wall-clock bounds during each save.
Overwrites and quicksaves must refresh it; separate slots and process restarts
must preserve their own values. Games run in the non-UTC `EST5` timezone by
default (`--timezone` accepts another POSIX timezone). Native font matching of the actual menu
screenshots verifies elapsed time, counters and the saved timestamp converted
to local time; the stored timestamp must remain UTC Unix seconds. Missing or
corrupt statistics must display `UNKNOWN`, including a preview-only save from
the previous format. Corrupt or absent thumbnails must retain valid statistics.
Brief screenshot retries avoid a transient animated menu blood droplet hiding
a digit; every required digit must still match exactly. The final restart changes the viewer's timezone to `JST-9`: the unchanged save
must display the newly converted local date without rewriting its UTC value.

A real Blood 1.21 E1M1 save verifies nonzero totals (18 enemies and 11 normal
secrets at the fixture's default difficulty). A clearly marked synthetic copy
changes only the uniquely identified legacy EndGame counter block to seven
kills and three secrets found. Normal menu loading and re-saving must capture
those actual world counters, display them for the selected slot, and preserve
them after another process restart. `--stats-only --renderers 5` runs just this
focused counter/timezone check when the thumbnail phases were already verified. The optional statistics block is not
modified to manufacture that result.

Evidence includes the executable hash, logs, complete saves, extracted preview
PNGs, full menu/game screenshots, per-digit matching scores and `result.json`. The supplied game's palette
is read locally only to convert indexed preview bytes into viewable PNG files.
