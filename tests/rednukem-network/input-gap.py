#!/usr/bin/env python3
"""Recover from over 1024 lost input sequences without disconnecting the peer."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
import time

from run import Game, Failure, ROOT, build_driver, generate, poll_status, records, require
from relay import Relay


class InputRelay(Relay):
    """Drop client datagrams carrying input; pass other traffic unchanged."""
    def __init__(self, target):
        super().__init__(target)
        self.block_inputs = False
        self.latest_input = 0

    def _receive(self, stream, direction):
        relay = self

        class Filter:
            def recvfrom(self, limit):
                while True:
                    data, source = stream.recvfrom(limit)
                    offset, has_input = 0, False
                    while True:
                        offset = data.find(b'RNM1', offset)
                        if offset < 0:
                            break
                        if (offset + 22 <= len(data) and data[offset + 16] == 6 and
                                struct.unpack_from('<I', data, offset + 4)[0] == 1):
                            count = data[offset + 17]
                            if 1 <= count <= 8 and offset + 18 + 21 * count <= len(data):
                                has_input = True
                                for index in range(count):
                                    sequence = struct.unpack_from('<I', data, offset + 18 + 21 * index)[0]
                                    relay.latest_input = max(relay.latest_input, sequence)
                        offset += 4
                    if relay.block_inputs and has_input:
                        relay.counters['input_datagrams_filtered'] += 1
                        continue
                    return data, source

        super()._receive(Filter() if direction == 'client_to_server' else stream, direction)


def player(game):
    game.status()
    return records(game.text(), 'NET player ')[-1]


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
    require(os.environ.get('DISPLAY'), 'graphical client requires DISPLAY')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-input-gap-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source = args.binary.resolve()
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source, args.binary)
    generate(root / 'fixture')
    library = build_driver(root)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as available:
        available.bind(('127.0.0.1', 0)); port = available.getsockname()[1]
    games, relay = [], None
    result = {'passed': False, 'failures': [], 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    try:
        server = Game('server', args, root, library, port, dedicated=True)
        games.append(server)
        server.wait(lambda text: 'NET server role=dedicated' in text, 'server startup', args.timeout)
        relay = InputRelay(('127.0.0.1', port)).start()
        client = Game('client', args, root, library, relay.port)
        games.append(client)
        poll_status(client, lambda row: row['state'] == 'playing', 'client join', args.timeout)
        time.sleep(1)
        before = player(server)
        sequence_before = relay.latest_input
        require(int(before['ack']) > 0 and sequence_before > 0, 'initial input stream did not reach server')
        relay.block_inputs = True
        started = time.monotonic()
        # Real 30 Hz inputs must span the old 1024-command acceptance ceiling.
        while relay.latest_input - sequence_before < 1100:
            require(client.process.poll() is None and server.process.poll() is None, 'process exited during outage')
            require(time.monotonic() - started < args.timeout, 'client did not produce enough input during outage')
            time.sleep(0.2)
        during = player(server)
        client_during = client.status()
        require(client_during['role'] == 'client' and client_during['state'] == 'playing',
                'input-only outage disconnected the client')
        require(int(during['ack']) <= sequence_before + 64, 'input filter did not stop applied commands')
        result['outage'] = {'before': before, 'during': during, 'client': client_during,
                            'first_sequence': sequence_before, 'last_sequence': relay.latest_input,
                            'elapsed_seconds': time.monotonic() - started}
        relay.block_inputs = False
        deadline = time.monotonic() + 10
        after = during
        while time.monotonic() < deadline and int(after['ack']) <= sequence_before + 1024:
            time.sleep(0.2)
            after = player(server)
        require(int(after['ack']) > sequence_before + 1024, 'server never recovered from the forward input gap')
        client.key(26, True)
        time.sleep(1)
        client.key(26, False)
        client.input_barrier()
        time.sleep(0.5)
        moved = player(server)
        require(any(moved[key] != during[key] for key in ('x', 'y', 'z')), 'movement did not recover after input outage')
        result['recovered'] = {'first_applied': after, 'movement': moved}
        client.stop()
        server.stop()
        for game in games:
            require(game.process.returncode == 0 and not game.forced, game.name + ': unclean shutdown')
        result['passed'] = True
    except (Failure, OSError, KeyError, IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        if relay:
            relay.close()
            result['relay'] = relay.report()
        result['games'] = {game.name: game.result() for game in games}
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(('PASS' if result['passed'] else 'FAIL') + f': {root / "result.json"}', flush=True)
    for error in result['failures']:
        print(error, file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
