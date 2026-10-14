#!/usr/bin/env python3
"""Check live AO toggling, restoration, persistence and GL/Vulkan parity.

Requires DISPLAY, SDL2 development files, Pillow and separately supplied Blood
1.21 assets. Uses the ordinary game executable and an isolated configuration.
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
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('render_checks', ROOT / 'tests/vulkan/run.py')
render_checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(render_checks)


def difference(first, second):
    image = ImageChops.difference(first, second)
    return {'mean_rgb': sum(ImageStat.Stat(image).mean) / 3,
            'bbox': image.getbbox()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'nblood')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--map', default='E1M1')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--wait-ready', action='store_true',
                        help='wait for the current executable\'s level-ready diagnostic before input')
    parser.add_argument('--initial-delay-ms', type=int, default=20000)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='netblood-ao-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    binary = output / 'nblood-tested'
    shutil.copy2(args.binary.resolve(), binary)
    library = render_checks.build_input_driver(output)
    results, captures = [], {}
    for renderer in (3, 5):
        runtime = output / ('polymost' if renderer == 3 else 'vulkan')
        runtime.mkdir()
        (runtime / 'ao.cfg').write_text(
            '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n[Screen Setup]\n'
            f'Renderer = {renderer}\nScreenMode = 0\nScreenBPP = 32\n'
            'ScreenWidth = 800\nScreenHeight = 600\n')
        bindings = '\n'.join(f'bind {key} norepeat "exec step-{index}.cfg"'
                             for index, key in enumerate(('I', 'O', 'P', 'L')))
        (runtime / 'autoexec.cfg').write_text(
            'r_maxfps 60\nr_showfps 0\nhud_showmaptitle 0\nr_texcompr 0\nr_texcache 0\n'
            'r_ambientocclusion 0\nr_realtimelighting 0\n' + bindings + '\n')
        scripts = (f'map "{args.map}"\npause\necho AO_FROZEN\n',
                   'screenshot\nr_ambientocclusion 1\necho AO_OFF_CAPTURED\n',
                   'screenshot\nr_ambientocclusion 0\necho AO_ON_CAPTURED\n',
                   'screenshot\necho AO_RESTORED_CAPTURED\nquit\n')
        for index, script in enumerate(scripts):
            (runtime / f'step-{index}.cfg').write_text(script)
        command = [str(binary), '-usecwd', '-nosetup', '-noautoload', '-quick', '-nodemo',
                   '-cfg', 'ao.cfg', '-j=' + str(args.assets.resolve()), '-j=' + str(ROOT),
                   '-map', args.map]
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
                   RENDER_TEST_INITIAL_MS=str(args.initial_delay_ms),
                   RENDER_TEST_INTERVAL_MS='1500', RENDER_TEST_PRESSES='4', MESA_DEBUG='context')
        if args.wait_ready:
            env['RENDER_TEST_READY_LOG'] = str(runtime / 'nblood.log')
        env['LD_PRELOAD'] = str(library) + (' ' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
        if args.validation:
            env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
        print(f'Checking {runtime.name}: off / on / off', flush=True)
        errors = []
        (runtime / 'nblood.log').unlink(missing_ok=True)
        with (runtime / 'console.log').open('w') as log:
            try:
                completed = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, timeout=args.timeout)
                if completed.returncode:
                    errors.append(f'exit status {completed.returncode}')
            except subprocess.TimeoutExpired:
                errors.append('timed out')
        log = (runtime / 'console.log').read_text(errors='replace')
        errors += [line for line in log.splitlines() if re.search(
            r'\b(?:ERROR|FATAL)\||VUID-|Validation Error|SYNC-HAZARD|GL_INVALID_|unknown (?:command|symbol)|ambient occlusion.*failed', line, re.I)]
        for marker in ('Paused at simulation frame 0.', 'AO_FROZEN', 'AO_OFF_CAPTURED',
                       'AO_ON_CAPTURED', 'AO_RESTORED_CAPTURED'):
            if marker not in log:
                errors.append('missing marker: ' + marker)
        paths = sorted((runtime / 'screenshots').glob('*.png'))
        result = {'renderer': runtime.name, 'errors': errors}
        if len(paths) != 3:
            errors.append(f'expected 3 screenshots, found {len(paths)}')
        else:
            images = [render_checks.inspect_image(path, (800, 600)) for path in paths]
            captures[renderer] = paths
            result['off_on'] = difference(images[0], images[1])
            result['off_restored'] = difference(images[0], images[2])
            # The default HUD has opaque panels in the two bottom corners;
            # the world remains visible between them and may receive AO.
            result['status_panels'] = [difference(images[0].crop(box), images[1].crop(box))
                                       for box in ((0, 523, 168, 600), (625, 523, 800, 600))]
            ImageChops.difference(images[0], images[1]).save(runtime / 'ao-difference.png')
            if result['off_on']['mean_rgb'] <= 0.01:
                errors.append('AO did not visibly affect the scene')
            if result['off_restored']['bbox'] is not None:
                errors.append('disabling AO did not restore the original image exactly')
            if any(panel['bbox'] is not None for panel in result['status_panels']):
                errors.append('AO changed the opaque status panels')
        saved_cvars = runtime / 'ao_cvars.cfg'
        if not saved_cvars.exists() or not re.search(r'^r_ambientocclusion\s+"?0"?\s*$', saved_cvars.read_text(), re.M):
            errors.append('AO-off preference was not saved')
        # Restart without overriding AO. Query the saved cvar through the console.
        (runtime / 'autoexec.cfg').write_text('bind I norepeat "exec restart.cfg"\n')
        (runtime / 'restart.cfg').write_text('r_ambientocclusion\necho AO_RESTARTED\nquit\n')
        env['RENDER_TEST_PRESSES'] = '1'
        # Never let the input driver mistake the previous process's readiness
        # message for the restarted game's completed loading phase.
        (runtime / 'nblood.log').unlink(missing_ok=True)
        with (runtime / 'restart.log').open('w') as log_file:
            try:
                completed = subprocess.run(command, cwd=runtime, env=env, stdout=log_file,
                                           stderr=subprocess.STDOUT, timeout=args.timeout)
                if completed.returncode:
                    errors.append(f'restart exit status {completed.returncode}')
            except subprocess.TimeoutExpired:
                errors.append('restart timed out')
        restart_log = (runtime / 'restart.log').read_text(errors='replace')
        if 'AO_RESTARTED' not in restart_log or "'r_ambientocclusion' is '0'" not in restart_log:
            errors.append('AO-off preference did not survive restart')
        result['passed'] = not errors
        results.append(result)
    parity = []
    if len(captures) == 2:
        for index, label in enumerate(('off', 'on', 'restored')):
            comparison = render_checks.compare_images(captures[3][index], captures[5][index],
                                                       output / f'parity-{label}.png', 2.0)
            parity.append(dict(setting=label, **comparison))
    result = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'cases': results, 'parity': parity,
              'passed': all(case['passed'] for case in results) and len(parity) == 3
                        and all(case['passed'] for case in parity)}
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(f'Evidence: {output}')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
