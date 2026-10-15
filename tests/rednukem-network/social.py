#!/usr/bin/env python3
"""Live Rednukem social checks: roster, team privacy, votes, UI and balancing."""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from run import ROOT, Game, Failure, require, records, generate, build_driver, poll_status


def social(game):
    before = len(records(game.text(), 'NET social '))
    game.command_text('net_socialstatus')
    return {int(row['slot']): row for row in records(game.text(), 'NET social ')[before:]}


def vote_status(game):
    game.command_text('net_socialstatus')
    return records(game.text(), 'NET vote ')[-1]


def settled(game, timeout):
    poll_status(game, lambda row: row['state'] == 'playing', 'playing', timeout)


def vote_result(server, previous, expected, timeout=40):
    server.wait(lambda text: text.count('Vote passed') + text.count('Vote failed') > previous,
                '30-second vote result', timeout)
    messages = [line for line in server.text().splitlines() if 'Vote passed' in line or 'Vote failed' in line]
    require(expected in messages[-1], f'Unexpected vote result: {messages[-1]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--grp', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--chat-only', action='store_true', help='quick roster, Tab chat, scoreboard smoke')
    parser.add_argument('--menu-only', action='store_true', help='quick actual menu vote/input smoke')
    parser.add_argument('--renderer', type=int, choices=(0, 3, 5), default=3)
    args = parser.parse_args()
    args.binary, args.assets = args.binary.resolve(), args.assets.resolve()
    args.grp = (args.grp or next(path for path in args.assets.iterdir() if path.name.lower() == 'duke3d.grp')).resolve()
    require(os.environ.get('DISPLAY'), 'clients require an X11 DISPLAY')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-social-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    source_binary = args.binary
    args.binary = root / 'rednukem-tested'
    shutil.copy2(source_binary, args.binary)
    generate(root / 'fixture')
    library = build_driver(root)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    games, clients = [], []
    result = {'passed': False, 'failures': [], 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem social evidence: {root}', flush=True)
    try:
        server = Game('server', args, root, library, port, dedicated=True,
                      extra_arguments=('-gametype', 'teams', '-maprotation', 'RNET1.MAP,RNET2.MAP'))
        games.append(server)
        server.wait(lambda text: 'NET server role=dedicated' in text, 'server startup', args.timeout)
        if args.menu_only:
            for index in range(2):
                client = Game(f'client-{index}',args,root,library,port)
                games.append(client)
                clients.append(client)
                settled(client,args.timeout)
            generation = server.status()['map_generation']
            # Escape -> New Game -> Network -> Host Game -> Launch Game.
            for key in (41,40,81,81,40,81,40):
                clients[0].key(key,True)
                clients[0].input_barrier()
                time.sleep(0.3)
                clients[0].key(key,False)
                clients[0].input_barrier()
                time.sleep(0.15)
            server.wait(lambda text:'called a vote: Map: RNET1.MAP' in text or 'called a vote: Map: /RNET1.MAP' in text,'ordinary menu map vote',args.timeout)
            clients[1].key(58,True)
            clients[1].input_barrier()
            time.sleep(0.15)
            clients[1].key(58,False)
            clients[1].input_barrier()
            time.sleep(0.5)
            state=vote_status(server)
            require(state['active']=='1' and state['yes']=='2','menu vote or F1 ballot failed')
            require(server.status()['map_generation']==generation,'menu bypassed vote deadline')
            result['menu_vote']=state
            for client in clients:
                require(client.status()['state']=='playing','menu left client in legacy waiting screen')
        else:
            for index in range(3):
                client = Game(f'client-{index}', args, root, library, port)
                games.append(client)
                clients.append(client)
                settled(client,args.timeout)
                client.command_text('name ""')
            poll_status(server, lambda row: row['players'] == '3', 'three clients', args.timeout)
            roster = social(server)
            require([roster[i]['name'] for i in range(3)] == ['Duke00','Duke01','Duke02'], 'blank names were not unique Duke## names')
            require([roster[i]['team'] for i in range(3)] == ['0','1','0'], 'joins did not choose the smaller team')
            result['initial_roster'] = roster
            # Use the normal T chat entry and Tab mode toggle, including SDL
            # text input, before separately checking the console commands.
            for key in (23,43):
                clients[0].key(key,True);clients[0].input_barrier();time.sleep(.3);clients[0].key(key,False)
                clients[0].input_barrier();time.sleep(.15)
            clients[0].enter_text('PRIVATE_TAB_TOKEN')
            clients[0].key(40,True);clients[0].input_barrier();time.sleep(.3);clients[0].key(40,False)
            clients[0].input_barrier()
            clients[2].wait(lambda text:'(TEAM) Duke00: PRIVATE_TAB_TOKEN' in text,'Tab team-chat input',args.timeout)
            require('PRIVATE_TAB_TOKEN' not in clients[1].text(),'Tab team chat reached opponent')
            time.sleep(.5)
            clients[0].command_text('say_team PRIVATE_TEAM_TOKEN')
            clients[2].wait(lambda text: '(TEAM) Duke00: PRIVATE_TEAM_TOKEN' in text, 'teammate chat', args.timeout)
            time.sleep(0.5)
            require('PRIVATE_TEAM_TOKEN' not in clients[1].text(), 'opponent received private team chat')
            clients[0].command_text('say PUBLIC_TOKEN')
            for client in clients:
                client.wait(lambda text: 'Duke00: PUBLIC_TOKEN' in text, 'public chat', args.timeout)
            server.command_text('say OPERATOR_TOKEN')
            clients[1].wait(lambda text: 'SERVER: OPERATOR_TOKEN' in text, 'dedicated announcement', args.timeout)
            clients[0].key(68,True)  # Default F11 scoreboard binding.
            clients[0].input_barrier()
            time.sleep(0.5)
            clients[0].key(69,True)  # F12 screenshot without opening the console.
            clients[0].input_barrier()
            time.sleep(0.15)
            clients[0].key(69,False)
            time.sleep(0.5)
            clients[0].key(68,False)
            result['scoreboard_screenshots'] = [str(path) for path in clients[0].path.rglob('*.png')]
            require(result['scoreboard_screenshots'], 'scoreboard screenshot missing')

            if not args.chat_only:
                # Freeze three eligible players, then join a fourth. Two yes votes must
                # still pass; counting the late arrival would incorrectly require three.
                generation = int(server.status()['map_generation'])
                clients[0].command_text('callvote restart')
                server.wait(lambda text: 'called a vote: Restart map' in text, 'restart vote', args.timeout)
                client = Game('client-3', args, root, library, port)
                games.append(client)
                clients.append(client)
                settled(client,args.timeout)
                client.command_text('name ""\nvote yes')
                state = vote_status(server)
                require(state['eligible'] == '3' and state['yes'] == '1', 'late joiner changed electorate or cast a ballot')
                clients[1].key(58,True)  # F1 through SDL, not a console shortcut.
                clients[1].input_barrier()
                time.sleep(0.15)
                clients[1].key(58,False)
                clients[1].input_barrier()
                time.sleep(0.5)
                state = vote_status(server)
                require(state['yes'] == '2' and state['required'] == '2', 'F1 did not cast the eligible yes ballot')
                result['fixed_electorate'] = state
                require(int(server.status()['map_generation']) == generation, 'vote passed before the 30-second deadline')
                vote_result(server,0,'Vote passed')
                for game in (server,*clients):
                    poll_status(game,lambda row: int(row['map_generation']) > generation and row['state']=='playing','voted restart',args.timeout)
                clients[0].command_text('callvote next')
                clients[0].wait(lambda text: 'Vote cooldown:' in text,'cooldown survives map restart',args.timeout)

                before = social(server)
                previous = server.text().count('Vote passed') + server.text().count('Vote failed')
                vote_calls = server.text().count('called a vote:')
                clients[1].command_text('callvote shuffle')
                server.wait(lambda text: text.count('called a vote:') > vote_calls, 'shuffle vote started', args.timeout)
                for index in (0,2):
                    clients[index].command_text('vote yes')
                vote_result(server,previous,'Vote passed')
                after = social(server)
                groups_before = {frozenset(slot for slot,row in before.items() if row['team']==team) for team in ('0','1')}
                groups_after = {frozenset(slot for slot,row in after.items() if row['team']==team) for team in ('0','1')}
                require(groups_after != groups_before, 'shuffle only renamed teams or left teammates unchanged')
                require(sum(row['team']=='0' for row in after.values()) == 2, 'shuffle unbalanced teams')
                result['shuffle'] = {'before':before,'after':after}

                for caller,action,target in ((2,'map RNET2.MAP','RNET2.MAP'),(3,'next','RNET1.MAP')):
                    previous = server.text().count('Vote passed') + server.text().count('Vote failed')
                    vote_calls = server.text().count('called a vote:')
                    clients[caller].command_text('callvote '+action)
                    server.wait(lambda text: text.count('called a vote:') > vote_calls, 'map vote started', args.timeout)
                    for index in range(4):
                        if index != caller:
                            clients[index].command_text('vote yes')
                    vote_result(server,previous,'Vote passed')
                    for game in (server,*clients):
                        poll_status(game,lambda row: row['map'].lstrip('/')==target and row['state']=='playing','voted '+action,args.timeout)
                result['map_and_next_votes'] = True

                # With 4 frozen voters, one yes and one no must fail. Disconnecting
                # two voters after start must not lower the threshold to 2 players.
                previous = server.text().count('Vote passed') + server.text().count('Vote failed')
                vote_calls = server.text().count('called a vote:')
                clients[0].command_text('callvote restart')
                server.wait(lambda text: text.count('called a vote:') > vote_calls, 'failed vote started', args.timeout)
                clients[1].command_text('vote no')
                state = vote_status(server)
                require(state['eligible']=='4' and state['yes']=='1' and state['no']=='1','failed vote setup incorrect')
                before = social(server)
                removed_team = before[2]['team']
                removed = [slot for slot,row in before.items() if row['team']==removed_team]
                for slot in removed:
                    clients[slot].stop()
                poll_status(server,lambda row: row['players']=='2','two departures',args.timeout)
                after = social(server)
                require(sorted(row['team'] for row in after.values())==['0','1'],'disconnect did not auto-balance the remaining team')
                result['disconnect_balance'] = {'before':before,'after':after}
                state = vote_status(server)
                require(state['eligible']=='4' and state['required']=='3', 'departures lowered the frozen vote threshold')
                vote_result(server,previous,'Vote failed')
                result['failed_vote_fixed_electorate'] = vote_status(server)
        result['passed'] = True
    except (Failure,OSError,subprocess.SubprocessError,KeyError,IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result['passed'] = False
                result['failures'].append(f'{game.name}: unclean shutdown')
        result['games'] = {game.name:game.result() for game in games}
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(('PASS' if result['passed'] else 'FAIL')+f': {root / "result.json"}',flush=True)
    for error in result['failures']:
        print(error,file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__=='__main__':
    sys.exit(main())
