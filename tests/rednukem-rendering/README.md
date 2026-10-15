# Rednukem rendering checks

`run.py` exercises the Rednukem executable with an original generated Build v7
room, solid pillar and stationary burning barrel. The fixture disables the barrel's
ART animation so paused comparisons remain stable. Supply Duke Nukem 3D assets
for the palette, textures and HUD; the original 1.3D shareware distribution also
works for this fixture. The runner supplies its own small CON script, so this
does not establish support for running the shareware game's original scripts.
No game data is downloaded, modified or checked into the repository.

```sh
python3 tests/rednukem-rendering/run.py \
    --binary ./rednukem --assets /path/to/duke3d --mode all --validation
```

The runner needs Python 3/Pillow, a C compiler and SDL2 development headers.
Linux needs an existing X11 `DISPLAY`, OpenGL and Vulkan drivers; `--validation`
also needs the Khronos validation layer. In the configured cloud workspace:

```sh
/workspace/netblood-env/with-vulkan-display.sh \
    python3 tests/rednukem-rendering/run.py \
    --assets /workspace/rednukem-game-data/duke13-shareware --validation
```

Each renderer loads the fixture and receives the ordinary Pause key. The runner
then sends console commands to capture AO off/on/off and lighting off/on/off.
Both effects must visibly change the room, leave the opaque status bar unchanged,
and restore the original pixels exactly when disabled. A fresh process verifies
that both disabled preferences were saved and restored. The default `all` mode
also compares OpenGL/native captures and switches OpenGL → native → OpenGL with
both effects enabled. Every switching capture must match its standalone
combined-effects reference; returning to OpenGL must restore its image exactly.
A final switch to the native renderer followed by a restart verifies that the
renderer preference is saved and restored.
Cross-renderer comparisons allow mean RGB error ≤ 2 and at most 5% of pixels
differing by more than 16 in any channel. Comparisons with standalone images from
the same renderer use mean error ≤ 0.5, allowing a small difference in the
independently paused light flicker while rejecting a missing lighting pass.

Logs check native surface creation without OpenGL context attempts, successful
commands, and graphics/validation errors. The evidence directory retains the
original fixture, executable and asset hashes, isolated configuration files,
screenshots, difference images and `results.json`. Use `--output` to select a new
directory, `--mode opengl` or `--mode vulkan` to select one renderer, and
`--initial-delay-ms` to allow longer initialization on slower machines.

On macOS with SDL 2.0.14+, use an unbundled development executable and native Metal:

```sh
SDL_CONFIG=/path/to/sdl2-config python3 tests/rednukem-rendering/run.py \
    --binary /path/to/rednukem --assets /path/to/duke3d \
    --native-backend metal --validation
```

Metal selection on Linux exits 77 and records an explicit skip. It is not a
Metal compilation or GPU test. The runner does not validate Redneck Rampage,
Rides Again or Duke Nukem 64 gameplay, and does not measure performance.

The shared [Vulkan](../vulkan/README.md), [Metal](../metal/README.md),
[AO](../ambient-occlusion/README.md) and
[lighting/shadow](../realtime-lighting/README.md) backend suites additionally
cover original synthetic geometry, shadow casting outside the camera view,
resource lifetime and renderer state restoration without game assets.
