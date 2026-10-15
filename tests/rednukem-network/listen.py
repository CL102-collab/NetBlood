#!/usr/bin/env python3
"""Real listen host, repeated reconnects, slot15 gameplay and host packet demo."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from run import Game, Failure, ROOT, build_driver, generate, inspect_demo, poll_status, records, require


def position(game, slot):
    game.status()
    return [row for row in records(game.text(), 'NET player ') if row['slot'] == str(slot)][-1]


def move(game, slot):
    before = position(game, slot)
    game.key(26, True)
    time.sleep(1)
    game.key(26, False)
    game.input_barrier()
    time.sleep(0.5)
    after = position(game, slot)
    require(any(before[key] != after[key] for key in ('x', 'y', 'z')), game.name + ': player did not move')
    return {'before': before, 'after': after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    args.assets = args.assets.resolve()
    args.grp = next(path for path in args.assets.iterdir() if path.name.lower() == 'duke3d.grp')
    args.renderer = 3
    require(os.environ.get('DISPLAY'), 'listen host requires DISPLAY')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-listen-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source = args.binary.resolve()
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source, args.binary)
    generate(root / 'fixture')
    library = build_driver(root)
    probe = root / 'slot-probe'
    command = shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++11', '-O1',
        '-I' + str(ROOT / 'source/build/include'), '-I' + str(ROOT / 'source/blood/src'),
        str(Path(__file__).with_name('slot-probe.cpp')), '-pthread', '-o', str(probe)]
    command += shlex.split(os.environ.get('LDFLAGS', ''))
    subprocess.run(command, check=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as available:
        available.bind(('127.0.0.1', 0)); port = available.getsockname()[1]
    games, holders, holder_log = [], None, None
    result = {'passed': False, 'failures': [], 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    try:
        host = Game('listen-host', args, root, library,
                    launch_arguments=['-server', '16', '-port', str(port), '-v1', '-l1', '-s1'])
        games.append(host)
        host.wait(lambda text: 'NET server role=listen' in text, 'listen startup', args.timeout)
        poll_status(host, lambda row: row['players'] == '1' and int(row['tick']) >= 30,
                    'host alone advances', args.timeout)
        result['host_movement'] = move(host, 0)
        host.command_text('net_record host.rnpd')
        client = Game('reconnecting-client', args, root, library, port)
        games.append(client)
        client.wait(lambda text: 'NET content ready ' in text, 'late client content', args.timeout)
        poll_status(client, lambda row: row['state'] == 'playing', 'late client snapshot', args.timeout)
        result['client_movement'] = move(client, 1)
        host.command_text('hud_showmapname 0\ncl_viewbob 0\ncl_weaponsway 0\nscreenshot')
        before_image = sorted((host.path / 'screenshots').glob('duke*.png'))[-1]
        result['host_movement_with_peer'] = move(host, 0)
        host.command_text('screenshot')
        after_image = sorted((host.path / 'screenshots').glob('duke*.png'))[-1]
        from PIL import Image, ImageChops, ImageStat
        before_pixels = Image.open(before_image).convert('RGB').crop((0, 100, 640, 300))
        after_pixels = Image.open(after_image).convert('RGB').crop((0, 100, 640, 300))
        mean_change = sum(ImageStat.Stat(ImageChops.difference(before_pixels, after_pixels)).mean) / 3
        result['host_camera_motion'] = {'mean_rgb_change': mean_change,
                                        'before': str(before_image), 'after': str(after_image)}
        require(mean_change > 0.01, 'listen host camera remained frozen while its authoritative player moved')
        rounds = []
        for index in range(3):
            client.command_text('disconnect' if index == 0 else 'net_disconnect')
            require(client.status()['role'] == 'offline', 'client did not disconnect')
            poll_status(host, lambda row: row['players'] == '1', 'host alone after disconnect', args.timeout)
            host.command_text('map ' + ('RNET2.MAP' if index % 2 == 0 else 'RNET1.MAP'))
            expected = int(host.status()['map_generation'])
            client.command_text(f'connect 127.0.0.1:{port}')
            state = poll_status(client, lambda row: row['role'] == 'client' and row['state'] == 'playing' and
                                int(row['map_generation']) == expected, 'same-process reconnect', args.timeout)
            rounds.append(state)
        result['reconnects'] = rounds
        client.stop()
        poll_status(host, lambda row: row['players'] == '1', 'client gone before slot15 test', args.timeout)
        holder_log = (root / 'holders.log').open('w')
        holders = subprocess.Popen([str(probe), str(port), '14'], stdin=subprocess.PIPE,
                                   stdout=holder_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.timeout
        while 'READY holders=14' not in (root / 'holders.log').read_text():
            require(holders.poll() is None and time.monotonic() < deadline, 'holding peers did not authenticate')
            time.sleep(0.1)
        top = Game('client-slot15', args, root, library, port)
        games.append(top)
        top.wait(lambda text: 'NET content ready ' in text, 'slot15 content', args.timeout)
        state = poll_status(top, lambda row: row['state'] == 'playing', 'slot15 snapshot', args.timeout)
        require(state['local'] == '15', 'upper-slot client was not assigned player15')
        require(host.status()['players'] == '16', 'sixteen connected players not reported')
        result['slot15_movement'] = move(top, 15)
        top.stop()
        holders.stdin.write(b'quit\n'); holders.stdin.flush(); holders.wait(timeout=5)
        require(holders.returncode == 0, 'holding peers failed')
        poll_status(host, lambda row: row['players'] == '1', 'host remains after all remote peers leave', args.timeout)
        host.command_text('net_stoprecord')
        demo = inspect_demo(host.path / 'host.rnpd')
        result['demo'] = demo
        host.command_text(f'net_disconnect\nnet_playback "{demo["path"]}"')
        last = demo['last_snapshot']
        host.wait(lambda text: 'NET playback finished ' in text, 'host packet recording playback',
                  args.timeout + last['elapsed_ticks'] / demo['tick_rate'])
        host.status()
        actual = records(host.text(), 'NET snapshot ')[-1]
        require(int(actual['tick']) == last['tick'] and int(actual['sequence']) == last['sequence'] and
                actual['hash'] == last['hash'], 'host demo playback differs from recorded final state')
        result['playback'] = actual
        completions = host.text().count('NET playback finished ')
        host.command_text(f'net_disconnect\nnet_playback "{demo["path"]}"')
        host.wait(lambda text: text.count('NET playback finished ') > completions,
                  'second playback completion', args.timeout + last['elapsed_ticks'] / demo['tick_rate'])
        host.status()
        repeated = records(host.text(), 'NET snapshot ')[-1]
        require(repeated == actual, 'repeated host demo playback differs from its first playback')
        result['repeated_playback'] = repeated
        host.stop()
        for game in games:
            require(game.process.returncode == 0 and not game.forced, game.name + ': unclean shutdown')
        result['passed'] = True
    except (Failure, OSError, KeyError, IndexError, subprocess.SubprocessError) as error:
        result['failures'].append(str(error))
    finally:
        if holders is not None and holders.poll() is None:
            holders.stdin.write(b'quit\n'); holders.stdin.flush()
            try: holders.wait(timeout=5)
            except subprocess.TimeoutExpired: holders.kill(); holders.wait()
        if holder_log is not None:
            holder_log.close()
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
