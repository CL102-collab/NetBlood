#!/usr/bin/env python3
"""Check Vulkan game lighting, exact toggle restoration, HUD and saved settings.

The default scene is an original room with a live torch and a solid pillar.
Requires DISPLAY, SDL2 development files, Pillow and supplied Blood 1.21 assets.
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
import subprocess
import tempfile
import zlib

from PIL import ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


render_checks = load_module('render_checks', ROOT / 'tests/vulkan/run.py')
fixtures = load_module('fixture_generator', ROOT / 'tests/vulkan/make-fixtures.py')


def generate_fixture(directory):
    """One sector with a reverse-wound interior loop for a solid pillar."""
    directory.mkdir()
    for kind in ('wall', 'floor', 'ceiling', 'sprite'):
        (directory / f'lighting-{kind}.png').write_bytes(fixtures.pattern_png(kind))
    torch_tile = 28010
    definitions = ['include "blood.def"']
    for tile, kind in ((fixtures.WALL_TILE, 'wall'), (fixtures.FLOOR_TILE, 'floor'),
                       (fixtures.CEILING_TILE, 'ceiling'), (torch_tile, 'sprite'),
                       (torch_tile + 1, 'sprite')):
        definitions += [f'tilefromtexture {tile} {{ file "lighting-{kind}.png" }}',
                        f'texture {tile} {{ pal 0 {{ file "lighting-{kind}.png" }} }}']
    (directory / 'lighting.def').write_text('\n'.join(definitions) + '\n')

    torch = list(fixtures.SPRITE.unpack(fixtures.map_sprite(
        torch_tile, 2304, 1700, -1536, repeat=16)))
    torch[20] = 30  # kDecorationTorch, statnum=kStatDecoration, active without an XSPRITE.
    outer = ((0, 0), (4096, 0), (4096, 4096), (0, 4096))
    pillar = ((2600, 1800), (2600, 2300), (2900, 2300), (2900, 1800))
    data = bytearray(b'BLM\x1a' + struct.pack('<H', 0x600))
    data += fixtures.MAP_HEADER.pack(1024, 2048, -4096, 0, 0, 0, 32, 0, 0, 1, 1, 8, 1)
    data += struct.pack('<h', 0)
    data += fixtures.SECTOR.pack(0, 8, -12288, 0, 0, 0,
                                fixtures.CEILING_TILE, 0, 4, 0, 0, 0,
                                fixtures.FLOOR_TILE, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1)
    for base, loop in ((0, outer), (4, pillar)):
        for index, (x, y) in enumerate(loop):
            data += fixtures.WALL.pack(x, y, base + (index + 1) % 4, -1, -1, 0,
                                       fixtures.WALL_TILE, 0, 4, 0, 16, 16, 0, 0, 0, 0, -1)
    data += fixtures.SPRITE.pack(*torch)
    data += struct.pack('<I', zlib.crc32(data))
    assert len(data) == 6 + 37 + 2 + 40 + 8 * 32 + 44 + 4
    assert zlib.crc32(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
    (directory / 'VFLIGHT.MAP').write_bytes(data)
    (directory / 'fixture.json').write_text(json.dumps({
        'map': 'VFLIGHT', 'map_sha256': hashlib.sha256(data).hexdigest(),
        'features': ['active torch', 'solid interior pillar', 'opaque floor and walls'],
        'license': 'Original generated fixture data; no copied game assets.',
    }, indent=2) + '\n')


def difference(first, second):
    image = ImageChops.difference(first, second)
    return {'mean_rgb': sum(ImageStat.Stat(image).mean) / 3, 'bbox': image.getbbox()}


def run_game(command, runtime, env, log_path, timeout):
    errors = []
    with log_path.open('w') as log:
        try:
            completed = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, timeout=timeout)
            if completed.returncode:
                errors.append(f'exit status {completed.returncode}')
        except subprocess.TimeoutExpired:
            errors.append('timed out')
    log = log_path.read_text(errors='replace')
    errors += [line for line in log.splitlines() if re.search(
        r'\b(?:ERROR|FATAL)\||VUID-|Validation Error|SYNC-HAZARD|unknown (?:command|symbol)', line, re.I)]
    contexts = re.search(r'RENDER_TEST_CONTEXTS: gl_attempts=(\d+) gl_contexts=(\d+) vulkan_surfaces=(\d+)', log)
    if not contexts or int(contexts[1]) != 0 or int(contexts[3]) < 1:
        errors.append('expected a native Vulkan surface and no OpenGL context attempts')
    return log, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'nblood')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--map', help='Use an existing scene such as E1M1 instead of the generated torch fixture')
    parser.add_argument('--expect-unlit', action='store_true',
                        help='Require an unchanged image for a scene without visible light contributions')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--initial-delay-ms', type=int, default=20000)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='netblood-lighting-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    binary = output / 'nblood-tested'
    shutil.copy2(args.binary.resolve(), binary)
    library = render_checks.build_input_driver(output)
    runtime = output / 'vulkan'
    runtime.mkdir()
    (runtime / 'lighting.cfg').write_text(
        '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n[Screen Setup]\n'
        'Renderer = 5\nScreenMode = 0\nScreenBPP = 32\nScreenWidth = 800\nScreenHeight = 600\n')
    map_name = args.map or 'VFLIGHT'
    bindings = '\n'.join(f'bind {key} norepeat "exec step-{index}.cfg"'
                         for index, key in enumerate(('I', 'O', 'P', 'L')))
    (runtime / 'autoexec.cfg').write_text(
        'r_maxfps 60\nr_showfps 0\nhud_showmaptitle 0\nr_texcompr 0\nr_texcache 0\n'
        'r_ambientocclusion 0\nr_realtimelighting 0\n' + bindings + '\n')
    scripts = (f'map "{map_name}"\npause\necho LIGHTING_FROZEN\n',
               'screenshot\nr_realtimelighting 1\necho LIGHTING_OFF_CAPTURED\n',
               'screenshot\nr_realtimelighting 0\necho LIGHTING_ON_CAPTURED\n',
               'screenshot\necho LIGHTING_RESTORED_CAPTURED\nquit\n')
    for index, script in enumerate(scripts):
        (runtime / f'step-{index}.cfg').write_text(script)
    command = [str(binary), '-usecwd', '-nosetup', '-noautoload', '-quick', '-nodemo',
               '-cfg', 'lighting.cfg', '-j=' + str(args.assets.resolve()), '-j=' + str(ROOT), '-map', map_name]
    if not args.map:
        fixture_path = output / 'fixture'
        generate_fixture(fixture_path)
        command += ['-j=' + str(fixture_path), '-h=lighting.def']
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
               RENDER_TEST_INITIAL_MS=str(args.initial_delay_ms), RENDER_TEST_INTERVAL_MS='1500',
               RENDER_TEST_PRESSES='4', MESA_DEBUG='context')
    env['LD_PRELOAD'] = str(library) + (' ' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    if args.validation:
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
        env['VK_LAYER_ENABLES'] = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
    print('Checking Vulkan lighting: off / on / off', flush=True)
    log, errors = run_game(command, runtime, env, runtime / 'console.log', args.timeout)
    for marker in ('Paused at simulation frame 0.', 'LIGHTING_FROZEN', 'LIGHTING_OFF_CAPTURED',
                   'LIGHTING_ON_CAPTURED', 'LIGHTING_RESTORED_CAPTURED'):
        if marker not in log:
            errors.append('missing marker: ' + marker)
    if not args.map and 'Definitions file "lighting.def" loaded' not in log:
        errors.append('fixture definitions were not loaded')
    paths = sorted((runtime / 'screenshots').glob('*.png'))
    result = {'map': map_name, 'expect_unlit': args.expect_unlit,
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'errors': errors}
    if len(paths) != 3:
        errors.append(f'expected 3 screenshots, found {len(paths)}')
    else:
        images = [render_checks.inspect_image(path, (800, 600)) for path in paths]
        result['off_on'] = difference(images[0], images[1])
        result['off_restored'] = difference(images[0], images[2])
        result['status_panels'] = [difference(images[0].crop(box), images[1].crop(box))
                                   for box in ((0, 523, 168, 600), (625, 523, 800, 600))]
        ImageChops.difference(images[0], images[1]).save(runtime / 'lighting-difference.png')
        if args.expect_unlit and result['off_on']['bbox'] is not None:
            errors.append('lighting changed a scene expected to remain unlit')
        elif not args.expect_unlit and result['off_on']['mean_rgb'] <= 0.01:
            errors.append('point lighting did not visibly affect the scene')
        if result['off_restored']['bbox'] is not None:
            errors.append('disabling lighting did not restore the original image exactly')
        if any(panel['bbox'] is not None for panel in result['status_panels']):
            errors.append('lighting changed the opaque status panels')
    saved_cvars = runtime / 'lighting_cvars.cfg'
    if not saved_cvars.exists() or not re.search(r'^r_realtimelighting\s+"?0"?\s*$', saved_cvars.read_text(), re.M):
        errors.append('lighting-off preference was not saved')
    # Remove the override and query the saved setting in a fresh process.
    (runtime / 'autoexec.cfg').write_text('bind I norepeat "exec restart.cfg"\n')
    (runtime / 'restart.cfg').write_text('r_realtimelighting\necho LIGHTING_RESTARTED\nquit\n')
    env['RENDER_TEST_PRESSES'] = '1'
    print('Checking saved lighting preference after restart', flush=True)
    restart_log, restart_errors = run_game(command, runtime, env, runtime / 'restart.log', args.timeout)
    errors += ['restart: ' + error for error in restart_errors]
    if 'LIGHTING_RESTARTED' not in restart_log or "'r_realtimelighting' is '0'" not in restart_log:
        errors.append('lighting-off preference did not survive restart')
    result['passed'] = not errors
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(f'Evidence: {output}')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
