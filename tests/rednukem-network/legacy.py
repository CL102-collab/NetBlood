#!/usr/bin/env python3
"""Record legacy input on a pre-port binary and replay it with seed checking."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

from run import Game, Failure, ROOT, build_driver, generate, records, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-binary', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    args.assets = args.assets.resolve()
    args.grp = next(path for path in args.assets.iterdir() if path.name.lower() == 'duke3d.grp')
    args.renderer = 3
    require(os.environ.get('DISPLAY'), 'legacy replay requires DISPLAY')
    require(args.baseline_binary.is_file() and args.binary.is_file(), 'supply both built binaries')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-legacy-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    binaries = {}
    hashes = {}
    for name, source in (('baseline', args.baseline_binary), ('candidate', args.binary)):
        binaries[name] = root / ('rednukem-' + name)
        shutil.copy2(source.resolve(), binaries[name])
        hashes[name] = hashlib.sha256(binaries[name].read_bytes()).hexdigest()
    generate(root / 'fixture')
    library = build_driver(root)
    games, result = [], {'passed': False, 'failures': [], 'binary_sha256': hashes}
    try:
        args.binary = binaries['baseline']
        recording = Game('record-baseline', args, root, library, playback=True, network_options=False,
                         extra_arguments=['-r'], autoexec_extra='demorec_diffs 0\ndemorec_seeds 1\n')
        games.append(recording)
        recording.wait(lambda text: 'E1L1: NETWORK ROOM ONE' in text, 'baseline recording map', args.timeout)
        recording.key(26, True)
        time.sleep(2)
        recording.key(26, False)
        recording.input_barrier()
        time.sleep(1)
        # Scroll Lock is the original in-game stop-recording control.
        recording.key(71, True)
        time.sleep(0.2)
        recording.key(71, False)
        recording.input_barrier()
        recording.command_text('echo LEGACY_RECORDING_COMPLETE')
        recording.stop()
        demos = list(recording.path.glob('*.edm'))
        require(len(demos) == 1 and demos[0].stat().st_size > 1024, 'baseline did not write one legacy EDM')
        demo = demos[0]
        result['demo'] = {'path': str(demo), 'bytes': demo.stat().st_size,
                          'sha256': hashlib.sha256(demo.read_bytes()).hexdigest()}
        counts = {}
        for name in ('baseline', 'candidate'):
            args.binary = binaries[name]
            replay = Game('replay-' + name, args, root, library, network_options=False,
                          extra_arguments=['-j', str(demo.parent)],
                          launch_arguments=['-d', f'{demo.name}:0'], autoexec_extra='demoplay_diffs 0\n')
            games.append(replay)
            replay.wait(lambda text: bool(re.search(r'== demo \d+: \d+ gametics', text)),
                        name + ' complete legacy replay', args.timeout)
            counts[name] = int(re.findall(r'== demo \d+: (\d+) gametics', replay.text())[-1])
            if name == 'candidate':
                rows = records(replay.text(), 'DEMO sync ')
                require(rows, 'candidate lacks legacy profiling seed diagnostics')
                result['sync'] = rows[-1]
                require(int(rows[-1]['seeds_checked']) >= 60 and rows[-1]['mismatches'] == '0' and
                        rows[-1]['diffs'] == '0', 'legacy lockstep replay diverged or used state diffs')
            replay.stop()
            require(not re.search(r'is corrupt|errors opening demo|INCOMPATIBLE|version mismatch', replay.text(), re.I),
                    name + ': legacy demo rejected or corrupt')
        require(counts['baseline'] == counts['candidate'] and counts['candidate'] >= 60,
                'baseline and candidate replayed different input-tick counts')
        result['gametics'] = counts
        for game in games:
            require(game.process.returncode == 0 and not game.forced, game.name + ': unclean shutdown')
        result['passed'] = True
    except (Failure, OSError, KeyError, IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        result['games'] = {game.name: game.result() for game in games}
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(('PASS' if result['passed'] else 'FAIL') + f': {root / "result.json"}', flush=True)
    for error in result['failures']:
        print(error, file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
