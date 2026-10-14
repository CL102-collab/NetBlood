# Vulkan rendering checks

`run.py` exercises actual SDL X11 windows and native Vulkan surfaces. It requires
a Linux build containing both OpenGL and Vulkan, Blood 1.21 assets, Python 3 with
Pillow, a C compiler, SDL2 development headers, `pkg-config`, and an existing
`DISPLAY`. Supply your game files separately; this directory contains no game data.

```sh
python3 tests/vulkan/run.py --assets /path/to/blood121 --mode all --validation
```

The cloud environment has verified lavapipe and Xvfb helpers outside the checkout:

```sh
source /workspace/netblood-env/activate-vulkan.sh
/workspace/netblood-env/with-vulkan-display.sh \
  python3 tests/vulkan/run.py \
  --assets /workspace/netblood-game-data/blood121 --mode all --validation
```

`--mode opengl` checks fixture loading in the reference renderer, `vulkan` checks
Vulkan startup/rendering, `compare` compares OpenGL mode 3
with Vulkan mode 5, `switch` checks 3 → 5 → 3 in one process, and `bridge` checks
3 → 0 → 5 → 0 → 3, including software renderer restoration. `lifecycle` switches
5 → 0 → 5 → 3 → 5 without intermediate screenshots and captures only the final
restored frame. This lets pending GPU work reach renderer teardown without a
preceding readback draining it. `all` runs comparisons and all three switching
sequences, comparing the lifecycle result with the standalone Vulkan image. `--map E1M1 --map E2M1` adds scenes. Each invocation
creates a new evidence directory; use `--output /path/to/new-directory` to select
it explicitly.

The runner builds a small temporary SDL input interposer. It sends normal letter key
events, records successful Vulkan surface creation and OpenGL context attempts,
and executes console scripts through a key binding. After ordinary level startup,
the first script synchronously reloads the requested map with `map`, then pauses
at tick zero before another simulation update. Later keys capture the rendered
scene, optionally switch renderers, and exit through `quit`. Input delays allow
shader compilation and window initialization; they do not select the game tick.
Use `--initial-delay-ms` or `--timeout` when a slow machine needs more startup time.
CI adds `--wait-ready --initial-delay-ms 1000`: the driver waits for the current
executable's level-ready diagnostic and a short settling interval before sending
the first key. This prevents texture loading from consuming that key, while
avoiding a fixed long delay on fast machines. The process timeout still applies.
The option is separate for older executables without the diagnostic.

Checks require normal exit, each expected capture, nontrivial screenshots at the
requested size, retained 32-bit renderer selection, and no logged renderer or
Vulkan validation errors. Vulkan-only runs must create a Vulkan surface with
**zero OpenGL context creation attempts**. Switching must create Vulkan and both
OpenGL contexts. Logs, generated console/config files, screenshots, image
differences, and machine-readable `result.json`/`results.json` files are retained.
The evidence also retains the tested executable and its SHA-256, keeping every
case on the same binary if a concurrent build replaces the original.
`--mode vulkan --disable-opengl` additionally forces OpenGL library loading to
fail. It requires exactly the deliberate missing-library error and permits no
other renderer errors, while retaining the native surface and screenshot checks.

`--vulkan-workers N` requests a positive **total recorder count**, including
the calling thread, through `NBLOOD_VULKAN_WORKERS`. The automatic total is
`max(1, min(5, available CPU cores))`: three available cores use the caller plus
two background recorders; five or more use the caller plus four. Explicit
requests can lower this total, and higher requests clamp to the automatic limit.
The runner checks the effective total against the available-core count reported
at startup. Omitting the option preserves an existing environment override or
automatic selection.

A value of `1` records synchronously on the caller without background recording
threads. A value of `5` allows the caller and four persistent background threads
when enough cores are available. Threads are created as batches need them, and
small batches can use fewer recorders than the configured total.

Add `--require-parallel` to prove that the caller actually records alongside
background work. At least one backend lifetime must report a positive
`caller_parallel_batches` count and `peak_active >= 2`; slot zero must record
draws on the reported caller thread, and another slot must record draws on a
distinct background thread. Overlap involving only background threads cannot
pass. OpenGL reference cases are exempt from this assertion.

The JSON evidence retains configured/started totals and background counts, the
caller identity, batch/draw/secondary-buffer counters, dispatch time, and each
recorder's role, thread ID, draw count, and recording time. These cumulative
counters prove behavior during the backend lifetime, not specifically the
captured frame, and do not establish a performance improvement.

```sh
python3 tests/vulkan/run.py --assets /path/to/blood121 \
  --mode compare --vulkan-workers 5 --require-parallel --validation
```

`--frames-in-flight 1`, `2`, or `3` sets `NBLOOD_VULKAN_FRAMES_IN_FLIGHT`;
omitting it preserves the environment or the default of two submission slots.
The evidence retains each backend lifetime's configuration, submitted/completed
counts, pending high-water mark, unfinished-fence observations, presentation
count, fence/slot-reuse/finish/readback waits, queue/device-idle waits, and retired
image counts. Checks reject mismatched slot counts, inconsistent counters,
queue-idle waits, incomplete teardown, and unreclaimed retired images. Device-idle
waits are reported separately because teardown and resizing can require them.

`--require-inflight` requires at least one lifetime with `max_pending >= 2` and
`max_observed_unsignaled >= 2`: two submitted fences were actually polled
unfinished together. Submitted work that has not yet been checked for completion
alone cannot pass. This proves outstanding GPU submissions, not simultaneous GPU
execution or a speedup. The assertion is optional because a fast driver can finish
before polling observes overlap; OpenGL-only reference cases are exempt.

```sh
python3 tests/vulkan/run.py --assets /path/to/blood121 --mode all \
  --frames-in-flight 2 --require-inflight --validation
```

Paused fixtures cover render state, texture/depth lifetimes, and restoration, but
not continuous game simulation. A separate deterministic demo harness can compare
every simulation step and capture selected frame checkpoints against a reference.
Leave intervals between captures: screenshot readbacks drain work, so capturing
every frame cannot establish multiple frames in flight. Delirium's accumulation
blur also currently requires a framebuffer readback while active.

Comparisons disable texture compression and its disk cache in both renderers
(`r_texcompr 0`, `r_texcache 0`), so OpenGL DXT quantization does not alter source
texels relative to Vulkan. They also disable Vulkan's additional point lighting
(`r_realtimelighting 0`) to compare the shared base rendering. They reject a mean RGB difference greater than 2 on the
0–255 channel scale, or more than 5% of pixels differing by over 16 in any channel.
`--max-mean-error` and `--max-changed-fraction` expose these tolerances. Review
the difference images as well: a passing aggregate threshold does not prove
pixel-perfect parity or cover small missing objects. E1M1 is only an initial
scene. Additional scenes are needed for mirrors, slopes, translucency, fog,
palette changes, voxels, models, and HUD variants. This suite does not replace
simulation/demo determinism tests or hardware/driver coverage.

`make-fixtures.py` creates original, unencrypted Blood maps and tiny MD3/KVX/PNG
assets without copying external art. Generate them into a separate directory:

```sh
python3 tests/vulkan/make-fixtures.py --output /tmp/nblood-vulkan-fixtures
python3 tests/vulkan/run.py --assets /path/to/blood121 \
  --fixtures /tmp/nblood-vulkan-fixtures --mode compare --validation
```

The fixture suite contains an empty room, a skinned MD3 pyramid, an asymmetric
KVX voxel, model maphack rotation/pivot/position offsets, masked and translucent
sprites with different alignments, a slope, a wall mirror, distance shading, and
the zero-visibility fog edge case. Ordinary scenes use positive visibility so
colored materials remain visible in the OpenGL reference.
The generator verifies map CRCs, packed structures, model offsets and winding,
and voxel occupancy. Runtime checks require the maphack load message and visible
image differences from matching baseline scenes in each renderer, so both backends
silently omitting a fixture cannot pass solely by matching each other. Map title
overlays are disabled for these comparisons. Additional GL/Vulkan comparisons
apply the same tolerances specifically to pixels affected by each fixture, so
small objects cannot hide inside a large matching background. Review the retained
images to confirm the intended effect; these small scenes do not cover every combination
or establish full parity. `--map` restricts the suite; effect comparisons run only
when both participating maps are selected. `--defs` and repeatable
`--search-path` allow additional local fixtures.

`make-ror-fixtures.py` creates a separate original Blood room-over-room suite:

```sh
python3 tests/vulkan/make-ror-fixtures.py --output /tmp/nblood-ror-fixtures
python3 tests/vulkan/run.py --assets /path/to/blood121 \
  --fixtures /tmp/nblood-ror-fixtures --mode compare --validation \
  --width 800 --height 600 --output /tmp/nblood-ror-results
```

`VRFLOOR0`/`VRFLOOR` look down through a floor portal, while
`VRCEIL0`/`VRCEIL` look up through a ceiling portal. `VRDEEP0`/`VRDEEP` and
`VRHIGH0`/`VRHIGH` repeat those directions through two portals across three
rooms. Rooms occupy disconnected XY areas and use Blood's upper/lower stack
markers (types 11/12), with no ordinary wall connections. Each pair changes
only the farthest destination room's materials to a bright checker pattern;
geometry, markers, source materials, and the camera remain identical. Visible
changes therefore prove that the renderer drew the room beyond the portal.

The runner requires each destination change to produce mean RGB error of at
least 0.1/255 in each renderer. It compares Polymost and Vulkan over both the
complete frame and the union of changed destination pixels using the usual
parity tolerances. Restrict captures with repeatable `--map` options, retaining
both members of each pair to keep its visibility check. Review screenshots and
difference images alongside `results.json`. These paused fixtures test ordinary
floor/ceiling ROR and vertical portal chains, not movement through portals or
every map configuration. Masked ROR sectors are excluded because `mirrors.cpp`
currently rejects them for the Polymost reference (`MASKED_ROR_ISSUE`).

`generate-shaders.py` regenerates the checked-in material and ambient-occlusion
SPIR-V includes using `glslangValidator`; generated shader consistency is
separate from these runtime checks. The AO shader includes
`source/build/src/ambient_occlusion.glsl`, also used by OpenGL. See the
[AO checks](../ambient-occlusion/README.md) for contact shading, toggle persistence,
HUD isolation and the focused `--ambient-occlusion-only` backend test.

`backend-test.sh` runs focused Vulkan API checks without game assets. It needs a
C++ compiler, SDL2/Vulkan development packages discoverable through `pkg-config`,
and an existing `DISPLAY`:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation tests/vulkan/backend-test.sh
```

It checks depth, stencil, blending, culling, texture uploads and mipmaps,
scissors, descriptor rollover, resizing, and resource lifecycle. Complete RGBA
images must match between one recorder and parallel recording, including overlapping
transparent draws, mutable draw state, texture updates/deletion, clear/readback
boundaries, and stream rollover. It also checks caller/background overlap, lazy
thread creation, the automatic CPU policy, oversized request clamping, and recovery
after injected worker failures through test-only dispatch wrappers. Affinity checks exercise one-core and
three-core configurations when available. These API checks complement the game
screenshots and simulation tests.

The executable links only SDL; Vulkan entry points are resolved at runtime.
The suite checks startup without a Vulkan loader, continued software-window
availability, and clean teardown/recovery when a global, instance, or device
entry point is missing. Test-only recording rendezvous keep the small CPU-policy
overlap checks independent of scheduler timing.

Frame-slot checks compare exact images with a one-slot reference for parallel
recording with one, two, and three slots. They present 48 consecutive partial
frames without readback or Finish, audit real Vulkan calls for fence reuse and
steady-state queue/device-idle waits, and report unfinished-fence observations
separately. Queued texture changes/deletion/ID reuse, stream and descriptor
rollover, Flush/readback/Finish drains, resize/vsync/suspension restoration, and
injected submission/acquisition failures exercise resource ownership and recovery.
`backend-test.sh --teardown-idle-only` isolates the exceptional device-idle error
recovery test, which requires a queue-idle fallback before freeing resources.
Healthy-path game checks continue to require zero queue-idle waits.

`backend-test.sh --realtime-lighting-only` isolates point lighting and geometry
shadow checks without game assets. It renders constant-colored receiving planes
and shadow casters, including a caster entirely outside the camera frustum. It
checks visible illumination, localized shadows, moving lights, camera/world
translation, exact lighting and shadow bypass, empty light/caster lists, alpha
and stencil preservation, post-pass HUD drawing, and inset viewports. Repeated
frames with changing light positions run with one, two, and three submission
slots, followed by resizing and teardown. The tests require no steady-state
queue/device-idle waits and also run in the complete backend suite.

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    tests/vulkan/backend-test.sh --realtime-lighting-only
```

Set `NBLOOD_LIGHTING_TEST_OUTPUT` to an existing directory to retain lossless PPM
images of the unlit, lit, shadowed, and offscreen-caster scenes. These synthetic
checks cover the GPU passes; they do not establish game light selection,
performance, or behavior across hardware vendors.
