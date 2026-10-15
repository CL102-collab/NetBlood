#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test Rednukem STUN/Discord joins with local protocol fixtures and real games.

Requires Duke assets and DISPLAY/Xvfb. No public STUN server, Discord account,
registry, or persistent desktop association is used. Reports retain IPC frames,
UDP source ports, and game logs so the actual join behavior can be reviewed.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('rednukem_network_fixture', HERE / 'run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
spec = importlib.util.spec_from_file_location('rednukem_discord_fixture', ROOT / 'tests/network/discord-presence.py')
discord = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discord)
spec = importlib.util.spec_from_file_location('rednukem_stun_fixture', ROOT / 'tests/network/stun-live.py')
stun = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stun)
require, Failure = runner.require, runner.Failure


def tap(game, key):
    game.key(key, True)
    time.sleep(.12)
    game.key(key, False)
    game.input_barrier()
    time.sleep(.12)


class PortableGame(runner.Game):
    """A real portable install launched without ordinary game CLI options."""
    def __init__(self, name, args, root, library, port, peer, dedicated=False, launch_uri=None):
        self.name, self.path = name, root / name
        self.path.mkdir()
        self.command_serial = self.input_serial = self.text_serial = 0
        self.dedicated, self.forced = dedicated, False
        self.fifo = self.path / 'input.fifo'
        os.mkfifo(self.fifo)
        self.events = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        executable = self.path / 'rednukem'
        shutil.copy2(args.binary, executable)
        for name in ('fixture.grp', 'fixture.con', 'RNET1.MAP', 'RNET2.MAP'):
            shutil.copy2(root / 'fixture' / name, self.path / name)
        group_info = (root / 'fixture/fixture.grpinfo').read_text()
        (self.path / 'fixture.grpinfo').write_text(group_info.replace('dependency 0', 'dependency 0 scriptname "fixture.con"'))
        (self.path / 'autoload').mkdir()
        (self.path / 'autoload/DUKE3D.GRP').symlink_to(args.grp)
        (self.path / 'user_profiles_disabled').touch()
        (self.path / 'rednukem.cfg').write_text(
            '[Setup]\nForceSetup = 0\nNoAutoLoad = 0\nSelectedGRP = "fixture.grp"\n'
            '[Screen Setup]\n' + f'Renderer = {args.renderer}\nScreenMode = 0\n'
            'ScreenBPP = 32\nScreenWidth = 640\nScreenHeight = 480\n')
        (self.path / 'autoexec.cfg').write_text('r_maxfps 60\nnet_stunserver off\n')
        environment = dict(os.environ, **peer.environment, SDL_AUDIODRIVER='dummy',
                           NETWORK_TEST_EVENTS=str(self.fifo), LD_PRELOAD=str(library))
        if os.environ.get('LD_PRELOAD'):
            environment['LD_PRELOAD'] += ':' + os.environ['LD_PRELOAD']
        command = [str(executable)]
        if dedicated:
            command += ['-usecwd', '-dedicated', '-server', '2', '-port', str(port), '-v1', '-l1', '-s1']
            for variable in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY'):
                environment.pop(variable, None)
            environment.update(SDL_VIDEODRIVER='network-test-invalid-video-driver',
                               SDL_AUDIODRIVER='network-test-invalid-audio-driver', NETWORK_TEST_HEADLESS='1')
        elif launch_uri:
            command += ['-connect-uri', launch_uri]
        else:
            command += ['-discord-launch', discord.APPLICATION_ID, f'discord-{discord.APPLICATION_ID}://']
        self.command = command
        self.log = (self.path / 'console.log').open('w')
        # Browsers and Discord need not launch from the game's working directory.
        self.process = subprocess.Popen(command, cwd=self.path if dedicated else '/', env=environment,
                                        stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT)


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
    require(os.environ.get('DISPLAY'), 'game clients need DISPLAY (use Xvfb)')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-presence-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source = args.binary
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source, args.binary)
    runner.generate(root / 'fixture')
    library = runner.build_driver(root)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    peer = discord.FakeDiscord(root / 'desktop')
    echo = stun.FakeStun()
    games = []
    result = dict(passed=False, binary=str(source), binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(), phases={}, failures=[])
    print(f'Rednukem presence evidence: {root}', flush=True)

    def game(name, **kwargs):
        with discord.environment(peer.environment):
            item = runner.Game(name, args, root, library, port, capacity=2, password='', **kwargs)
        games.append(item)
        return item

    try:
        server = game('server', dedicated=True, extra_arguments=(
            '-discord-server-address', f'rednukem://127.0.0.1:{port}'))
        server.wait(lambda text: 'NET server role=dedicated' in text, 'dedicated server', args.timeout)
        server.command_text(f'discord_server_address ""\ndiscord_appid {discord.APPLICATION_ID}\nnet_stunserver "rednukem://127.0.0.1:{echo.port}"')
        link = f'rednukem://127.0.0.1:{port}'
        server.wait(lambda text: 'NET STUN discovered ' + link in text, 'same-socket STUN mapping', args.timeout)
        require(echo.requests and all(row['source'][1] == port for row in echo.requests),
                'STUN requests did not originate from the bound game UDP port')
        result['phases']['same-socket-stun'] = echo.requests
        client = game('client', playback=True)
        client.wait(lambda text: 'E1L1: NETWORK ROOM ONE' in text, 'offline fixture map', args.timeout)
        client.command_text(f'discord_appid {discord.APPLICATION_ID}\ndiscord_enabled 1')
        offline = peer.activity(lambda value: value and not discord.secret(value) and not value.get('party'),
                                'offline nonjoinable presence', args.timeout)
        result['phases']['offline'] = offline
        mark = peer.mark()
        # Bracketed IPv6 exercises the actual callback join path; the STUN
        # mapping advertised by the server then supplies its IPv4 invite link.
        peer.join(f'rednukem://[0000:0000:0000:0000:0000:0000:0000:0001]:{port}')
        client.wait(lambda text: 'NET content ready ' in text, 'Discord callback join', args.timeout)
        activity = peer.activity(lambda value: value and discord.secret(value) == link
                                 and value.get('party', {}).get('size') == [1, 2],
                                 'advertised STUN endpoint and party', args.timeout, mark)
        party = activity['party']['id']
        require(party.startswith('rednukem:'), 'Discord party has wrong game branding')
        require(activity.get('timestamps') == offline.get('timestamps'), 'join reset elapsed presence time')
        result['phases']['discord-join-stun-advertisement'] = activity
        mark = peer.mark()
        guest = game('guest')
        guest.wait(lambda text: 'NET content ready ' in text, 'second player', args.timeout)
        full = peer.activity(lambda value: value and value.get('party', {}).get('size') == [2, 2]
                             and not discord.secret(value), 'full server hides invite', args.timeout, mark)
        require(full['party']['id'] == party, 'player join replaced session party identity')
        result['phases']['full-server'] = full
        mark = peer.mark()
        guest.stop()
        peer.activity(lambda value: value and value.get('party', {}).get('size') == [1, 2]
                      and discord.secret(value) == link, 'invite returns after departure', args.timeout, mark)
        mark = peer.mark()
        server.command_text('map RNET2.MAP')
        changed = peer.activity(lambda value: value and 'RNET2.MAP' in value.get('details', '').upper()
                                and discord.secret(value) == link, 'map change presence', args.timeout, mark)
        require(changed['party']['id'] == party, 'map rotation replaced party identity')
        require(changed.get('timestamps') == offline.get('timestamps'), 'map change reset elapsed presence time')
        result['phases']['map-change'] = changed
        client.command_text('net_record presence.rnpd')
        time.sleep(2)
        client.command_text('net_stoprecord')
        mark = peer.mark()
        client.command_text('net_disconnect')
        peer.activity(lambda value: value and not discord.secret(value) and not value.get('party'),
                      'disconnect removes party/invites', args.timeout, mark)
        mark = peer.mark()
        client.command_text('net_playback presence.rnpd')
        playback = peer.activity(lambda value: value and 'demo' in value.get('details', '').lower()
                                 and not discord.secret(value) and not value.get('party'),
                                 'packet demo hides invites', args.timeout, mark)
        result['phases']['nonjoinable-demo'] = playback
        client.command_text('net_disconnect')
        offset = len(client.text())
        for malformed in ('netblood://127.0.0.1', 'rednukem://127.0.0.1:0',
                          'rednukem://example.org/path', 'rednukem://host\" -x evil.con',
                          'rednukem://host;quit', 'https://example.org'):
            peer.join(malformed)
            time.sleep(.1)
        # The IPC parser discards whitespace-bearing secrets before dispatch;
        # the remaining five reach the game's strict Rednukem URI parser.
        client.wait(lambda text: text[offset:].count('DISCORD rejected invitation:') >= 5,
                    'malformed invitations rejected', args.timeout)
        require('NET connecting ' not in client.text()[offset:], 'malformed secret started a connection')
        mark = peer.mark()
        client.command_text('discord_enabled 0')
        peer.activity(lambda value: value is None, 'opt-out clears presence', args.timeout, mark)
        deadline = time.monotonic() + args.timeout
        while peer.connection is not None and time.monotonic() < deadline:
            time.sleep(.05)
        require(peer.connection is None, 'Discord IPC remained open after opt-out')
        result['phases']['malformed-and-opt-out'] = True
        # Explicit endpoint override is advertised without exposing passwords.
        server.command_text('discord_server_address "rednukem://example.test:23456"\nnet_stun_status')
        server.wait(lambda text: 'NET join link: rednukem://example.test:23456' in text,
                    'explicit endpoint override', args.timeout)
        server.command_text('discord_server_address ""\nnet_stunserver off\nnet_stun_status')
        require(not echo.errors and not peer.errors, f'fixture error: {echo.errors + peer.errors}')
        require(peer.connections == 1, 'dedicated/unconfigured clients unexpectedly contacted Discord')
        server.stop()
        portable_server = PortableGame('portable-server', args, root, library, port, peer, dedicated=True)
        games.append(portable_server)
        portable_server.wait(lambda text: 'NET server role=dedicated' in text, 'portable server', args.timeout)
        mark = peer.mark()
        cold = PortableGame('cold-discord-launch', args, root, library, port, peer)
        games.append(cold)
        peer.activity(lambda value: value and not discord.secret(value), 'cold Discord launch at menu', args.timeout, mark)
        require('E1L1: NETWORK ROOM ONE' not in cold.text(), 'cold launch entered gameplay before the invitation')
        peer.join(link)
        cold.wait(lambda text: 'NET content ready ' in text, 'cold-launched Discord join', args.timeout)
        cold_activity = peer.activity(lambda value: value and discord.secret(value) == link
                                      and value.get('party', {}).get('size') == [1, 2],
                                      'cold-launch joined activity', args.timeout, mark)
        result['phases']['cold-discord-launch-and-join'] = cold_activity

        # A Discord/URI invitation carries only the endpoint. Password entry
        # happens in the real masked menu, including retry and cancellation.
        protected_password = 'fixture secret;token'
        portable_server.command_text(f'echo PASSWORD_CHAIN_BEGIN; password "{protected_password}"; echo PASSWORD_CHAIN_OK')
        require('PASSWORD_CHAIN_OK' in portable_server.text(), 'quoted password swallowed the next command')
        cold.command_text('net_disconnect\npassword')
        offset = len(cold.text())
        peer.join(link)
        cold.wait(lambda text: 'NET password prompt (required)' in text[offset:],
                  'missing password menu', args.timeout)
        cold.enter_text('incorrect fixture password')
        tap(cold, 40)
        cold.wait(lambda text: 'NET password prompt (incorrect)' in text[offset:],
                  'incorrect password retry menu', args.timeout)
        connections = cold.text().count('NET connecting ')
        tap(cold, 41)
        time.sleep(.3)
        require(cold.text().count('NET connecting ') == connections, 'cancelled password prompt retried')
        offset = len(cold.text())
        peer.join(link)
        cold.wait(lambda text: 'NET password prompt (incorrect)' in text[offset:],
                  'new invitation after cancelled password prompt', args.timeout)
        cold.enter_text(protected_password)
        tap(cold, 40)
        cold.wait(lambda text: 'NET content ready ' in text[offset:],
                  'masked password retry joins protected server', args.timeout)
        # Real console input exercises history redaction before dispatch. The
        # semicolon in the quoted value must remain part of the password.
        tap(cold, 53)
        cold.enter_text(f'password "{protected_password}"; history')
        tap(cold, 40)
        tap(cold, 53)
        cold.wait(lambda text: 'Command history:' in text, 'redacted password command history', args.timeout)
        require(protected_password not in cold.text() and protected_password not in portable_server.text(),
                'password leaked into console echo/history')
        require(protected_password not in json.dumps(peer.rows()), 'password leaked into Discord IPC')
        offset = len(cold.text())
        cold.command_text('net_disconnect')
        peer.join(link)
        cold.wait(lambda text: 'NET content ready ' in text[offset:],
                  'console quoted password preserved for reconnect', args.timeout)
        result['phases']['protected-discord-join-password-menu-and-redaction'] = True
        uri_client = PortableGame('cold-uri-launch', args, root, library, port, peer, launch_uri=link)
        games.append(uri_client)
        uri_client.wait(lambda text: 'NET password prompt (required)' in text,
                        'cold URI password menu', args.timeout)
        uri_client.enter_text(protected_password)
        tap(uri_client, 40)
        uri_client.wait(lambda text: 'NET content ready ' in text,
                        'cold URI joins protected server', args.timeout)
        require(protected_password not in uri_client.text(), 'URI password menu leaked the secret')
        result['phases']['cold-uri-protected-join'] = True
        uri_client.stop()
        cold.command_text('net_disconnect')
        # Main Join Game -> optional masked password -> custom UDP port -> Connect.
        for key in (81, 40, 81, 81, 40):
            tap(cold, key)
        cold.enter_text(protected_password)
        tap(cold, 40)
        tap(cold, 82)
        tap(cold, 40)
        for _ in range(5):
            tap(cold, 42)
        cold.enter_text(str(port))
        tap(cold, 40)
        offset = len(cold.text())
        for key in (81, 81, 40):
            tap(cold, key)
        cold.wait(lambda text: 'NET content ready ' in text[offset:],
                  'main Join Game menu with optional password and custom port', args.timeout)
        result['phases']['main-join-menu-password-and-port'] = True
        result['passed'] = True
    except (Failure, discord.Failure, OSError, subprocess.SubprocessError, KeyError, IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for item in reversed(games):
            item.stop()
            if item.process.returncode != 0 or item.forced:
                result['passed'] = False
                result['failures'].append(f'{item.name}: unclean shutdown (exit {item.process.returncode})')
        result['games'] = {item.name: item.result() for item in games}
        result['discord_frames'] = peer.rows()
        result['stun_requests'] = echo.requests
        peer.close()
        echo.close()
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(('PASS' if result['passed'] else 'FAIL') + f': {root / "result.json"}', flush=True)
    for error in result['failures']:
        print(error, file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
