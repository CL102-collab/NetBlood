#!/usr/bin/env python3
"""Check Rednukem's native renderers, AO, lighting, HUD and saved settings.

Requires supplied Duke Nukem 3D assets (including 1.3D shareware), SDL2 headers,
Python/Pillow and an X11 display or macOS desktop. No assets are downloaded.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import zlib

from PIL import ImageChops

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lighting = load_module('lighting_checks', ROOT / 'tests/realtime-lighting/run.py')
render = lighting.render_checks
fixtures = lighting.fixtures
RENDERERS = dict(opengl=3, vulkan=5, metal=6, switch=3)


def generate_fixture(directory):
    """Original Build v7 room with a burning barrel and a solid interior pillar."""
    directory.mkdir()
    definitions = ['animtilerange 1240 1240 0 0']  # ART animation otherwise runs while paused.
    for tile, kind in ((fixtures.WALL_TILE, 'wall'), (fixtures.FLOOR_TILE, 'floor'),
                       (fixtures.CEILING_TILE, 'ceiling')):
        (directory / f'{kind}.png').write_bytes(fixtures.pattern_png(kind))
        definitions += [f'tilefromtexture {tile} {{ file "{kind}.png" }}',
                        f'texture {tile} {{ pal 0 {{ file "{kind}.png" }} }}']
    (directory / 'fixture.def').write_text('\n'.join(definitions) + '\n')
    # A small original CON keeps actor animation and unrelated gameplay out of
    # image comparisons. It also lets shareware art run without its older CON
    # dialect, which Rednukem's normal Atomic Edition game path does not support.
    (directory / 'fixture.con').write_text(
        'definevolumename 0 RENDERING FIXTURE\ndefineskillname 0 FIXTURE\n'
        'definelevelname 0 0 RNLIGHT.MAP 00:00 00:00 RENDERING FIXTURE\n'
        'gamestartup 512 10 100 0 768 768 53200 176 1780 2500 650 3880 2500 2500 2048 '
        '200 50 200 50 50 50 99 10 99 50 1 3 0 64 0\n'
        'actor 1240 1\nenda\n')
    outer = ((0, 0), (4096, 0), (4096, 4096), (0, 4096))
    pillar = ((2600, 1800), (2600, 2300), (2900, 2300), (2900, 1800))
    data = bytearray(struct.pack('<iiiihhH', 7, 1024, 2048, -10240, 0, 0, 1))
    data += fixtures.SECTOR.pack(0, 8, -16384, 0, 0, 0,
                                fixtures.CEILING_TILE, 0, 16, 0, 0, 0,
                                fixtures.FLOOR_TILE, 0, 16, 0, 0, 0, 0, 0, 0, 0, -1)
    data += struct.pack('<H', 8)
    for base, loop in ((0, outer), (4, pillar)):
        for index, (x, y) in enumerate(loop):
            data += fixtures.WALL.pack(x, y, base + (index + 1) % 4, -1, -1, 0,
                                       fixtures.WALL_TILE, 0, 16, 0, 16, 16, 0, 0, 0, 0, -1)
    data += struct.pack('<H', 1)
    data += fixtures.map_sprite(1240, 2304, 1700, 0, repeat=32)  # FIREBARREL
    assert len(data) == 22 + 40 + 2 + 8 * 32 + 2 + 44
    (directory / 'RNLIGHT.MAP').write_bytes(data)
    group = b'KenSilverman' + struct.pack('<I12sI', 1, b'RNLIGHT.MAP', len(data)) + data
    (directory / 'fixture.grp').write_bytes(group)
    # Make the original map group the explicitly selected base game; supplied
    # Duke art is loaded afterward. Shareware's map reader only searches group0.
    (directory / 'fixture.grpinfo').write_text(
        f'grpinfo {{ name "Original renderer fixture" crc {zlib.crc32(group)} '
        f'size {len(group)} flags GAMEFLAG_DUKE dependency 0 }}\n')
    (directory / 'fixture.json').write_text(json.dumps({
        'map': 'RNLIGHT.MAP', 'sha256': hashlib.sha256(data).hexdigest(),
        'features': ['burning barrel', 'solid pillar', 'original textured room'],
        'license': 'Original generated map and texture data; no copied game assets.',
    }, indent=2) + '\n')


def build_input_driver(output):
    # Rednukem suppresses bound commands while paused. Feed actual console input,
    # opening and closing the console within one event-poll batch so captures
    # contain the world and HUD, without a transient console overlay.
    driver = r'''
int SDL_PollEvent(SDL_Event *event)
{
    static int (*original)(SDL_Event *);
    static unsigned initialized, presses, limit, stage;
    static Uint32 next_press, interval;
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!original) return 0;
    if (!initialized) {
        next_press = (Uint32)strtoul(getenv("RENDER_TEST_INITIAL_MS"), NULL, 10);
        interval = (Uint32)strtoul(getenv("RENDER_TEST_INTERVAL_MS"), NULL, 10);
        limit = (unsigned)strtoul(getenv("RENDER_TEST_PRESSES"), NULL, 10);
        initialized = 1;
    }
    int result = original(event);
    if (result || !event || SDL_GetTicks() < next_press) return result;
    memset(event, 0, sizeof(*event));
    if (presses >= limit) {
        // Dismiss Duke's ordinary exit screens after the final quit command.
        event->type = stage == 0 ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.keysym.scancode = SDL_SCANCODE_SPACE;
        event->key.keysym.sym = SDLK_SPACE;
        event->key.state = stage == 0 ? SDL_PRESSED : SDL_RELEASED;
        stage ^= 1; next_press = SDL_GetTicks() + 500;
        return 1;
    }
    int restart = getenv("RENDER_TEST_SKIP_PAUSE") != NULL;
    if (!restart && presses == 0) {
        event->type = stage == 0 ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.keysym.scancode = SDL_SCANCODE_PAUSE;
        event->key.keysym.sym = SDLK_PAUSE;
        if (stage++ == 0) next_press = SDL_GetTicks() + 100;
        else { stage = 0; ++presses; next_press = SDL_GetTicks() + interval; }
    } else {
        if (stage == 2) {
            event->type = SDL_TEXTINPUT;
            if (restart) snprintf(event->text.text, sizeof(event->text.text), "exec restart.cfg");
            else snprintf(event->text.text, sizeof(event->text.text), "exec step-%u.cfg", presses - 1);
        } else {
            int enter = stage == 3 || stage == 4;
            event->type = (stage == 0 || stage == 3 || stage == 5) ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.keysym.scancode = enter ? SDL_SCANCODE_RETURN : SDL_SCANCODE_GRAVE;
            event->key.keysym.sym = enter ? SDLK_RETURN : SDLK_BACKQUOTE;
        }
        if (++stage == 7) {
            fprintf(stderr, "REDNUKEM_TEST_COMMAND: %u at %u ms\n", presses, SDL_GetTicks());
            stage = 0; ++presses; next_press = SDL_GetTicks() + interval;
        }
    }
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)
        event->key.state = event->type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    return 1;
}
'''
    source = render.INPUT_DRIVER
    start = source.index('int SDL_PollEvent(')
    end = source.index('__attribute__((destructor))', start)
    render.INPUT_DRIVER = source[:start] + driver + '\n' + source[end:]
    return lighting.build_input_driver(output)


def prepare_runtime(args, output, binary, library, renderer, effects=False):
    runtime = output / renderer
    runtime.mkdir()
    (runtime / 'render.cfg').write_text(
        '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n[Screen Setup]\n'
        f'Renderer = {RENDERERS[renderer]}\n'
        'ScreenMode = 0\nScreenBPP = 32\nScreenWidth = 800\nScreenHeight = 600\n')
    (runtime / 'autoexec.cfg').write_text(
        'r_maxfps 60\nr_showfps 0\nhud_showmapname 0\nr_texcompr 0\nr_texcache 0\n'
        'r_size 8\nhud_scale 100\ncl_viewbob 0\ncl_weaponsway 0\n'
        f'r_ambientocclusion {int(effects)}\nr_realtimelighting {int(effects)}\n')
    command = [str(binary), '-usecwd', '-nosetup', '-noautoload', '-nologo', '-ns', '-nm',
               '-cfg', 'render.cfg', '-j', str(args.assets.resolve()),
               '-j', str(args.grp.resolve().parent), '-gamegrp', 'fixture.grp', '-j', str(output / 'fixture'),
               '-g', args.grp.name, '-h', 'fixture.def', '-x', 'fixture.con',
               '-v1', '-l1', '-s1']
    env = os.environ.copy()
    env.pop('RENDER_TEST_SKIP_PAUSE', None)
    env.update(SDL_VIDEODRIVER='cocoa' if sys.platform == 'darwin' else 'x11', SDL_AUDIODRIVER='dummy',
               RENDER_TEST_INITIAL_MS=str(args.initial_delay_ms), RENDER_TEST_INTERVAL_MS='1500',
               RENDER_TEST_PRESSES='6', MESA_DEBUG='context')
    preload = 'DYLD_INSERT_LIBRARIES' if sys.platform == 'darwin' else 'LD_PRELOAD'
    separator = ':' if sys.platform == 'darwin' else ' '
    env[preload] = str(library) + (separator + env[preload] if env.get(preload) else '')
    if args.validation:
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
        env['VK_LAYER_ENABLES'] = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
        if sys.platform == 'darwin':
            env['MTL_DEBUG_LAYER'] = env['MTL_SHADER_VALIDATION'] = '1'
    return runtime, command, env


def run_case(args, runtime, command, env, renderer, scripts):
    for index, script in enumerate(scripts):
        (runtime / f'step-{index}.cfg').write_text(script + f'\necho REDNUKEM_CAPTURE_{index}\n'
                                                 + ('quit\n' if index == len(scripts) - 1 else ''))
    env['RENDER_TEST_PRESSES'] = str(len(scripts) + 1)
    log, errors = lighting.run_game(command, runtime, env, runtime / 'console.log',
                                   args.timeout, renderer, args.native_backend)
    for index in range(len(scripts)):
        if f'REDNUKEM_CAPTURE_{index}' not in log:
            errors.append(f'missing capture marker {index}')
    # premap logs the episode/level slot, not its map filename. The original CON
    # maps that slot exclusively to RNLIGHT.MAP, absent from supplied game data;
    # a failed load cannot reach this successful level-entry message.
    if 'E1L1: RENDERING FIXTURE' not in log or 'Loading fixture.def' not in log:
        errors.append('the generated RNLIGHT fixture was not loaded')
    paths = sorted((runtime / 'screenshots').glob('duke*.png'))
    images = []
    if len(paths) != len(scripts):
        errors.append(f'expected {len(scripts)} screenshots, found {len(paths)}')
    else:
        images = [render.inspect_image(path, (800, 600)) for path in paths]
    return errors, paths, images


def toggle_case(args, output, binary, library, renderer):
    runtime, command, env = prepare_runtime(args, output, binary, library, renderer)
    print(f'Checking {renderer}: AO and lighting toggles, HUD and saved preferences', flush=True)
    scripts = ('screenshot\nr_ambientocclusion 1', 'screenshot\nr_ambientocclusion 0',
               'screenshot\nr_realtimelighting 1', 'screenshot\nr_realtimelighting 0',
               'screenshot\nr_ambientocclusion 1\nr_realtimelighting 1',
               'screenshot\nr_ambientocclusion 0\nr_realtimelighting 0')
    errors, paths, images = run_case(args, runtime, command, env, renderer, scripts)
    metrics = {}
    if images:
        for effect, enabled, restored in (('ambient-occlusion', 1, 2), ('lighting', 3, 4)):
            metrics[effect] = lighting.difference(images[0], images[enabled])
            if metrics[effect]['mean_rgb'] <= .01:
                errors.append(f'{effect} did not visibly change the room')
            if lighting.difference(images[0], images[restored])['bbox'] is not None:
                errors.append(f'{effect} disabling did not restore the scene exactly')
            # Full-width opaque Duke status bar: the view ends above this region.
            hud = (0, 508, 800, 600)
            if lighting.difference(images[0].crop(hud), images[enabled].crop(hud))['bbox'] is not None:
                errors.append(f'{effect} changed the opaque status bar')
            ImageChops.difference(images[0], images[enabled]).save(runtime / f'{effect}-difference.png')
    saved = runtime / 'render_settings.cfg'
    for cvar in ('r_ambientocclusion', 'r_realtimelighting'):
        if not saved.exists() or not re.search(rf'^{cvar}\s+"?0"?\s*$', saved.read_text(), re.M):
            errors.append(cvar + ' disabled preference was not saved')
    (runtime / 'autoexec.cfg').write_text('')
    (runtime / 'restart.cfg').write_text('r_ambientocclusion\nr_realtimelighting\necho REDNUKEM_RESTARTED\nquit\n')
    env['RENDER_TEST_SKIP_PAUSE'] = '1'
    env['RENDER_TEST_PRESSES'] = '1'
    log, restart_errors = lighting.run_game(command, runtime, env, runtime / 'restart.log',
                                           args.timeout, renderer, args.native_backend)
    errors += ['restart: ' + error for error in restart_errors]
    for marker in ('REDNUKEM_RESTARTED', "'r_ambientocclusion' is '0'", "'r_realtimelighting' is '0'"):
        if marker not in log:
            errors.append('missing restart marker: ' + marker)
    result = dict(renderer=renderer, errors=errors, effects=metrics, passed=not errors)
    (runtime / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result, paths


def switch_case(args, output, binary, library, captures):
    runtime, command, env = prepare_runtime(args, output, binary, library, 'switch', effects=True)
    print(f'Checking OpenGL / {args.native_backend} / OpenGL with both effects enabled', flush=True)
    scripts = (f'screenshot\nsetrendermode {RENDERERS[args.native_backend]}',
               'screenshot\nsetrendermode 3', f'screenshot\nsetrendermode {RENDERERS[args.native_backend]}')
    errors, paths, images = run_case(args, runtime, command, env, 'switch', scripts)
    comparisons = []
    if images:
        comparisons.append(render.compare_images(paths[0], paths[1], runtime / 'parity.png', 2.0))
        comparisons.append(render.compare_images(paths[0], paths[2], runtime / 'restored.png', 0.0, 0.0))
        for index, renderer in enumerate(('opengl', args.native_backend, 'opengl')):
            if len(captures[renderer]) == 6:
                comparisons.append(render.compare_images(captures[renderer][5], paths[index],
                                                          runtime / f'standalone-{index}.png', 0.5))
            else:
                errors.append(f'missing standalone combined-effects {renderer} capture')
        if not all(result['passed'] for result in comparisons):
            errors.append('renderer switching changed the scene beyond the comparison tolerance')
    if not re.search(rf'^Renderer\s*=\s*{RENDERERS[args.native_backend]}\s*$',
                     (runtime / 'render.cfg').read_text(), re.M):
        errors.append('the switched native renderer preference was not saved')
    (runtime / 'autoexec.cfg').write_text('')
    (runtime / 'restart.cfg').write_text('echo REDNUKEM_RENDERER_RESTARTED\nquit\n')
    env['RENDER_TEST_SKIP_PAUSE'] = '1'
    env['RENDER_TEST_PRESSES'] = '1'
    log, restart_errors = lighting.run_game(command, runtime, env, runtime / 'restart.log',
                                           args.timeout, args.native_backend, args.native_backend)
    errors += ['renderer restart: ' + error for error in restart_errors]
    if 'REDNUKEM_RENDERER_RESTARTED' not in log:
        errors.append('missing renderer restart completion marker')
    result = dict(renderer='switch', comparisons=comparisons, errors=errors, passed=not errors)
    (runtime / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--grp', type=Path, help='Main Duke GRP; defaults to DUKE3D.GRP in --assets')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mode', choices=('opengl', 'vulkan', 'metal', 'all'), default='all')
    parser.add_argument('--native-backend', choices=('vulkan', 'metal'), default='vulkan')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--initial-delay-ms', type=int, default=15000)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    args.grp = args.grp or next((path for path in args.assets.iterdir() if path.name.lower() == 'duke3d.grp'),
                               args.assets / 'DUKE3D.GRP')
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-rendering-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    if sys.platform != 'darwin' and (args.mode == 'metal' or
                                    args.mode == 'all' and args.native_backend == 'metal'):
        result = dict(status='skipped', reason='Native Metal rendering requires macOS; no rendering was tested.',
                      exit_code=77)
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f"SKIP: {result['reason']} Evidence: {output}")
        return 77
    binary = output / 'rednukem-tested'
    shutil.copy2(args.binary.resolve(), binary)
    library = build_input_driver(output)
    generate_fixture(output / 'fixture')
    renderers = ('opengl', args.native_backend) if args.mode == 'all' else (args.mode,)
    cases, captures, parity = [], {}, []
    for renderer in renderers:
        case, captures[renderer] = toggle_case(args, output, binary, library, renderer)
        cases.append(case)
    if args.mode == 'all':
        if all(len(captures[renderer]) == 6 for renderer in renderers):
            for index, label in enumerate(('off', 'ao', 'ao-restored', 'lighting', 'lighting-restored', 'combined')):
                parity.append(dict(setting=label, **render.compare_images(
                    captures['opengl'][index], captures[args.native_backend][index], output / f'parity-{label}.png', 2.0)))
        cases.append(switch_case(args, output, binary, library, captures))
    result = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  grp_sha256=hashlib.sha256(args.grp.read_bytes()).hexdigest(),
                  native_backend=args.native_backend, cases=cases, parity=parity,
                  passed=all(case['passed'] for case in cases) and all(item['passed'] for item in parity)
                         and (args.mode != 'all' or len(parity) == 6))
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(f'Evidence: {output}')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
