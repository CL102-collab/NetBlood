#!/usr/bin/env python3
"""Exercise real Rednukem dedicated service, hot joins, movement and packet demos.

Linux/SDL2: supply Duke assets (1.3D shareware works) and an existing X11 DISPLAY.
The test generates its own two maps and CON; it never downloads game assets.
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
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from fixtures import generate

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests/network'))
spec = importlib.util.spec_from_file_location('blood_network_checks', ROOT / 'tests/network/run.py')
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
Failure, require, records = shared.Failure, shared.require, shared.records


def build_driver(output):
    """Add ordinary SDL text events to the shared key/device auditing driver."""
    source = (ROOT / 'tests/network/input-driver.c').read_text()
    marker = '        if (!valid || scan < 0 || scan >= SDL_NUM_SCANCODES'
    require(marker in source, 'shared input driver changed; update text event adapter')
    insertion = '''        if (valid && scan == -4 && down > 0) {
            char text_path[64], value[32];
            snprintf(text_path, sizeof(text_path), "text-%d.txt", down);
            FILE *input = fopen(text_path, "rb");
            if (!input) continue;
            size_t bytes = fread(value, 1, sizeof(value), input);
            fclose(input);
            if (!bytes || bytes >= sizeof(value)) continue;
            memset(event, 0, sizeof(*event));
            event->type = SDL_TEXTINPUT;
            memcpy(event->text.text, value, bytes);
            ++injected_events;
            return 1;
        }
        if (valid && scan == -3 && down > 0 && down < 128) {
            memset(event, 0, sizeof(*event));
            event->type = SDL_TEXTINPUT;
            event->text.text[0] = (char)down;
            event->text.text[1] = 0;
            ++injected_events;
            return 1;
        }
        if (valid && scan == -2 && down > 0) {
            memset(event, 0, sizeof(*event));
            event->type = SDL_TEXTINPUT;
            snprintf(event->text.text, sizeof(event->text.text), "exec step-%d.cfg", down);
            ++injected_events;
            return 1;
        }
'''
    source = source.replace(marker, insertion + marker)
    path = output / 'input-driver.c'
    path.write_text(source)
    library = output / 'input-driver.so'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-Wall', '-Wextra', '-Werror', '-shared', '-fPIC']
    command += shlex.split(os.environ.get('CFLAGS', '')) + flags + [str(path), '-o', str(library), '-ldl']
    subprocess.run(command, check=True)
    return library


class Game(shared.Game):
    """Reuse ordinary SDL FIFO events and device audit, without changing game state."""
    def __init__(self, name, args, root, library, port=None, dedicated=False, playback=False,
                 capacity=8, password='fixture-password', extra_arguments=(),
                 network_options=True, autoexec_extra='', launch_arguments=None, audio=False):
        self.name = name
        self.path = root / name
        self.path.mkdir()
        self.fifo = self.path / 'input.fifo'
        os.mkfifo(self.fifo)
        self.events = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        self.command_serial = self.input_serial = self.text_serial = 0
        self.dedicated, self.forced = dedicated, False
        (self.path / 'network.cfg').write_text(
            '[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n[Screen Setup]\n'
            f'Renderer = {args.renderer}\nScreenMode = 0\nScreenBPP = 32\n'
            'ScreenWidth = 640\nScreenHeight = 480\n')
        (self.path / 'autoexec.cfg').write_text(
            'r_maxfps 60\n' + ('cl_lagometer 1\n' if network_options else '') +
            'bind I norepeat "exec command.cfg"\n'
            'bind W "gamefunc_Move_Forward"\n' + autoexec_extra)
        environment = os.environ.copy()
        environment.update(SDL_AUDIODRIVER='dummy', NETWORK_TEST_EVENTS=str(self.fifo),
                           LD_PRELOAD=str(library) + (':' + environment['LD_PRELOAD']
                                                      if environment.get('LD_PRELOAD') else ''))
        if dedicated:
            for variable in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY'):
                environment.pop(variable, None)
            environment.update(SDL_VIDEODRIVER='network-test-invalid-video-driver',
                               SDL_AUDIODRIVER='network-test-invalid-audio-driver', NETWORK_TEST_HEADLESS='1')
        command = [str(args.binary), '-usecwd', '-nosetup', '-noautoload', '-nologo', '-nm',
                   '-cfg', 'network.cfg', '-j', str(args.assets), '-j', str(args.grp.parent),
                   '-j', str(root / 'fixture'),
                   '-gamegrp', 'fixture.grp', '-g', args.grp.name, '-x', 'fixture.con']
        if not audio:
            command += ['-ns']
        if network_options:
            command += ['-stunserver', 'off', '-password', password]
        command += list(extra_arguments)
        if launch_arguments is not None:
            command += list(launch_arguments)
        elif dedicated:
            command += ['-dedicated', '-server', str(capacity), '-port', str(port), '-v1', '-l1', '-s1']
        elif playback:
            command += ['-v1', '-l1', '-s1']
        else:
            command += ['-client', '127.0.0.1', '-port', str(port)]
        self.command = command
        self.log = (self.path / 'console.log').open('w')
        self.process = subprocess.Popen(command, cwd=self.path, env=environment,
                                        stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT)

    def text(self):
        path = self.path / 'rednukem.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def status(self):
        self.command_text('net_status')
        rows = records(self.text(), 'NET status ')
        require(rows, f'{self.name}: net_status produced no status')
        return rows[-1]

    def command_text(self, commands, timeout=10):
        self.command_serial += 1
        marker = f'NETWORK_TEST_COMMAND_{self.command_serial}'
        script = commands + f'\necho {marker}\n'
        if self.dedicated:
            self.process.stdin.write(script.encode())
            self.process.stdin.flush()
        else:
            (self.path / f'step-{self.command_serial}.cfg').write_text(script)
            # Console text works in gameplay, menus and playback. Opening and
            # closing within one event batch also leaves captures unobscured.
            os.write(self.events, (f'53 1\n53 0\n-2 {self.command_serial}\n'
                                   '40 1\n40 0\n53 1\n53 0\n').encode())
        self.wait(lambda text: marker in text, marker, timeout)
        if not self.dedicated:
            self.input_barrier(timeout)

    def enter_text(self, value, timeout=10):
        """Send ordinary UTF-8 text events without console commands or log echo."""
        require(not self.dedicated, 'text entry requires a graphical client')
        chunks, chunk = [], b''
        for character in value:
            encoded = character.encode('utf-8')
            if len(chunk) + len(encoded) > 31:
                chunks.append(chunk)
                chunk = b''
            chunk += encoded
        if chunk:
            chunks.append(chunk)
        for chunk in chunks:
            self.text_serial += 1
            path = self.path / f'text-{self.text_serial}.txt'
            path.write_bytes(chunk)
            try:
                os.write(self.events, f'-4 {self.text_serial}\n'.encode())
                self.input_barrier(timeout)
            finally:
                path.unlink()

    def stop(self):
        if self.process.poll() is None:
            try:
                if self.dedicated:
                    self.process.stdin.write(b'quit\n')
                    self.process.stdin.flush()
                else:
                    self.command_text('net_stoprecord\nnet_disconnect\nquit', timeout=2)
            except (BrokenPipeError, Failure):
                pass
            # Duke's normal exit screens await a key. Dismiss them through SDL
            # rather than treating a live exit screen as a transport deadlock.
            deadline = time.monotonic() + 5
            while self.process.poll() is None and time.monotonic() < deadline:
                if not self.dedicated:
                    self.key(44, True)
                    time.sleep(0.05)
                    self.key(44, False)
                time.sleep(0.1)
            if self.process.poll() is None:
                self.forced = True
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
        self.log.close()
        if self.events is not None:
            os.close(self.events)
            self.events = None

    def result(self):
        return {'command': self.command, 'exit_code': self.process.poll(), 'forced_stop': self.forced,
                'log': str(self.path / 'rednukem.log'), 'status': records(self.text(), 'NET status '),
                'snapshots': records(self.text(), 'NET snapshot '), 'players': records(self.text(), 'NET player ')}


def poll_status(game, predicate, description, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        row = game.status()
        if predicate(row):
            return row
        time.sleep(0.2)
    raise Failure(f'{game.name}: timed out waiting for {description}')


def check_external_arguments(binary, root, library):
    """Reject malformed links before graphics, game assets, or extra arguments."""
    environment = os.environ.copy()
    for variable in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY'):
        environment.pop(variable, None)
    environment.update(SDL_VIDEODRIVER='network-test-invalid-video-driver',
                       SDL_AUDIODRIVER='network-test-invalid-audio-driver', NETWORK_TEST_HEADLESS='1',
                       LD_PRELOAD=str(library))
    directory = root / 'external-arguments'
    directory.mkdir()
    cases = (
        ('extra-options', ['-connect-uri', 'rednukem://127.0.0.1:23513', '-x', 'untrusted.con']),
        ('embedded-options', ['-connect-uri', 'rednukem://127.0.0.1 -x untrusted.con']),
        ('quoted-options', ['-connect-uri', 'rednukem://127.0.0.1" -x "untrusted.con']),
        ('wrong-scheme', ['-connect-uri', 'nblood://127.0.0.1:23513']),
        ('invalid-port', ['-connect-uri', 'rednukem://127.0.0.1:65536']),
        ('missing-uri', ['-connect-uri']),
    )
    results = []
    for name, arguments in cases:
        completed = subprocess.run([str(binary)] + arguments, cwd=directory, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        text = completed.stdout.decode(errors='replace')
        (directory / f'{name}.log').write_text(text)
        require(completed.returncode == 1, f'URI case {name}: expected exit 1, got {completed.returncode}')
        require('NETWORK_TEST_DEVICES windows=0 audio=0' in text and
                'Initializing SDL' not in text and 'Compiling' not in text,
                f'URI case {name}: startup progressed beyond link validation')
        results.append({'case': name, 'arguments': arguments, 'exit_code': completed.returncode})
    return results


def inspect_demo(path):
    data = path.read_bytes()
    require(len(data) >= 20 and data[:8] == b'RNPKDEM\x1a', 'packet demo header missing')
    version, size, checksum = struct.unpack_from('<III', data, 8)
    require(version == 1 and 29 <= size <= 1052, 'invalid packet demo metadata bounds')
    metadata = data[20:20 + size]
    require(zlib.crc32(metadata) == checksum, 'packet demo metadata checksum mismatch')
    protocol, rate, map_crc, content_crc, slot, name_size, reserved = struct.unpack_from('<7I', metadata)
    require(protocol == 1 and rate == 30 and slot < 16 and not reserved and name_size + 28 == size,
            'invalid packet demo metadata')
    offset, elapsed_before, tick_before = 20 + size, None, None
    packets = []
    while offset < len(data):
        require(offset + 24 <= len(data), 'truncated packet demo record')
        marker, length, elapsed, tick, crc = struct.unpack_from('<IIQII', data, offset)
        header = data[offset:offset + 20]
        offset += 24
        require(length <= 16 * 1024 * 1024 + 4096 and offset + length <= len(data), 'invalid demo payload bounds')
        body = data[offset:offset + length]
        offset += length
        require(zlib.crc32(body, zlib.crc32(header)) == crc, 'packet demo record checksum mismatch')
        if elapsed_before is None:
            require(elapsed == 0, 'packet demo timeline must start at zero')
        else:
            delta = 0 if marker == 0x31545352 else (tick - tick_before) & 0xffffffff
            require(delta <= 0x7fffffff and elapsed == elapsed_before + delta, 'invalid demo timeline')
        elapsed_before, tick_before = elapsed, tick
        if marker == 0x31444e45:
            require(length == 0 and offset == len(data), 'invalid demo end marker or trailing data')
            break
        require(marker in (0x31544b50, 0x31545352) and length >= 17, 'invalid demo record type')
        magic, wire_protocol, generation, wire_tick, kind = struct.unpack_from('<4IB', body)
        require(magic == 0x314d4e52 and wire_protocol == protocol and wire_tick == tick,
                'invalid Rednukem envelope in demo')
        record = {'type': kind, 'generation': generation, 'tick': tick, 'elapsed_ticks': elapsed,
                  'sha256': hashlib.sha256(body).hexdigest()}
        if kind == 7:
            require(length >= 57, 'truncated recorded snapshot')
            sequence, baseline, ack = struct.unpack_from('<3I', body, 17)
            codec_magic, codec_version, flags, raw_size, base_size, state_hash, base_hash = struct.unpack_from('<7I', body, 29)
            require(codec_magic == 0x315a534e and codec_version == 1 and not flags & 1 and
                    not baseline and not base_size and not base_hash,
                    'recorded snapshot depends on live network state')
            require(0 < raw_size <= 16 * 1024 * 1024, 'invalid recorded snapshot size')
            record.update(sequence=sequence, ack=ack, hash=f'{state_hash:08x}')
        packets.append(record)
    else:
        raise Failure('packet demo has no end marker')
    require(packets and packets[0]['type'] == 4, 'packet demo must begin with Welcome')
    snapshots = [row for row in packets if row['type'] == 7]
    require(len(snapshots) >= 10, 'packet demo contains too few authoritative snapshots')
    require(len({row['generation'] for row in packets}) >= 2, 'packet demo missed the map transition')
    return {'path': str(path), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'tick_rate': rate, 'local_slot': slot, 'packets': packets, 'last_snapshot': snapshots[-1]}


def check_handshakes(server, port, root):
    probe = root / 'handshake-probe'
    command = shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++11', '-O1']
    command += shlex.split(os.environ.get('CXXFLAGS', ''))
    command += ['-I' + str(ROOT / 'source/build/include'), '-I' + str(ROOT / 'source/blood/src'),
                str(Path(__file__).with_name('handshake-probe.cpp')),
                '-pthread', '-o', str(probe)] + shlex.split(os.environ.get('LDFLAGS', ''))
    subprocess.run(command, check=True)
    result = []
    for mode in ('malformed', 'version', 'missing-password', 'wrong-password'):
        completed = subprocess.run([str(probe), str(port), mode], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=15)
        text = completed.stdout.decode(errors='replace')
        (root / f'handshake-{mode}.log').write_text(text)
        require(completed.returncode == 0, f'handshake {mode} failed: {text.strip()}')
        require(server.status()['players'] == '0', 'rejected handshake occupied an active player slot')
        result.append({'case': mode, 'result': text.strip()})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--grp', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--renderer', type=int, choices=(0, 3, 5), default=3)
    args = parser.parse_args()
    args.binary, args.assets = args.binary.resolve(), args.assets.resolve()
    args.grp = (args.grp or next((path for path in args.assets.iterdir()
                                 if path.name.lower() == 'duke3d.grp'), args.assets / 'DUKE3D.GRP')).resolve()
    require(os.environ.get('DISPLAY'), 'graphical clients require DISPLAY (use Xvfb on Linux)')
    require(args.binary.is_file() and args.grp.is_file(), 'supply a built Rednukem binary and Duke GRP assets')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-network-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source_binary = args.binary
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source_binary, args.binary)
    fixture = generate(root / 'fixture')
    library = build_driver(root)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    games = []
    result = {'passed': False, 'failures': [], 'source_binary': str(source_binary),
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'asset_sha256': hashlib.sha256(args.grp.read_bytes()).hexdigest(), 'fixture': fixture}
    print(f'Rednukem network evidence: {root}', flush=True)
    try:
        result['external_arguments'] = check_external_arguments(args.binary, root, library)
        server = Game('server', args, root, library, port, dedicated=True)
        games.append(server)
        server.wait(lambda text: 'NET server role=dedicated' in text, 'dedicated startup', args.timeout)
        empty = poll_status(server, lambda row: int(row['tick']) >= 30 and int(row['players']) == 0,
                            'simulation before clients', args.timeout)
        result['before_clients'] = empty
        result['handshakes'] = check_handshakes(server, port, root)
        first = Game('client-first', args, root, library, port)
        games.append(first)
        first.wait(lambda text: 'NET content ready ' in text, 'first hot join content', args.timeout)
        poll_status(first, lambda row: row['state'] == 'playing', 'first authoritative snapshot', args.timeout)
        first.command_text('net_record session.rnpd\nnet_status')
        first_state = first.status()
        poll_status(server, lambda row: int(row['tick']) >= int(first_state['tick']) + 60,
                    'time before delayed join', args.timeout)
        second = Game('client-delayed', args, root, library, port)
        games.append(second)
        second.wait(lambda text: 'NET content ready ' in text, 'delayed hot join content', args.timeout)
        poll_status(second, lambda row: row['state'] == 'playing', 'delayed authoritative snapshot', args.timeout)
        poll_status(server, lambda row: int(row['players']) == 2, 'two active players', args.timeout)
        names = [row['name'] for row in records(server.text(), 'NET player ')[-2:]]
        require(len(set(names)) == 2 and all(re.fullmatch(r'Duke\d{2}', name) for name in names),
                'empty client names were not assigned distinct Duke## defaults')
        result['default_names'] = names
        second.status()
        local_slot = records(second.text(), 'NET snapshot ')[-1]['local']
        before = [row for row in records(second.text(), 'NET player ') if row['slot'] == local_slot][-1]
        second.key(26, True)  # W through the ordinary SDL event path.
        time.sleep(2)
        second.key(26, False)
        second.input_barrier()
        time.sleep(0.5)
        second.command_text('net_status\nscreenshot')
        after = [row for row in records(second.text(), 'NET player ') if row['slot'] == local_slot][-1]
        require(any(before[axis] != after[axis] for axis in ('x', 'y', 'z')),
                'ordinary client movement did not change authoritative player position')
        result['movement'] = {'before': before, 'after': after}
        generation = int(server.status()['map_generation'])
        server.command_text('map RNET2.MAP')
        for game in (server, first, second):
            poll_status(game, lambda row: int(row['map_generation']) > generation and
                        row['state'] == 'playing', 'second map', args.timeout)
        first.command_text('net_stoprecord\nnet_status')
        demo = first.path / 'session.rnpd'
        require(demo.is_file() and demo.stat().st_size > 1024, 'packet demo was not written')
        result['demo'] = inspect_demo(demo)
        first.stop()
        second.stop()
        empty = poll_status(server, lambda row: int(row['players']) == 0, 'all clients disconnect', args.timeout)
        poll_status(server, lambda row: int(row['tick']) >= int(empty['tick']) + 30 and
                    int(row['players']) == 0, 'empty server continues', args.timeout)
        rejoin = Game('client-rejoin', args, root, library, port)
        games.append(rejoin)
        rejoin.wait(lambda text: 'NET content ready ' in text, 'rejoin content', args.timeout)
        poll_status(rejoin, lambda row: row['state'] == 'playing', 'join after everyone left', args.timeout)
        require(int(rejoin.status()['map_generation']) > generation, 'rejoin loaded the wrong map generation')
        rejoin.stop()
        server.stop()
        require('NETWORK_TEST_DEVICES windows=0 audio=0' in server.console(),
                'dedicated server created a graphics/audio device or failed orderly shutdown')
        replay = Game('packet-playback', args, root, library, playback=True)
        games.append(replay)
        replay.wait(lambda text: 'E1L1: NETWORK ROOM ONE' in text, 'offline replay setup', args.timeout)
        replay.command_text(f'net_playback "{demo}"')
        final_snapshot = result['demo']['last_snapshot']
        replay.wait(lambda text: 'NET playback finished ' in text, 'packet demo completion',
                    args.timeout + final_snapshot['elapsed_ticks'] / result['demo']['tick_rate'])
        replay.command_text('net_status\nscreenshot')
        result['playback'] = replay.status()
        installed = records(replay.text(), 'NET snapshot ')[-1]
        require(int(result['playback']['map_generation']) == final_snapshot['generation'] and
                int(installed['tick']) == final_snapshot['tick'] and
                int(installed['sequence']) == final_snapshot['sequence'] and
                installed['hash'] == final_snapshot['hash'],
                'packet playback final authoritative snapshot differs from the recording')
        result['playback_snapshot'] = installed
        replay.stop()
        for game in games:
            require(game.process.returncode == 0 and not game.forced, f'{game.name}: unclean shutdown')
            require(not re.search(r'AddressSanitizer|runtime error:|Segmentation fault|NETWORK_TEST_FORBIDDEN',
                                  game.text() + game.console()), f'{game.name}: runtime error')
        result['passed'] = True
    except (Failure, OSError, subprocess.SubprocessError, KeyError, IndexError) as error:
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
