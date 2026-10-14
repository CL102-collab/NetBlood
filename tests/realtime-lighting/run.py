#!/usr/bin/env python3
"""Check OpenGL/native lighting, live toggles, renderer switching and persistence.

The default scene is an original room with a live torch and a solid pillar.
Requires SDL2 development files, Pillow and supplied Blood 1.21 assets.
Linux uses DISPLAY; macOS uses a logged-in Cocoa session and DYLD interposition.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
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


def macos_input_driver_source():
    # Mach-O interposition does not rely on ELF symbol preemption. Only this
    # unsigned test dylib is injected into the supplied development executable.
    source = render_checks.INPUT_DRIVER
    source = source.replace('#include <SDL_vulkan.h>', '#include <SDL_vulkan.h>\n#include <SDL_metal.h>')
    source = source.replace('static unsigned gl_attempts, gl_contexts, vulkan_surfaces;',
                            'static unsigned gl_attempts, gl_contexts, vulkan_surfaces, metal_views;')
    source = source.replace('SDL_GLContext SDL_GL_CreateContext(', 'static SDL_GLContext lighting_GL_CreateContext(')
    source = source.replace('SDL_bool SDL_Vulkan_CreateSurface(', 'static SDL_bool lighting_Vulkan_CreateSurface(')
    source = source.replace('int SDL_PollEvent(', 'static int lighting_PollEvent(')
    # dyld leaves references from the interposing image bound to the original
    # function. Use those references directly when forwarding each call.
    for function in ('SDL_GL_CreateContext', 'SDL_Vulkan_CreateSurface', 'SDL_PollEvent'):
        source = source.replace(f'if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "{function}");',
                                f'if (!original) original = {function};')
    source = source.replace('vulkan_surfaces=%u\\n",', 'vulkan_surfaces=%u metal_views=%u\\n",')
    source = source.replace('gl_attempts, gl_contexts, vulkan_surfaces);',
                            'gl_attempts, gl_contexts, vulkan_surfaces, metal_views);')
    source += r'''
static SDL_MetalView lighting_Metal_CreateView(SDL_Window *window)
{
    static SDL_MetalView (*original)(SDL_Window *);
    if (!original) original = SDL_Metal_CreateView;
    SDL_MetalView result = original ? original(window) : NULL;
    if (result) ++metal_views;
    fprintf(stderr, "RENDER_TEST_METAL_VIEW: %s\n", result ? "created" : "failed");
    return result;
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *original;
} const lighting_interposes[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)lighting_GL_CreateContext, (const void *)SDL_GL_CreateContext},
    {(const void *)lighting_Vulkan_CreateSurface, (const void *)SDL_Vulkan_CreateSurface},
    {(const void *)lighting_PollEvent, (const void *)SDL_PollEvent},
    {(const void *)lighting_Metal_CreateView, (const void *)SDL_Metal_CreateView},
};
'''
    return source


def build_input_driver(output):
    if sys.platform != 'darwin':
        return render_checks.build_input_driver(output)
    source_path = output / 'input_driver.c'
    source_path.write_text(macos_input_driver_source())
    library = output / 'input_driver.dylib'
    sdl_config = os.environ.get('SDL_CONFIG', 'sdl2-config')
    cflags = shlex.split(subprocess.check_output([sdl_config, '--cflags'], text=True))
    libraries = shlex.split(subprocess.check_output([sdl_config, '--libs'], text=True))
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-Wall', '-Wextra', '-Werror', '-dynamiclib']
    command += shlex.split(os.environ.get('CPPFLAGS', '')) + shlex.split(os.environ.get('CFLAGS', ''))
    command += cflags + [str(source_path), '-o', str(library)]
    command += shlex.split(os.environ.get('LDFLAGS', '')) + libraries
    subprocess.run(command, check=True)
    return library


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


def run_game(command, runtime, env, log_path, timeout, renderer, native_backend='vulkan'):
    errors = []
    with log_path.open('w') as log:
        try:
            completed = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, timeout=timeout)
            if completed.returncode:
                errors.append(f'exit status {completed.returncode}')
        except subprocess.TimeoutExpired:
            errors.append('timed out')
        except OSError as error:
            errors.append(str(error))
    log = log_path.read_text(errors='replace')
    errors += [line for line in log.splitlines() if re.search(
        r'\b(?:ERROR|FATAL)\||VUID-|Validation Error|SYNC-HAZARD|GL_INVALID_|unknown (?:command|symbol)'
        r'|Metal (?:API|Shader) Validation.*(?:error|failed)|validate\w*:.*failed|GPU (?:fault|error)|MTLCommandBufferError', line, re.I)]
    contexts = re.search(r'RENDER_TEST_CONTEXTS: gl_attempts=(\d+) gl_contexts=(\d+) vulkan_surfaces=(\d+)'
                         r'(?: metal_views=(\d+))?', log)
    if not contexts:
        errors.append('missing renderer context evidence')
    elif renderer == 'vulkan' and (int(contexts[1]) != 0 or int(contexts[3]) < 1 or int(contexts[4] or 0) != 0):
        errors.append('expected a native Vulkan surface and no OpenGL context attempts')
    elif renderer == 'opengl' and (int(contexts[2]) < 1 or int(contexts[3]) != 0 or int(contexts[4] or 0) != 0):
        errors.append('expected an OpenGL context without Vulkan surfaces or Metal views')
    elif renderer == 'metal' and (int(contexts[1]) != 0 or int(contexts[3]) != 0 or int(contexts[4] or 0) < 1):
        errors.append('expected a native Metal view without OpenGL context attempts or Vulkan surfaces')
    elif renderer == 'switch':
        if int(contexts[2]) < 2 or (native_backend == 'vulkan' and int(contexts[3]) < 1):
            errors.append(f'switching did not create both OpenGL contexts and the {native_backend} backend')
        if native_backend == 'metal' and (int(contexts[3]) != 0 or int(contexts[4] or 0) < 1):
            errors.append('switching did not create a native Metal view without Vulkan surfaces')
    return log, errors


def prepare_runtime(args, output, binary, library, renderer, lighting):
    runtime = output / renderer
    runtime.mkdir()
    (runtime / 'lighting.cfg').write_text(
        '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n[Screen Setup]\n'
        f'Renderer = {dict(opengl=3, switch=3, vulkan=5, metal=6)[renderer]}\n'
        'ScreenMode = 0\nScreenBPP = 32\nScreenWidth = 800\nScreenHeight = 600\n')
    map_name = args.map or 'VFLIGHT'
    bindings = '\n'.join(f'bind {key} norepeat "exec step-{index}.cfg"'
                         for index, key in enumerate(('I', 'O', 'P', 'L')))
    (runtime / 'autoexec.cfg').write_text(
        'r_maxfps 60\nr_showfps 0\nhud_showmaptitle 0\nr_texcompr 0\nr_texcache 0\n'
        f'r_ambientocclusion {int(args.ambient_occlusion)}\nr_realtimelighting {lighting}\n' + bindings + '\n')
    command = [str(binary), '-usecwd', '-nosetup', '-noautoload', '-quick', '-nodemo',
               '-cfg', 'lighting.cfg', '-j=' + str(args.assets.resolve()), '-j=' + str(ROOT), '-map', map_name]
    if not args.map:
        fixture_path = output / 'fixture'
        command += ['-j=' + str(fixture_path), '-h=lighting.def']
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='cocoa' if sys.platform == 'darwin' else 'x11', SDL_AUDIODRIVER='dummy',
               RENDER_TEST_INITIAL_MS=str(args.initial_delay_ms), RENDER_TEST_INTERVAL_MS='1500',
               RENDER_TEST_PRESSES='4', MESA_DEBUG='context')
    if sys.platform == 'darwin':
        env['DYLD_INSERT_LIBRARIES'] = str(library) + (':' + env['DYLD_INSERT_LIBRARIES'] if env.get('DYLD_INSERT_LIBRARIES') else '')
    else:
        env['LD_PRELOAD'] = str(library) + (' ' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    if args.validation:
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
        env['VK_LAYER_ENABLES'] = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
        if sys.platform == 'darwin':
            env['MTL_DEBUG_LAYER'] = env['MTL_SHADER_VALIDATION'] = '1'
    return runtime, command, env


def toggle_case(args, output, binary, library, renderer):
    runtime, command, env = prepare_runtime(args, output, binary, library, renderer, 0)
    map_name = args.map or 'VFLIGHT'
    scripts = (f'map "{map_name}"\npause\necho LIGHTING_FROZEN\n',
               'screenshot\nr_realtimelighting 1\necho LIGHTING_OFF_CAPTURED\n',
               'screenshot\nr_realtimelighting 0\necho LIGHTING_ON_CAPTURED\n',
               'screenshot\necho LIGHTING_RESTORED_CAPTURED\nquit\n')
    for index, script in enumerate(scripts):
        (runtime / f'step-{index}.cfg').write_text(script)
    print(f'Checking {renderer} lighting: off / on / off', flush=True)
    log, errors = run_game(command, runtime, env, runtime / 'console.log', args.timeout, renderer)
    for marker in ('Paused at simulation frame 0.', 'LIGHTING_FROZEN', 'LIGHTING_OFF_CAPTURED',
                   'LIGHTING_ON_CAPTURED', 'LIGHTING_RESTORED_CAPTURED'):
        if marker not in log:
            errors.append('missing marker: ' + marker)
    if not args.map and 'Definitions file "lighting.def" loaded' not in log:
        errors.append('fixture definitions were not loaded')
    paths = sorted((runtime / 'screenshots').glob('*.png'))
    result = {'renderer': renderer, 'errors': errors}
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
    print(f'Checking saved {renderer} lighting preference after restart', flush=True)
    restart_log, restart_errors = run_game(command, runtime, env, runtime / 'restart.log', args.timeout, renderer)
    errors += ['restart: ' + error for error in restart_errors]
    if 'LIGHTING_RESTARTED' not in restart_log or "'r_realtimelighting' is '0'" not in restart_log:
        errors.append('lighting-off preference did not survive restart')
    result['passed'] = not errors
    (runtime / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result, paths


def switch_case(args, output, binary, library, captures):
    runtime, command, env = prepare_runtime(args, output, binary, library, 'switch', 1)
    map_name = args.map or 'VFLIGHT'
    scripts = (f'map "{map_name}"\npause\necho LIGHTING_FROZEN\n',
               f'screenshot\nsetrendermode {6 if args.native_backend == "metal" else 5}\necho LIGHTING_SWITCH_0\n',
               'screenshot\nsetrendermode 3\necho LIGHTING_SWITCH_1\n',
               'screenshot\necho LIGHTING_SWITCH_2\nquit\n')
    for index, script in enumerate(scripts):
        (runtime / f'step-{index}.cfg').write_text(script)
    print(f'Checking lit renderer switching: OpenGL / {args.native_backend} / OpenGL', flush=True)
    log, errors = run_game(command, runtime, env, runtime / 'console.log', args.timeout, 'switch', args.native_backend)
    for marker in ('Paused at simulation frame 0.', 'LIGHTING_FROZEN', 'LIGHTING_SWITCH_0',
                   'LIGHTING_SWITCH_1', 'LIGHTING_SWITCH_2'):
        if marker not in log:
            errors.append('missing marker: ' + marker)
    paths = sorted((runtime / 'screenshots').glob('*.png'))
    comparisons = []
    if len(paths) != 3:
        errors.append(f'expected 3 switching screenshots, found {len(paths)}')
    else:
        for index, renderer in enumerate(('opengl', args.native_backend, 'opengl')):
            render_checks.inspect_image(paths[index], (800, 600))
            if len(captures[renderer]) == 3:
                comparison = render_checks.compare_images(
                    captures[renderer][1], paths[index], runtime / f'difference-{index}.png', 2.0)
                comparisons.append(dict(renderer=renderer, **comparison))
                if not comparison['passed']:
                    errors.append(f'{renderer} switching frame {index} differs from standalone lit scene')
        restoration = render_checks.compare_images(paths[0], paths[2], runtime / 'restoration.png', 0.0, 0.0)
        comparisons.append(dict(renderer='opengl restoration', **restoration))
        if not restoration['passed']:
            errors.append('returning to OpenGL did not restore its lit image exactly')
    result = {'renderer': 'switch', 'errors': errors, 'comparisons': comparisons, 'passed': not errors}
    (runtime / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'nblood')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mode', choices=('opengl', 'vulkan', 'metal', 'compare', 'all'), default='all',
                        help='all also checks switching with lighting enabled (default)')
    parser.add_argument('--native-backend', choices=('vulkan', 'metal'), default='vulkan',
                        help='native renderer used by compare/all (default: vulkan)')
    parser.add_argument('--map', help='Use an existing scene instead of the generated torch fixture')
    parser.add_argument('--expect-unlit', action='store_true',
                        help='Require an unchanged image for a scene without visible light contributions')
    parser.add_argument('--ambient-occlusion', action='store_true',
                        help='Also enable ambient occlusion to check both effects together')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--initial-delay-ms', type=int, default=20000)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='netblood-lighting-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    if sys.platform != 'darwin' and (args.mode == 'metal' or
                                    (args.mode in ('compare', 'all') and args.native_backend == 'metal')):
        result = {'status': 'skipped', 'runtime': 'unavailable', 'exit_code': 77,
                  'reason': 'Native Metal game rendering requires macOS; no rendering was tested.'}
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f"SKIP: {result['reason']} Evidence: {output}")
        return 77
    binary = output / 'nblood-tested'
    shutil.copy2(args.binary.resolve(), binary)
    library = build_input_driver(output)
    if not args.map:
        generate_fixture(output / 'fixture')
    renderers = ('opengl', args.native_backend) if args.mode in ('compare', 'all') else (args.mode,)
    cases, captures, parity = [], {}, []
    for renderer in renderers:
        case, captures[renderer] = toggle_case(args, output, binary, library, renderer)
        cases.append(case)
    if len(renderers) == 2 and all(len(captures[renderer]) == 3 for renderer in renderers):
        for index, setting in enumerate(('off', 'on', 'restored')):
            comparison = render_checks.compare_images(
                captures['opengl'][index], captures[args.native_backend][index], output / f'parity-{setting}.png', 2.0)
            parity.append(dict(setting=setting, **comparison))
    if args.mode == 'all':
        cases.append(switch_case(args, output, binary, library, captures))
    result = {'map': args.map or 'VFLIGHT', 'mode': args.mode, 'expect_unlit': args.expect_unlit,
              'native_backend': args.native_backend,
              'ambient_occlusion': args.ambient_occlusion,
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'cases': cases, 'parity': parity,
              'passed': all(case['passed'] for case in cases)
                        and (len(renderers) == 1 or len(parity) == 3)
                        and all(comparison['passed'] for comparison in parity)}
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(f'Evidence: {output}')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
