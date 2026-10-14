#!/usr/bin/env python3
"""Render paused Blood scenes with OpenGL/Vulkan and retain comparison evidence.

Linux/SDL2 runner. An existing X11 DISPLAY and legally supplied Blood 1.21 game
data are required; the runner does not download assets or start an X server.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat


INPUT_DRIVER = r'''
#define _GNU_SOURCE
#include <SDL.h>
#include <SDL_vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned gl_attempts, gl_contexts, vulkan_surfaces;

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    static SDL_GLContext (*original)(SDL_Window *);
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_GL_CreateContext");
    ++gl_attempts;
    SDL_GLContext result = original ? original(window) : NULL;
    if (result) ++gl_contexts;
    fprintf(stderr, "RENDER_TEST_GL_CONTEXT: %s\n", result ? "created" : "failed");
    return result;
}

SDL_bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, VkSurfaceKHR *surface)
{
    static SDL_bool (*original)(SDL_Window *, VkInstance, VkSurfaceKHR *);
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_Vulkan_CreateSurface");
    SDL_bool result = original ? original(window, instance, surface) : SDL_FALSE;
    if (result) ++vulkan_surfaces;
    fprintf(stderr, "RENDER_TEST_VULKAN_SURFACE: %s\n", result ? "created" : "failed");
    return result;
}

int SDL_PollEvent(SDL_Event *event)
{
    static int (*original)(SDL_Event *);
    static unsigned initialized, presses, down, limit, input_polls;
    static Uint32 next_press, release_at, interval;
    static char const *ready_log;
    static unsigned ready;
    static Uint32 check_ready_at, ready_at;
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!original) return 0;
    if (!initialized) {
        next_press = (Uint32)strtoul(getenv("RENDER_TEST_INITIAL_MS"), NULL, 10);
        interval = (Uint32)strtoul(getenv("RENDER_TEST_INTERVAL_MS"), NULL, 10);
        limit = (unsigned)strtoul(getenv("RENDER_TEST_PRESSES"), NULL, 10);
        ready_log = getenv("RENDER_TEST_READY_LOG");
        ready = !ready_log || !*ready_log;
        initialized = 1;
    }
    int result = original(event);
    if (result || !event) return result;
    // A heavily loaded renderer can be descheduled past release_at while
    // draining events. End the drain and give ordinary input processing a
    // chance to consume the key before emitting its opposite transition.
    if (input_polls) {
        --input_polls;
        return 0;
    }
    Uint32 now = SDL_GetTicks();
    // Startup/precache also pumps SDL events. Optional log synchronization keeps
    // the first key out of those loading loops, where it would be discarded.
    // The game creates this ordinary diagnostic after completing StartLevel;
    // callers remove the previous process's log before launching a new one.
    if (!ready && now >= check_ready_at) {
        check_ready_at = now + 50;
        FILE *log = fopen(ready_log, "r");
        if (log) {
            char line[1024];
            while (fgets(line, sizeof(line), log)) {
                if (strstr(line, "Level ready: map=")) {
                    ready = 1;
                    ready_at = now + 250;
                    fprintf(stderr, "RENDER_TEST_READY: level finished loading at %u ms\n", now);
                    break;
                }
            }
            fclose(log);
        }
    }
    if (!ready || now < ready_at) return 0;
    if ((!down && presses < limit && now >= next_press) || (down && now >= release_at)) {
        memset(event, 0, sizeof(*event));
        event->type = down ? SDL_KEYUP : SDL_KEYDOWN;
        event->key.type = event->type;
        event->key.timestamp = now;
        event->key.state = down ? SDL_RELEASED : SDL_PRESSED;
        static SDL_Scancode const scans[] = { SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P,
                                            SDL_SCANCODE_L, SDL_SCANCODE_U, SDL_SCANCODE_K };
        static SDL_Keycode const symbols[] = { SDLK_i, SDLK_o, SDLK_p, SDLK_l, SDLK_u, SDLK_k };
        unsigned key = down ? presses - 1 : presses;
        event->key.keysym.scancode = scans[key];
        event->key.keysym.sym = symbols[key];
        if (!down) {
            ++presses;
            release_at = now + 1000;
            fprintf(stderr, "RENDER_TEST_KEY: %u at %u ms\n", presses, now);
        } else next_press = now + interval;
        down = !down;
        input_polls = 2;
        return 1;
    }
    return 0;
}

__attribute__((destructor)) static void report_contexts(void)
{
    fprintf(stderr, "RENDER_TEST_CONTEXTS: gl_attempts=%u gl_contexts=%u vulkan_surfaces=%u\n",
            gl_attempts, gl_contexts, vulkan_surfaces);
}
'''


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return value


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[2] / 'nblood')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--engine-data', type=Path, help='Directory containing nblood.pk3; defaults to binary directory')
    parser.add_argument('--fixtures', type=Path, help='Directory generated by make-fixtures.py; selects its maps and definitions')
    parser.add_argument('--defs', type=Path, help='Additional definitions file (overrides fixture definitions)')
    parser.add_argument('--search-path', action='append', type=Path, default=[], help='Additional asset search directory; repeat as needed')
    parser.add_argument('--output', type=Path, help='New evidence directory; defaults to a temporary directory')
    parser.add_argument('--map', action='append', dest='maps', help='Repeat for additional scenes; default E1M1')
    parser.add_argument('--mode', choices=('opengl', 'vulkan', 'compare', 'switch', 'bridge', 'lifecycle', 'all'), default='compare')
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--initial-delay-ms', type=int, default=25000)
    parser.add_argument('--wait-ready', action='store_true',
                        help='wait for the current executable\'s level-ready diagnostic before input')
    parser.add_argument('--interval-ms', type=int, default=2000)
    parser.add_argument('--max-mean-error', type=float, default=2.0,
                        help='Maximum mean RGB difference in 0..255 units (default 2)')
    parser.add_argument('--max-changed-fraction', type=float, default=0.05,
                        help='Maximum fraction of pixels differing by more than 16 in any channel (default 0.05)')
    parser.add_argument('--vulkan-workers', type=positive_int, metavar='N',
                        help='Total Vulkan recorders including caller; sets NBLOOD_VULKAN_WORKERS (omitted uses environment/auto)')
    parser.add_argument('--require-parallel', action='store_true',
                        help='Require Vulkan caller recording to overlap a distinct background recorder')
    parser.add_argument('--frames-in-flight', type=int, choices=(1, 2, 3),
                        help='Vulkan submission slots; sets NBLOOD_VULKAN_FRAMES_IN_FLIGHT (omitted uses environment/default 2)')
    parser.add_argument('--require-inflight', action='store_true',
                        help='Require at least two submitted fences observed unfinished together')
    parser.add_argument('--validation', action='store_true', help='Enable installed Khronos Vulkan validation layer')
    parser.add_argument('--disable-opengl', action='store_true',
                        help='Require Vulkan to render while OpenGL library loading is deliberately disabled')
    return parser.parse_args()


def build_input_driver(output):
    source = output / 'input_driver.c'
    library = output / 'input_driver.so'
    source.write_text(INPUT_DRIVER)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-Wall', '-Wextra', '-Werror', '-shared', '-fPIC']
    command += shlex.split(os.environ.get('CFLAGS', '')) + flags
    command += [str(source), '-o', str(library), '-ldl']
    subprocess.run(command, check=True)
    return library


def write_console_files(runtime, map_name, sequence, final_capture_only=False):
    # The first binding executes only after ordinary -map startup. Calling map
    # synchronously again and then pause freezes the identical reset world before
    # another simulation tick, regardless of driver initialization duration.
    # Use identical uncompressed source texels: legacy OpenGL's default DXT
    # compression otherwise changes model skin colors before rasterization.
    (runtime / 'autoexec.cfg').write_text(
        'r_maxfps 60\nhud_showmaptitle 0\nr_models 1\nr_voxels 1\nr_hightile 1\n'
        'r_texcompr 0\nr_texcache 0\nr_realtimelighting 0\n'
        'bind I norepeat "exec prepare.cfg"\n'
        'bind O norepeat "exec capture-0.cfg"\n'
        'bind P norepeat "exec capture-1.cfg"\n'
        'bind L norepeat "exec capture-2.cfg"\n'
        'bind U norepeat "exec capture-3.cfg"\n'
        'bind K norepeat "exec capture-4.cfg"\n')
    (runtime / 'prepare.cfg').write_text(
        f'map "{map_name}"\npause\necho RENDER_TEST_FROZEN\n')
    capture_count = len(sequence)
    for index in range(capture_count):
        commands = [] if final_capture_only and index + 1 < capture_count else ['screenshot']
        commands += [f'echo RENDER_TEST_CAPTURE_{index}']
        if index + 1 < capture_count:
            commands += [f'setrendermode {sequence[index + 1]}']
        else:
            commands += ['quit']
        (runtime / f'capture-{index}.cfg').write_text('\n'.join(commands) + '\n')
    return capture_count


def inspect_image(path, expected):
    with Image.open(path) as source:
        image = source.convert('RGB')
        if image.size != expected:
            raise ValueError(f'{path}: expected {expected}, got {image.size}')
        colors = image.getcolors(maxcolors=32)
        if colors is not None and len(colors) < 8:
            raise ValueError(f'{path}: screenshot has fewer than eight colors')
        return image.copy()


def difference_mask(difference):
    red, green, blue = difference.split()
    return ImageChops.lighter(ImageChops.lighter(red, green), blue).point(lambda value: 255 if value > 16 else 0)


def compare_images(first, second, output, threshold, max_changed_fraction=0.05, mask=None):
    with Image.open(first) as a, Image.open(second) as b:
        difference = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
    difference.save(output)
    stats = ImageStat.Stat(difference, mask)
    # Report the maximum over the same pixels used for the focused mean.
    relevant_difference = Image.composite(difference, Image.new('RGB', difference.size), mask) if mask else difference
    mean_error = sum(stats.mean) / 3
    changed_fraction = ImageStat.Stat(difference_mask(difference), mask).mean[0] / 255
    return {'first': str(first), 'second': str(second), 'difference': str(output),
            'mean_rgb_error': mean_error, 'max_rgb_error': max(high for low, high in relevant_difference.getextrema()),
            'changed_pixel_fraction': changed_fraction,
            'threshold': threshold, 'max_changed_fraction': max_changed_fraction,
            'passed': mean_error <= threshold and changed_fraction <= max_changed_fraction}


def recording_evidence(log, uses_vulkan, requested_workers=None, require_parallel=False):
    """Keep each lifetime's caller/background configuration and recording evidence."""
    lifetimes = []
    configuration = None
    configuration_pattern = re.compile(
        r'Vulkan recording configuration: available_cores=(\d+) workers=(\d+) '
        r'background_workers=(\d+) requested=(\d+) caller_thread_id=(\d+)')
    summary_pattern = re.compile(
        r'Vulkan recording stats: workers=(\d+) started_workers=(\d+) background_workers=(\d+) '
        r'started_background_workers=(\d+) caller_thread_id=(\d+) peak_active=(\d+) batches=(\d+) '
        r'parallel_batches=(\d+) caller_parallel_batches=(\d+) draws=(\d+) '
        r'secondary_buffers=(\d+) dispatch_ns=(\d+)')
    worker_pattern = re.compile(
        r'Vulkan recording worker (\d+): thread_id=(\d+) draws=(\d+) record_ns=(\d+)')
    for line in log.splitlines():
        startup = configuration_pattern.search(line)
        summary = summary_pattern.search(line)
        if startup:
            configuration = dict(zip(('available_cores', 'workers', 'background_workers',
                                      'requested', 'caller_thread_id'), map(int, startup.groups())))
        elif summary:
            stats = dict(zip(('workers', 'started_workers', 'background_workers', 'started_background_workers',
                              'caller_thread_id', 'peak_active', 'batches', 'parallel_batches',
                              'caller_parallel_batches', 'draws', 'secondary_buffers', 'dispatch_ns'),
                             map(int, summary.groups())))
            stats['configuration'] = configuration
            configuration = None
            stats['threads'] = []
            lifetimes.append(stats)
        elif lifetimes and (worker := worker_pattern.search(line)):
            entry = dict(zip(('worker', 'thread_id', 'draws', 'record_ns'), map(int, worker.groups())))
            entry['role'] = 'caller' if entry['worker'] == 0 else 'background'
            lifetimes[-1]['threads'].append(entry)
    errors = []
    if uses_vulkan:
        if requested_workers is not None and not lifetimes:
            errors.append('missing Vulkan recording statistics for requested recorder count')
        for stats in lifetimes:
            config = stats['configuration']
            if not config:
                if requested_workers is not None or require_parallel:
                    errors.append('missing Vulkan recording startup configuration')
                continue
            if config['available_cores'] < 1:
                errors.append('Vulkan recording reported no available CPU cores')
            limit = max(1, min(5, config['available_cores']))
            requested = requested_workers if requested_workers is not None else config['requested']
            expected = min(requested, limit) if requested else limit
            stats['automatic_recorder_limit'] = limit
            stats['expected_recorders'] = expected
            if requested_workers is not None and config['requested'] != requested_workers:
                errors.append('Vulkan startup did not receive --vulkan-workers')
            if stats['workers'] != expected or config['workers'] != expected:
                errors.append('Vulkan total recorder count does not match the request clamped to its CPU limit')
            if stats['background_workers'] != expected - 1 or config['background_workers'] != expected - 1:
                errors.append('Vulkan background recorder count does not reserve one slot for the caller')
            if (stats['started_workers'] > expected or stats['peak_active'] > stats['started_workers']
                    or (stats['draws'] > 0 and stats['started_workers'] == 0)
                    or stats['started_background_workers'] != max(0, stats['started_workers'] - 1)):
                errors.append('Vulkan started recorder counts are inconsistent with caller participation')
            if not stats['caller_parallel_batches'] <= stats['parallel_batches'] <= stats['batches']:
                errors.append('Vulkan caller/background overlap counters exceed their enclosing batch counts')
            caller_id = stats['caller_thread_id']
            if not caller_id or caller_id != config['caller_thread_id']:
                errors.append('Vulkan caller identity differs between startup and teardown')
            stats['caller_recorded'] = any(worker['worker'] == 0 and worker['thread_id'] == caller_id
                                           and worker['draws'] > 0 for worker in stats['threads'])
            if stats['draws'] > 0 and not stats['caller_recorded']:
                errors.append('Vulkan recorded draws without proving slot zero ran on the caller')
            background_recorded = any(worker['worker'] > 0 and worker['thread_id']
                                      and worker['thread_id'] != caller_id and worker['draws'] > 0
                                      for worker in stats['threads'])
            stats['caller_background_overlap'] = (stats['caller_recorded'] and background_recorded
                and stats['peak_active'] >= 2 and stats['parallel_batches'] > 0
                and stats['caller_parallel_batches'] > 0)
        if require_parallel and not any(stats.get('caller_background_overlap', False) for stats in lifetimes):
            errors.append('Vulkan did not prove caller recording overlapped a distinct background recorder')
    return lifetimes, errors


def submission_evidence(log, uses_vulkan, requested_frames=None, require_inflight=False):
    """Distinguish submitted/unretired work from fences actually polled unfinished."""
    lifetimes, errors = [], []
    configuration = None
    configuration_pattern = re.compile(r'Vulkan submission configuration: frames_in_flight=(\d+)')
    fields = ('frames_in_flight', 'submissions', 'completed', 'pending', 'max_pending',
              'max_observed_unsignaled', 'presents', 'fence_waits', 'slot_reuse_waits',
              'finish_waits', 'readback_waits', 'queue_idle_waits', 'device_idle_waits',
              'retired_images', 'pending_retired_images')
    summary_pattern = re.compile('Vulkan submission stats: ' + ' '.join(
        name + r'=(\d+)' for name in fields))
    for line in log.splitlines():
        if startup := configuration_pattern.search(line):
            if uses_vulkan and configuration is not None:
                errors.append('Vulkan submission startup lacks matching teardown statistics')
            configuration = {'frames_in_flight': int(startup.group(1))}
        elif summary := summary_pattern.search(line):
            stats = dict(zip(fields, map(int, summary.groups())))
            stats['configuration'] = configuration
            configuration = None
            stats['observed_multiple_unfinished'] = (stats['max_pending'] >= 2
                                                     and stats['max_observed_unsignaled'] >= 2)
            lifetimes.append(stats)
    if uses_vulkan:
        if configuration is not None:
            errors.append('Vulkan submission startup lacks matching teardown statistics')
        if not lifetimes:
            errors.append('missing Vulkan submission statistics')
        for stats in lifetimes:
            config = stats['configuration']
            slots = stats['frames_in_flight']
            if not config:
                errors.append('missing Vulkan submission startup configuration')
            elif config['frames_in_flight'] != slots:
                errors.append('Vulkan submission slot count differs between startup and teardown')
            if slots not in (1, 2, 3) or (requested_frames is not None and slots != requested_frames):
                errors.append('Vulkan submission slot count does not match the supported request')
            if not 0 <= stats['max_observed_unsignaled'] <= stats['max_pending'] <= slots:
                errors.append('Vulkan outstanding submission counters exceed the slot count')
            if stats['completed'] != stats['submissions'] or stats['pending'] != 0:
                errors.append('Vulkan submissions were not completely drained at teardown')
            if stats['pending_retired_images'] != 0:
                errors.append('Vulkan retired images remain pending at teardown')
            if stats['max_pending'] > stats['submissions'] or stats['presents'] > stats['submissions']:
                errors.append('Vulkan pending/presentation counts exceed submitted work')
            if stats['queue_idle_waits'] != 0:
                errors.append('Vulkan used a queue-idle wait during submission')
        if require_inflight and not any(stats['observed_multiple_unfinished'] for stats in lifetimes):
            errors.append('Vulkan did not observe at least two submitted fences unfinished together')
    return lifetimes, errors


def run_case(args, output, library, map_name, renderer, switching=False, bridge=False, lifecycle=False):
    label = re.sub(r'[^a-zA-Z0-9_.-]', '_', map_name)
    runtime = output / f'{label}-{"lifecycle" if lifecycle else "bridge" if bridge else "switch" if switching else renderer}'
    runtime.mkdir()
    sequence = [5, 0, 5, 3, 5] if lifecycle else [3, 0, 5, 0, 3] if bridge else [3, 5, 3] if switching else [renderer]
    captures = write_console_files(runtime, map_name, sequence, final_capture_only=lifecycle)
    expected_screenshots = 1 if lifecycle else captures
    (runtime / 'render-test.cfg').write_text(
        '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n\n[Screen Setup]\n'
        f'Renderer = {renderer}\nScreenMode = 0\nScreenBPP = 32\n'
        f'ScreenWidth = {args.width}\nScreenHeight = {args.height}\n')
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
               RENDER_TEST_INITIAL_MS=str(args.initial_delay_ms),
               RENDER_TEST_INTERVAL_MS=str(args.interval_ms),
               RENDER_TEST_PRESSES=str(captures + 1))
    if getattr(args, 'wait_ready', False):
        # Every run_case has a newly created directory, so no prior process's
        # diagnostic can accidentally satisfy this readiness condition.
        env['RENDER_TEST_READY_LOG'] = str(runtime / 'nblood.log')
    requested_workers = getattr(args, 'vulkan_workers', None)
    require_parallel = getattr(args, 'require_parallel', False)
    requested_frames = getattr(args, 'frames_in_flight', None)
    require_inflight = getattr(args, 'require_inflight', False)
    if requested_workers is not None:
        env['NBLOOD_VULKAN_WORKERS'] = str(requested_workers)
    if requested_frames is not None:
        env['NBLOOD_VULKAN_FRAMES_IN_FLIGHT'] = str(requested_frames)
    env['LD_PRELOAD'] = str(library) + (' ' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    if args.validation:
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
    missing_gl = runtime / 'intentionally-missing-libGL.so'
    if args.disable_opengl:
        env['SDL_OPENGL_LIBRARY'] = str(missing_gl)
        env['SDL_VIDEO_GL_DRIVER'] = str(missing_gl)
    command = [str(args.binary), '-usecwd', '-nosetup', '-noautoload', '-quick', '-nodemo',
               '-cfg', 'render-test.cfg', '-j=' + str(args.assets), '-j=' + str(args.engine_data),
               '-map', map_name]
    command += ['-j=' + str(path) for path in args.search_path]
    if args.defs:
        # Build's virtual filesystem resolves asset names through search paths;
        # an absolute DEF filename can fail silently despite the option parsing.
        command += ['-j=' + str(args.defs.parent), '-h=' + args.defs.name]
    print(f'Running {runtime.name}', flush=True)
    log_path = runtime / 'console.log'
    timed_out = False
    with log_path.open('w') as log:
        try:
            process = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=args.timeout)
            exit_code = process.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            exit_code = None
    log = log_path.read_text(errors='replace')
    errors = []
    if timed_out or exit_code != 0:
        errors.append('timed out' if timed_out else f'exit status {exit_code}')
    for marker in ['Paused at simulation frame 0.', 'RENDER_TEST_FROZEN'] + [f'RENDER_TEST_CAPTURE_{i}' for i in range(captures)]:
        if marker not in log:
            errors.append(f'missing marker: {marker}')
    keys = re.findall(r'RENDER_TEST_KEY: (\d+) at (\d+) ms', log)
    if len(keys) != captures + 1 or any(int(keys[i][1]) - int(keys[i-1][1]) < args.interval_ms
                                      for i in range(1, len(keys))):
        errors.append('capture keys did not wait the required render interval after preparing or switching')
    missing_gl_lines = 0
    for line in log.splitlines():
        expected_gl_failure = (args.disable_opengl and 'ERROR| Failed loading OpenGL driver: Failed loading ' in line
                               and str(missing_gl) in line and line.endswith('; all OpenGL modes are unavailable.'))
        if expected_gl_failure:
            missing_gl_lines += 1
        elif re.search(r'\b(?:ERROR|FATAL)\||Validation Error|VUID-|SYNC-HAZARD|unknown (?:command|symbol)|not a (?:valid|recognized)', line, re.I):
            errors.append(line.strip())
    if args.disable_opengl and missing_gl_lines != 1:
        errors.append('expected exactly one deliberate OpenGL library loading failure')
    if args.defs and f'Definitions file "{args.defs.name}" loaded' not in log:
        errors.append('requested definitions file was not loaded')
    if map_name in args.manifest.get('requires_maphack', []) and not re.search(
            rf'Loaded map hack file [^\n]*{re.escape(map_name)}\.mhk', log, re.I):
        errors.append('fixture maphack was not loaded')
    context_match = re.search(r'RENDER_TEST_CONTEXTS: gl_attempts=(\d+) gl_contexts=(\d+) vulkan_surfaces=(\d+)', log)
    contexts = None
    if not context_match:
        errors.append('missing native API context evidence')
    else:
        contexts = dict(zip(('gl_attempts', 'gl_contexts', 'vulkan_surfaces'), map(int, context_match.groups())))
        if renderer == 5 and not lifecycle and (contexts['gl_attempts'] != 0 or contexts['vulkan_surfaces'] < 1):
            errors.append('Vulkan-only run did not prove a Vulkan surface with zero OpenGL context attempts')
        if renderer == 3 and contexts['gl_contexts'] < 1:
            errors.append('OpenGL reference did not create an OpenGL context')
        if lifecycle and (contexts['gl_contexts'] < 1 or contexts['vulkan_surfaces'] < 3):
            errors.append('5 -> 0 -> 5 -> 3 -> 5 did not recreate all native contexts')
        if switching and not lifecycle and (contexts['gl_contexts'] < 2 or contexts['vulkan_surfaces'] < 1):
            errors.append('3 -> 5 -> 3 did not recreate both native contexts')
    recording_stats, recording_errors = recording_evidence(
        log, 5 in sequence, requested_workers, require_parallel)
    errors.extend(recording_errors)
    submission_stats, submission_errors = submission_evidence(
        log, 5 in sequence, requested_frames, require_inflight)
    errors.extend(submission_errors)
    if submission_stats and contexts and len(submission_stats) != contexts['vulkan_surfaces']:
        errors.append('Vulkan submission lifetime count does not match the created native surfaces')
    screenshots = sorted(runtime.rglob('blud*.png'))
    if len(screenshots) != expected_screenshots:
        errors.append(f'expected {expected_screenshots} screenshots, found {len(screenshots)}')
    for screenshot in screenshots:
        try:
            inspect_image(screenshot, (args.width, args.height))
        except (OSError, ValueError) as error:
            errors.append(str(error))
    saved = (runtime / 'render-test.cfg').read_text(errors='replace')
    expected_final = sequence[-1]
    if not re.search(rf'^Renderer\s*=\s*{expected_final}\s*$', saved, re.M):
        errors.append('saved renderer does not match the requested final renderer')
    if not re.search(r'^ScreenBPP\s*=\s*32\s*$', saved, re.M):
        errors.append('renderer fell back from 32-bit video')
    result = {'map': map_name, 'renderer': renderer, 'switching': switching,
              'renderer_sequence': sequence, 'lifecycle': lifecycle,
              'final_capture_only': lifecycle,
              'vulkan_workers_requested': requested_workers,
              'vulkan_workers_environment': env.get('NBLOOD_VULKAN_WORKERS'),
              'parallel_required': require_parallel and 5 in sequence,
              'vulkan_recording': recording_stats,
              'frames_in_flight_requested': requested_frames,
              'frames_in_flight_environment': env.get('NBLOOD_VULKAN_FRAMES_IN_FLIGHT'),
              'inflight_required': require_inflight and 5 in sequence,
              'vulkan_submission': submission_stats,
              'opengl_disabled': args.disable_opengl, 'expected_gl_loading_failures': missing_gl_lines,
              'features': args.manifest.get('features', {}).get(map_name, []),
              'command': command, 'exit_code': exit_code, 'contexts': contexts,
              'screenshots': [str(path) for path in screenshots], 'log': str(log_path),
              'passed': not errors, 'errors': errors}
    (runtime / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{"PASS" if result["passed"] else "FAIL"}: {runtime.name}', flush=True)
    return result


def check_feature_effects(args, output, cases):
    """Reject invisible fixtures even when both renderers omit the same feature."""
    checks, parity = [], []
    images = {(case['map'], case['renderer']): case['screenshots'][0]
              for case in cases if case['passed'] and not case['switching']}
    for effect in args.manifest.get('effect_checks', []):
        masks = []
        for renderer in (3, 5):
            baseline = images.get((effect['baseline'], renderer))
            modified = images.get((effect['modified'], renderer))
            if not baseline or not modified:
                continue
            path = output / f"effect-{effect['modified']}-{renderer}.png"
            comparison = compare_images(baseline, modified, path, args.max_mean_error)
            comparison.pop('threshold')
            comparison.pop('max_changed_fraction')
            with Image.open(path) as difference:
                masks.append(difference_mask(difference.convert('RGB')))
            minimum = effect['minimum_mean_error']
            comparison.update(feature=effect['feature'], renderer=renderer,
                              minimum_mean_error=minimum,
                              passed=comparison['mean_rgb_error'] >= minimum)
            checks.append(comparison)
        if len(masks) == 2:
            # Restrict this comparison to pixels affected by the fixture in at
            # least one renderer, preventing a large identical room from hiding
            # a missing or incorrectly rendered small object.
            mask = ImageChops.lighter(*masks)
            if mask.getbbox():
                comparison = compare_images(images[(effect['modified'], 3)], images[(effect['modified'], 5)],
                    output / f"feature-parity-{effect['modified']}.png", args.max_mean_error,
                    args.max_changed_fraction, mask)
                comparison.update(feature=effect['feature'], region='union of visible fixture changes')
                parity.append(comparison)
    return checks, parity


def main():
    args = arguments()
    if args.disable_opengl and args.mode != 'vulkan':
        raise SystemExit('--disable-opengl requires --mode vulkan.')
    if not os.environ.get('DISPLAY'):
        raise SystemExit('DISPLAY is required; use an existing X11 server or an external Xvfb wrapper.')
    args.binary = args.binary.resolve()
    args.assets = args.assets.resolve()
    args.engine_data = (args.engine_data or args.binary.parent).resolve()
    args.manifest = {}
    if args.fixtures:
        args.fixtures = args.fixtures.resolve()
        args.manifest = json.loads((args.fixtures / 'fixtures.json').read_text())
        args.search_path.append(args.fixtures)
        args.defs = args.defs or args.fixtures / args.manifest['defs']
        args.maps = args.maps or args.manifest['maps']
    args.search_path = [path.resolve() for path in args.search_path]
    args.defs = args.defs.resolve() if args.defs else None
    if not args.binary.is_file() or not args.assets.is_dir():
        raise SystemExit('The binary and assets directory must exist.')
    if any(not path.is_dir() for path in args.search_path) or (args.defs and not args.defs.is_file()):
        raise SystemExit('Definitions and additional search directories must exist.')
    args.maps = args.maps or ['E1M1']
    if len(set(args.maps)) != len(args.maps) or any(re.search(r'["\r\n;]', name) for name in args.maps):
        raise SystemExit('Map names must be unique and cannot contain quotes, semicolons, or line breaks.')
    if args.width < 320 or args.height < 200 or args.initial_delay_ms < 1 or args.interval_ms < 500:
        raise SystemExit('Invalid dimensions or input timing.')
    if args.max_mean_error < 0 or not 0 <= args.max_changed_fraction <= 1:
        raise SystemExit('Invalid image comparison tolerance.')
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='nblood-vulkan-test-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    print(f'Evidence: {output}', flush=True)
    # A concurrent developer rebuild must not mix executable versions between
    # cases. Keep the exact tested binary with the evidence.
    binary_source = args.binary
    args.binary = output / 'nblood-test'
    shutil.copy2(binary_source, args.binary)
    binary_sha256 = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    library = build_input_driver(output)
    cases, comparisons = [], []
    for map_name in args.maps:
        if args.mode == 'opengl':
            cases.append(run_case(args, output, library, map_name, 3))
        if args.mode in ('vulkan', 'compare', 'all'):
            reference = None
            if args.mode in ('compare', 'all'):
                reference = run_case(args, output, library, map_name, 3)
                cases.append(reference)
            native_case = run_case(args, output, library, map_name, 5)
            cases.append(native_case)
            if reference and reference['passed'] and native_case['passed']:
                comparisons.append(compare_images(reference['screenshots'][0], native_case['screenshots'][0],
                    Path(native_case['log']).parent / 'difference-from-gl.png', args.max_mean_error,
                    args.max_changed_fraction))
        if args.mode in ('switch', 'all'):
            switched = run_case(args, output, library, map_name, 3, switching=True)
            cases.append(switched)
            if switched['passed']:
                before, native, after = switched['screenshots']
                directory = Path(switched['log']).parent
                comparisons += [compare_images(before, native, directory / 'difference-switch-vulkan.png',
                                               args.max_mean_error, args.max_changed_fraction),
                                compare_images(before, after, directory / 'difference-switch-back.png',
                                               args.max_mean_error, args.max_changed_fraction)]
        if args.mode in ('bridge', 'all'):
            bridged = run_case(args, output, library, map_name, 3, switching=True, bridge=True)
            cases.append(bridged)
            if bridged['passed']:
                before, software, native, software_after, after = bridged['screenshots']
                directory = Path(bridged['log']).parent
                for first, second, name in ((before, native, 'vulkan'), (before, after, 'gl-restored'),
                                            (software, software_after, 'software-restored')):
                    comparisons.append(compare_images(first, second, directory / f'difference-bridge-{name}.png',
                        args.max_mean_error, args.max_changed_fraction))
        if args.mode in ('lifecycle', 'all'):
            lifecycle = run_case(args, output, library, map_name, 5, switching=True, lifecycle=True)
            cases.append(lifecycle)
            if args.mode == 'all' and native_case['passed'] and lifecycle['passed']:
                comparisons.append(compare_images(native_case['screenshots'][0], lifecycle['screenshots'][0],
                    Path(lifecycle['log']).parent / 'difference-lifecycle-restored.png',
                    args.max_mean_error, args.max_changed_fraction))
    effects, feature_parity = check_feature_effects(args, output, cases)
    comparisons += feature_parity
    passed = (all(case['passed'] for case in cases) and all(item['passed'] for item in comparisons)
              and all(item['passed'] for item in effects))
    result = {'passed': passed, 'binary_source': str(binary_source), 'binary_sha256': binary_sha256,
              'vulkan_workers_requested': args.vulkan_workers, 'parallel_required': args.require_parallel,
              'frames_in_flight_requested': args.frames_in_flight, 'inflight_required': args.require_inflight,
              'cases': cases, 'comparisons': comparisons,
              'feature_effects': effects,
              'scope': 'Paused supplied maps only; this does not establish full renderer parity or demo determinism.'}
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{"PASS" if passed else "FAIL"}: {output / "results.json"}')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
