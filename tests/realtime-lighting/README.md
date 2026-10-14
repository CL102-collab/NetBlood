# OpenGL, Vulkan and Metal point lighting and shadows

`shadow-mesh-test.sh` checks the CPU shadow mesh builder's concave sectors and
holes, slopes, moving doors, portals, masked surfaces, culling, and cache
invalidation. It needs a C++14 compiler and SDL2 headers, without a GPU, display,
or game data:

```sh
tests/realtime-lighting/shadow-mesh-test.sh
```

The focused backend suites render constant-colored synthetic geometry with point
lights and full geometry shadows, including a caster outside the camera frustum:

```sh
tests/realtime-lighting/opengl-backend-test.sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    tests/vulkan/backend-test.sh --realtime-lighting-only
# On a Mac with Xcode's Metal toolchain and native Metal hardware:
tests/metal/backend-test.sh --realtime-lighting-only --require-device
```

See [the Vulkan checks](../vulkan/README.md) for its image assertions, lifecycle
coverage and optional `NBLOOD_LIGHTING_TEST_OUTPUT` evidence directory. The
OpenGL suite also verifies restoration of the caller's program, framebuffer,
texture/sampler, buffer/VAO, matrix, blend, depth, stencil and viewport state. It
checks exact bypass, moving lights, HUD isolation, resizing and resource reset,
the compatibility path without vertex array objects, and recovery after an
unsupported depth target.
Its script needs C++14, Python 3, SDL2 headers and a compatibility OpenGL driver;
the Vulkan script needs SDL2/Vulkan development packages. Both use an existing
X11 display and require no game data.
The [Metal backend suite](../metal/README.md) uses Apple's compiler, native SDL
Metal windows, and API/shader validation. Its runner exits 77 with explicit skip
evidence on Linux; that outcome does not represent a Metal compile or GPU test.

`run.py` exercises the actual game with a generated original room, live torch,
and solid pillar. It captures lighting off, on, then off again at simulation tick
zero with ambient occlusion disabled. Lighting must visibly change the world,
leave opaque HUD panels unchanged, and restore the original pixels exactly when
disabled. A game restart verifies that the disabled setting persists in each
renderer. The default `--mode all` additionally compares OpenGL and Vulkan at
each setting and switches OpenGL → Vulkan → OpenGL with lighting enabled. Each
switching capture must match its standalone renderer, and returning to OpenGL
must restore its lit image exactly. The runner verifies the expected context and
surface creation and rejects rendering/validation errors. It retains the fixture, tested executable
hash, isolated configs, screenshots, difference image, logs and `results.json`.

```sh
source /workspace/netblood-env/activate-vulkan.sh
/workspace/netblood-env/with-vulkan-display.sh \
    python3 tests/realtime-lighting/run.py \
    --assets /workspace/netblood-game-data/blood121 --mode all --validation
```

Supply your own Blood 1.21 game assets. `--map` selects an existing scene
instead of generating the fixture; the scene must have active visible lighting
to pass. `--map E1M1 --expect-unlit` instead verifies that the enclosed starting
tomb stays unchanged when the effect is enabled. `--binary` selects the
executable and `--output` a new evidence directory. `--mode opengl` or `vulkan`
selects one renderer; `compare` checks both without renderer switching. Cross
renderer comparisons use the existing tolerance of mean RGB error ≤ 2 and at
most 5% of pixels differing by over 16 in any channel. The exact restoration and
HUD checks do not use that tolerance.
Add `--ambient-occlusion` to verify lighting alongside AO; the default disables
AO to isolate the lighting effect.

On macOS, select Metal explicitly while retaining the same fixture, toggle,
parity and switching checks:

```sh
SDL_CONFIG=/path/to/sdl2-config python3 tests/realtime-lighting/run.py \
    --binary /path/to/development/nblood --assets /path/to/blood121 \
    --native-backend metal --mode all --validation
```

`--native-backend` defaults to `vulkan`; `--mode metal` tests Metal alone. macOS
uses a logged-in Cocoa session and an injected Mach-O SDL input/context
interposer, including verification of `SDL_Metal_CreateView`. Use an unbundled,
unhardened development executable whose SDL dependency matches `SDL_CONFIG`.
The runner copies that executable into its evidence directory; it does not
repackage app bundles or alter signing settings. Selecting Metal on Linux exits
77 and explicitly records that no rendering was tested. Apple compilation and
Metal rendering must be checked on a real Mac.
The runner also needs Python 3/Pillow, SDL2 development headers, `pkg-config`, and
a C compiler for its temporary input driver. These correctness checks do not
measure performance or replace testing on other GPU drivers.
