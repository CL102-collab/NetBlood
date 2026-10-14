# Native Metal checks

These tests exercise `metal_renderer.mm` through a real SDL Metal window and
Apple's Metal driver. They need macOS 11 or later, Xcode with its Metal compiler,
Python 3, and SDL2 development files. No Blood game assets, Vulkan runtime,
MoltenVK, or OpenGL context are needed.

To build the universal game package on a Mac, follow the dependencies in
[the packaging guide](../../platform/ci/README.md) and run
`bash platform/ci/macos.sh`. Install your Blood game data as usual, then select
**Metal** in the startup window or the graphics menu. The in-game console command
`setrendermode 6` selects the same renderer; the selection persists in the config.

For a visual check, compare the same paused scene in Polymost and Metal, including
voxels, high-resolution models/textures, maphacks, mirrors and room-over-room
geometry. Also check AO on/off, HUD and cinematics, screenshots/save previews,
renderer switching, window resizing and fullscreen on a Retina display.

```sh
SDL_CONFIG=/path/to/native-architecture/prefix/bin/sdl2-config \
  tests/metal/backend-test.sh --require-device --output /tmp/netblood-metal-results
```

`SDL_CONFIG` defaults to `sdl2-config`; `CXX` defaults to `clang++`. `CPPFLAGS`,
`CXXFLAGS`, and `LDFLAGS` are supported. The evidence directory must not exist yet.
The test script first verifies the embedded shader source, invokes Apple's actual
`metal` and `metallib` tools with Metal 2.3/macOS 11 as the baseline, and compiles
and links the production Objective-C++ backend. It rejects direct OpenGL or
Vulkan linkage. These compiler checks run before hardware availability is tested.

Runtime checks cover color and masked clears, lower-left scissors and readbacks,
culling (including vertex-only polygon point mode), normal and reversed depth
ranges, depth comparison/clipping/clamping, source-alpha and reverse-subtract
blending, stencil tests and masked stencil clears, texture orientation, subimage
uploads, mip levels and LOD clamps, and large NPOT textures. Material checks
exercise indexed art, palette swaps, transparent palette index 255, detail/glow
textures, color correction, cinematic YUV, software-surface palettes, and fog.
All 3072 overlapping
transparent draws are checked against sequential CPU compositing across every
pixel. Forty-eight consecutive partial-frame presentations verify complete image
contents without intermediate readbacks or `Finish`. Texture mutation, deletion
and ID reuse, flush/readback ordering, more than 64 MiB of vertex-stream rollover,
resize, suspension, vsync, pending-frame
teardown, runtime-error ownership and recovery are also exercised.

AO scenes require a flat wall to remain unchanged, a concave corner to darken
locally, zero strength and disabled AO to restore the original pixels, stencil
exclusions to work, and the post-pass HUD and pixels outside an inset viewport
to remain unchanged. They check isolated geometry with an infinite far plane,
queued AO frames, and resized targets. Reference/on PPM images are retained.
Apple API and shader validation are enabled during these tests.

`result.json` and separate compile/link/runtime logs report exactly what ran.
Without a native Metal device the script exits **77**, records `status: skipped`
and `runtime: unavailable`, and explicitly says that GPU rendering was not
tested. `--require-device` makes missing hardware a failure. Missing compilers,
shader/backend build errors, SDL window creation errors on a Metal-capable host,
rendering mismatches and runtime validation errors always fail. On Linux the
script reports the macOS requirement and exits 77; it does not substitute mock
frameworks or claim that Apple compilation succeeded.

The universal macOS packaging job always performs shader/backend compilation and
runs these checks. A hosted runner with no exposed GPU may retain an explicit
runtime skip; its artifacts do not constitute a hardware rendering pass. A Mac
with a native Metal device must run the command above with `--require-device`
before making runtime parity or hardware compatibility claims. These focused
backend checks complement, rather than replace, actual game-map comparisons,
voxel/model/maphack/ROR scenes, renderer switching, and legacy demo validation.
