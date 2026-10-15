#!/usr/bin/env python3
"""Keep ordinary offline restartmap/changelevel behavior with network commands installed."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

from run import Game, Failure, ROOT, build_driver, generate, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args()
    args.assets = args.assets.resolve()
    args.grp = next(path for path in args.assets.iterdir() if path.name.lower() == 'duke3d.grp')
    args.renderer = 3
    require(os.environ.get('DISPLAY'), 'offline smoke requires DISPLAY')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-offline-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source = args.binary.resolve()
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source, args.binary)
    generate(root / 'fixture')
    library = build_driver(root)
    result = {'passed': False, 'failures': [], 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    game = None
    try:
        game = Game('offline', args, root, library, playback=True, network_options=False)
        game.wait(lambda text: 'E1L1: NETWORK ROOM ONE' in text, 'offline first level', args.timeout)
        for command, marker in (('restartmap', 'E1L1: NETWORK ROOM ONE'),
                                ('changelevel 1 2', 'E1L2: NETWORK ROOM TWO'),
                                ('restartmap', 'E1L2: NETWORK ROOM TWO'),
                                ('changelevel 1 1', 'E1L1: NETWORK ROOM ONE')):
            before = game.text().count(marker)
            game.command_text(command)
            game.wait(lambda text: text.count(marker) > before, command + ': ' + marker, args.timeout)
        require('Only the server' not in game.text() and 'NET server role=' not in game.text(),
                'offline commands were routed into online authority')
        result['level_entries'] = {'first': game.text().count('E1L1: NETWORK ROOM ONE'),
                                   'second': game.text().count('E1L2: NETWORK ROOM TWO')}
        game.stop()
        require(game.process.returncode == 0 and not game.forced, 'offline process did not exit cleanly')
        result['passed'] = True
    except (Failure, OSError) as error:
        result['failures'].append(str(error))
    finally:
        if game is not None:
            game.stop()
            result['game'] = game.result()
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(('PASS' if result['passed'] else 'FAIL') + f': {root / "result.json"}', flush=True)
    for error in result['failures']:
        print(error, file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
