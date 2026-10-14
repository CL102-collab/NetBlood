# NBlood / Rednukem / PCExhumed
Reverse-engineered ports of Build games using EDuke32 engine technology and development principles

## NBlood
Blood port based on EDuke32.

<img width="320" height="240" alt="screenshot" src="https://github.com/user-attachments/assets/f517d412-ac7f-4002-ab47-35e41bab9f26" />

### Installing
If you have a digital copy of Blood installed, such as from Steam or GOG, no installation is necessary! Otherwise:

1. Extract NBlood to a new directory
2. Copy the following files from Blood 1.21 to NBlood folder:
   * BLOOD.INI
   * BLOOD.RFF
   * BLOOD000.DEM, ..., BLOOD003.DEM (optional)
   * CP01.MAP, ..., CP09.MAP (optional, Cryptic Passage)
   * CPART07.AR_ (optional, Cryptic Passage)
   * CPART15.AR_ (optional, Cryptic Passage)
   * CPBB01.MAP, ..., CPBB04.MAP (optional, Cryptic Passage)
   * CPSL.MAP (optional, Cryptic Passage)
   * CRYPTIC.INI (optional, Cryptic Passage)
   * CRYPTIC.SMK (optional, Cryptic Passage)
   * CRYPTIC.WAV (optional, Cryptic Passage)
   * GUI.RFF
   * SOUNDS.RFF
   * SURFACE.DAT
   * TILES000.ART, ..., TILES017.ART
   * VOXEL.DAT

3. Optionally, if you want to use CD audio tracks instead of MIDI, provide FLAC/OGG recordings in following format: bloodXX.flac/ogg, where XX is track number. Make sure to enable Redbook audio option in sound menu.
4. Optionally, if you want cutscenes and you have the original CD, copy the `movie` folder into NBlood's folder (the folder itself too).
If you have the GOG version of the game, do the following:
   * make a copy of `game.ins` (or `game.inst`) named `game.cue`
   * mount the `.cue` as a virtual CD (for example with `WinCDEmu`)
   * copy the `movie` folder from the mounted CD into NBlood's folder
5. Launch NBlood (on Linux, to play Cryptic Passage, launch with the `-ini CRYPTIC.INI` parameter)

### Notes
NBlood now uses nblood_cvars.cfg instead of settings.cfg. Please rename your settings.cfg if you need to retain settings from a previous version.

### Join links

On Windows, run `nblood.exe -register-uri` once; on Linux, run
`./nblood -register-uri` (requires `xdg-utils`). This registers the current
installation for your user without administrator access. You can then click
`netblood://server.example:23513` links in websites or chat to join a server.
The default port is 23513. Run registration again if you move the executable.
Set up your game assets normally first. Links launch a new game process using
its usual saved settings. See [join-link details](docs/networking.md#join-links-on-windows-and-linux).

### Discord Rich Presence

Discord can display the current map, game mode, elapsed playing time, server
address and party size, with Join and game invitations. Configure a project-owned
Discord Application ID with `discord_appid` in the game console; no ID is bundled.
See [Discord setup and joining](docs/discord.md) for Windows/Linux registration,
listen-host addresses and the `discord_enabled` toggle.

### Public UDP address discovery

Hosted games use Google's public STUN servers to discover their mapped IP and
port for connection links and Discord invitations. No relay service is required.
Use `-stunserver off` to disable discovery or configure a custom server.
STUN alone cannot make every NAT/CGNAT-hosted server reachable.
See [STUN configuration and limits](docs/stun.md).

### Dedicated multiplayer server

Start a headless server with hot-join and server-owned gameplay rules:

```sh
./nblood -dedicated -port 23513 -gametype bloodbath -maxplayers 8 \
  -timelimit 15 -scorelimit 30 -autoaim off -weaponsstay permanent \
  -itemsstay markers -monsters none -maplist rotation.txt
```

Put one map name per line in `rotation.txt` on the game's search path; the list
repeats. For cooperative episode play, use `-gametype coop -episode 1
-difficulty 2 -losekeys off` instead of the score limit and map list.

`-weaponsstay` accepts `norespawn`, `permanent` (default), `respawn`, or
`markers`; `-itemsstay` accepts `norespawn` (default), `respawn`, or `markers`.
`-monsters` accepts `none`, `spawn`, or `respawn`; its default is `spawn` in
co-op and `none` in Bloodbath or teams. All modes are available in every game
type. Respawn markers show pickup locations while items or weapons are absent.
Connected clients cannot change the server's rules or start another game.

Add `-password "crypt key"` to require a password; omitting it leaves the
server open. Clients can enter it under **Multiplayer → Join a Game → Password**
or launch with `-client host -port 23513 -password "crypt key"`. The console
command `password "crypt key"` also sets and remembers the client password;
`password` displays it and `password ""` clears it. Passwords are case-sensitive,
up to 128 bytes, and are sent and saved as plaintext. A saved client password
does not enable a dedicated server password.
Protected URI and Discord joins open a password entry screen automatically
before downloading maps or checking assets. An incorrect password returns to
the prompt so it can be corrected or the connection cancelled.

See [dedicated startup rules](tests/network/README.md#dedicated-startup-rules)
for all parameters, defaults, examples, and validation checks.

## PCExhumed
A port of the PC version of Exhumed based on EDuke32.

<img width="320" height="240" alt="screenshot" src="https://github.com/user-attachments/assets/43ec8959-a6e6-4d8c-a4a9-3d3f9f08d65a" />

### Installing
If you have a digital copy of PowerSlave (DOS Classic Edition) installed, such as from Steam or GOG, no installation is necessary! Otherwise:

1. Extract PCExhumed to a new directory.
2. Copy the following files from the PC retail version of Exhumed or Powerslave (Exhumed preferred), or the Powerslave demo available at http://www.jonof.id.au/build.games/ps. Beta, pre-release or other demo versions not supported.

   * STUFF.DAT
   * DEMO.VCR
   * BOOK.MOV

3. Recommended (but optional) - Add the games CD audio tracks as OGG files in the format exhumedXX.ogg or trackXX.ogg (where XX is the track number) to the same folder as
   pcexhumed.exe. The game includes tracks 02 to 19.
   These will provide the game with its music soundtrack and add storyline narration by the King Ramses NPC.

4. Launch PCExhumed.

### Notes
Demo playback is not yet working.

### Adjusting settings
We are currently working on fancy new menus for the game. In the meantime, you can edit the settings.cfg file in the game directory that's created on first run.

To invert the mouse, add the line 'in_mouseflip 0' to settings.cfg.
To change the FOV, add a new line to settings.cfg, e.g. 'fov "120"' where 120 is the desired FOV value between 60 and 140.

## Rednukem
A port of several BUILD Engine games derived from the Duke Nukem 3D codebase, based on EDuke32.

<img width="320" height="240" alt="screenshot" src="https://github.com/user-attachments/assets/e7650c88-3907-47c3-81eb-2fb012435ca2" />

### Supported games

* Duke Nukem 3D: Atomic Edition
* Redneck Rampage
* Redneck Rampage: Rides Again
* Duke Nukem 64
* NAM/NAPALM
* World War II GI

### Installing
1. Extract Rednukem to a new directory.
2. Copy game data files:
    * Duke Nukem 3D: DUKE3D.GRP, DUKE.RTS
    * Redneck Rampage: REDNECK.GRP, REDNECK.RTS, optionally CD audio tracks as OGG file in the format trackXX.ogg (where XX is the track number)
    * Duke Nukem 64: Duke 64 cartridge ROM dump, optionally MIDI tracks extracted from DUKE3D.GRP
    * NAM: NAM.GRP, NAM.RTS, CON files
    * World War II GI: WW2GI.GRP, WW2GI.RTS, CON files
3. Launch Rednukem

## Building from source
See: https://wiki.eduke32.com/wiki/Main_Page

### Native Vulkan rendering

NBlood offers **Vulkan** in the startup renderer selector and the in-game Video
Mode menu. You can also enter `setrendermode 5` in the console, or set
`Renderer = 5` under `[Screen Setup]` in `nblood.cfg`. The selection is saved;
`setrendermode 3` selects OpenGL Polymost again.

Vulkan needs SDL **2.0.6 or newer**, the Vulkan loader, and a Vulkan-capable device
and driver. On macOS, provide a Vulkan loader and MoltenVK through your SDK/runtime
installation. The native backend shares Polymost's scene and material handling
for world geometry, sprites, models, voxels, maphacks, palettes, shading, fog and
the HUD.
It submits rendering and texture operations directly to Vulkan and creates no
OpenGL context. Polymer remains a separate renderer.
Vulkan currently uses uncompressed GPU textures, so large texture packs can use
more graphics memory than OpenGL with texture compression enabled.

Polymost and Vulkan enable screen-space ambient occlusion by default. Toggle
**Options → Display Setup → Renderer Setup → Ambient Occlusion**, or use
`r_ambientocclusion 0` / `r_ambientocclusion 1` in the console. The setting is
saved and takes effect immediately. AO adds local contact shading around visible
corners and geometry; weapons, crosshairs, text and the HUD are drawn afterward.
Sky and mirror/room-over-room portal surfaces are excluded from the depth samples.
As a screen-space effect, it cannot account for geometry outside the view or
behind another surface. Transparent surfaces use the opaque depth behind them.
OpenGL requires framebuffer blit and packed depth/stencil support; unsupported
hardware keeps rendering normally and reports that AO is unavailable.

Vulkan also enables realtime point lighting and shadows by default. Toggle
**Options → Display Setup → Renderer Setup → Lighting and Shadows**, or use
`r_realtimelighting 0` / `r_realtimelighting 1`. Changes apply immediately and
are saved independently of ambient occlusion. Torches, candles, flames, flares,
Tesla shots, explosions and muzzle flashes illuminate nearby world surfaces.
Up to eight nearby lights cast filtered cubemap shadows from opaque sector
walls, floors, ceilings and moving doors, including geometry outside the view.
Sprites, models and alpha-cutout surfaces receive lighting but do not cast these
map shadows. Transparent surfaces use the opaque depth behind them; sky,
mirror/room-over-room portal surfaces, weapons and HUD are excluded. This feature
requires sampleable scene depth/stencil and a supported depth shadow format;
unsupported devices retain the original rendering.

Vulkan records commands on the calling thread and up to four background threads.
The automatic background count is `min(4, available CPUs - 1)`, so three CPUs
use the caller plus two background workers; five or more use the caller plus
four. Linux CPU affinity is respected. Background workers receive their chunks
before the caller records its own, and draw batches retain their original
execution order. Small batches run entirely on the caller; background threads
are created only when needed.

`NBLOOD_VULKAN_WORKERS` sets the total recorder count, including the caller. Set
it to `1` for synchronous recording, or a positive integer to lower the automatic
total of `max(1, min(5, available CPUs))`. Requests above that limit are clamped;
`0` or an unset variable selects the automatic count:

```sh
NBLOOD_VULKAN_WORKERS=1 ./nblood
```

Worker threads prepare descriptors, copy draw data, and record Vulkan commands.
Polymost scene traversal, pipeline creation, and GPU submission remain serial.
The backend uses two submission slots by default, allowing the CPU to prepare
the next frame while submitted GPU work runs. Each slot has its own command
pools, descriptors, upload buffer, and completion fence. Reusing a busy slot
waits for its fence; normal submission and presentation do not wait for the
entire queue to become idle. Screenshots and explicit completion requests still
wait for their results, and swapchain recreation and shutdown drain the device.
Shared color and depth targets retain their contents and ordered rendering.

`NBLOOD_VULKAN_FRAMES_IN_FLIGHT` selects one, two, or three submission slots.
More slots allow additional queued work at the cost of memory and potential
input latency. Set it to `1` for comparison with a single reusable slot:

```sh
NBLOOD_VULKAN_FRAMES_IN_FLIGHT=1 ./nblood
```

Startup and shutdown logs report recording activity, submission counts, fence
waits, and observed pending GPU work.

For GNU Make, install the SDL2 and Vulkan development packages. Vulkan is enabled
automatically when `pkg-config --exists vulkan` succeeds; it can be requested or
disabled explicitly:

```sh
make -j4 blood USE_VULKAN=1 SDL_TARGET=2
# To build without Vulkan:
make -j4 blood USE_VULKAN=0
```

Keep the default `USE_OPENGL=1` build setting: it includes the shared Polymost
frontend. SDL 1.2, non-SDL builds, and `USE_OPENGL=0` disable Vulkan. Rebuild cleanly
when changing these compile-time feature flags.

For Visual Studio, install the LunarG Vulkan SDK **before starting Visual Studio**
so it inherits `VULKAN_SDK`. Vulkan enables automatically for x64 when the SDK has
`Include/vulkan/vulkan.h` and `Lib/vulkan-1.lib`; Win32 requires `Lib32/vulkan-1.lib`.
MSBuild's `/p:UseVulkan=true` requires the SDK, while `/p:UseVulkan=false` disables
Vulkan. ARM64 Visual Studio configurations currently exclude Vulkan.

Ordinary builds use the checked-in SPIR-V header. After editing
the Vulkan shaders or shared `source/build/src/ambient_occlusion.glsl`,
regenerate the shader headers with Python 3,
`glslangValidator`, and `spirv-val` on `PATH`:

```sh
python3 tests/vulkan/generate-shaders.py
python3 tests/vulkan/generate-shaders.py --check
```

[Rendering checks](tests/vulkan/README.md) cover native backend operations,
OpenGL/Vulkan scene comparisons and switching renderers. Those checks cover
selected scenes and fixtures; they do not establish complete Polymost parity or
coverage of every platform and driver. Use the demo suite below separately to
check simulation regressions. [Ambient occlusion checks](tests/ambient-occlusion/README.md)
cover contact shading, live toggling, HUD isolation and renderer parity.

### Demo regression validation

The DOS Blood 1.21 demo regression suite is available through `-validate`.
See [tests/demo-validation](tests/demo-validation/README.md) for the headless
runner, required game data, and expected results.

## Acknowledgments
  See AUTHORS.md

