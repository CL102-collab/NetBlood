# Vulkan point lighting and shadows

`shadow-mesh-test.sh` checks the CPU shadow mesh builder's concave sectors and
holes, slopes, moving doors, portals, masked surfaces, culling, and cache
invalidation. It needs a C++14 compiler and SDL2 headers, without a GPU, display,
or game data:

```sh
tests/realtime-lighting/shadow-mesh-test.sh
```

The focused backend suite renders constant-colored synthetic geometry with point
lights and full geometry shadows, including a caster outside the camera frustum:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    tests/vulkan/backend-test.sh --realtime-lighting-only
```

See [the Vulkan checks](../vulkan/README.md) for its image assertions, lifecycle
coverage and optional `NBLOOD_LIGHTING_TEST_OUTPUT` evidence directory. These
checks require SDL2/Vulkan development tools and an X11 display, but no game data.

`run.py` exercises the actual game with a generated original room, live torch,
and solid pillar. It captures lighting off, on, then off again at simulation tick
zero with ambient occlusion disabled. Lighting must visibly change the world,
leave opaque HUD panels unchanged, and restore the original pixels exactly when
disabled. A game restart verifies that the disabled setting persists. The runner
requires native Vulkan surface creation without OpenGL context attempts and
rejects rendering/validation errors. It retains the fixture, tested executable
hash, isolated configs, screenshots, difference image, logs and `results.json`.

```sh
source /workspace/netblood-env/activate-vulkan.sh
/workspace/netblood-env/with-vulkan-display.sh \
    python3 tests/realtime-lighting/run.py \
    --assets /workspace/netblood-game-data/blood121 --validation
```

Supply your own Blood 1.21 game assets. `--map` selects an existing scene
instead of generating the fixture; the scene must have active visible lighting
to pass. `--map E1M1 --expect-unlit` instead verifies that the enclosed starting
tomb stays unchanged when the effect is enabled. `--binary` selects the
executable and `--output` a new evidence directory.
The runner also needs Python 3/Pillow, SDL2 development headers, `pkg-config`, and
a C compiler for its temporary input driver. These correctness checks do not
measure performance or replace testing on other GPU drivers.
