# Ambient occlusion checks

These checks exercise the shared screen-space ambient occlusion algorithm in
Polymost and native Vulkan. They do not use Polymer.

## Synthetic native Vulkan checks

The existing backend runner has a focused AO entry point. It needs SDL2 and
Vulkan development packages, a C++11 compiler, and an active SDL display, but no
game assets:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    tests/vulkan/backend-test.sh --ambient-occlusion-only
```

The test renders original, constant-colored geometry using Polymost's positive-Z
perspective and the real native backend. It verifies:

- A flat wall does not shadow itself; a concave 90-degree corner becomes darker
  near the joint than on distant wall regions.
- AO does not brighten the scene or change its alpha channel.
- Zero strength and disabling AO restore the original pixels exactly, including
  after previously running the effect.
- Geometry excluded by the world stencil mask receives no AO.
- A one-pixel silhouette with an infinite far plane stays unchanged despite
  having no valid depth neighbors from which to reconstruct a normal.
- HUD bars drawn after the pass remain identical, and an inset world viewport
  leaves the surrounding framebuffer untouched.
- Repeated frames remain identical with one, two, and three submission slots,
  without introducing steady-state queue-idle or device-idle waits.
- Resizing recreates usable AO targets; normal teardown remains valid.

The same checks also run as part of the complete `backend-test.sh` suite.
The runner fails on Vulkan validation diagnostics as well as image assertions.
For lossless flat-wall and corner evidence, create a directory and set
`NBLOOD_AO_TEST_OUTPUT` to its absolute path; the test writes four PPM images.
The images are deliberately untextured so contact shading is easy to inspect.

```sh
mkdir /tmp/netblood-ao-evidence
NBLOOD_AO_TEST_OUTPUT=/tmp/netblood-ao-evidence \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    tests/vulkan/backend-test.sh --ambient-occlusion-only
```

## Game integration checks

The game runner requires a Linux executable containing both renderers, Python 3
with Pillow, SDL2 development files, an existing X11 `DISPLAY`, and separately
supplied Blood 1.21 assets:

```sh
python3 tests/ambient-occlusion/run.py --assets /path/to/blood121 --validation
```

It loads and pauses the same map in each renderer, then captures AO off, on, and
off again. The scene must visibly change when AO is enabled and return exactly
to its original image when disabled. Both opaque status panels must stay unchanged.
It restarts the game to verify the saved off setting is restored, and compares
Polymost/Vulkan screenshots for all three states using the existing rendering
suite's tolerances. Vulkan's separate point lighting is disabled during these AO
comparisons. Screenshots, difference images, isolated configurations,
logs, the tested executable's hash, and `results.json` are retained.

`--map E1M1` selects the scene, `--binary` selects the executable, and `--output`
selects a new evidence directory. The cloud environment can supply its verified
lavapipe/Xvfb setup:

```sh
source /workspace/netblood-env/activate-vulkan.sh
/workspace/netblood-env/with-vulkan-display.sh \
    python3 tests/ambient-occlusion/run.py \
    --assets /workspace/netblood-game-data/blood121 --validation
```

CI adds `--wait-ready`, which waits for the current executable's level-ready
diagnostic before sending input, including after restarting. This avoids losing
keypresses while textures are still loading on a busy machine. The ordinary
process timeout still applies. The option is separate so older executables
without that diagnostic can still use the default timed input sequence.

These are correctness checks, not performance measurements or broad GPU-driver
coverage. Game screenshots complement the synthetic geometry checks by covering
the actual pass placement, user setting, HUD, and renderer integration.
