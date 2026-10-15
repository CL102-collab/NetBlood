#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise co-op lives/revival, frozen intermission and rotation in real games.

The original fixture CON makes crouching lethal, and Use respawns a dead
player. Damage and revival still pass through the actual authoritative game
simulation; no test-only network messages or memory writes are involved.
"""
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

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run as runner

require, Failure, records = runner.require, runner.Failure, runner.records
ROOT = Path(__file__).resolve().parents[2]


def map_name(row):
    return row['map'].lstrip('/\\').upper()


def latest(game, prefix, player=None):
    game.command_text('net_status')
    values=records(game.text(), prefix)
    if player is not None:
        values=[item for item in values if item.get('player', item.get('slot'))==str(player)]
    require(values, f'{game.name}: missing {prefix} player={player}')
    return values[-1]


def wait_row(game, prefix, predicate, description, timeout, player=None):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        row=latest(game,prefix,player)
        if predicate(row): return row
        time.sleep(.15)
    raise Failure(f'{game.name}: timeout waiting for {description}; last={row}')


def down(client, server, player, timeout):
    client.key(6,True) # C -> Crouch -> fixture CON damage.
    try:
        return wait_row(server,'NET lives ',lambda row:row['down']=='1','downed player',timeout,player)
    finally:
        client.key(6,False)
        client.input_barrier()


def spend_life(client,server,player,before,timeout):
    client.key(8,True) # E -> Use, held for the server's three second timer.
    try:
        row=wait_row(server,'NET lives ',lambda row:int(row['remaining'])==before-1,
                     'give-up spends one life',timeout,player)
    finally:
        client.key(8,False)
        client.input_barrier()
    return row


def change_life_settings(server,clients,timeout):
    down(clients[0],server,0,timeout)
    # Input barriers acknowledge SDL consumption; allow already-sent crouch
    # commands to reach the server before its synthetic lethal input is unblocked.
    time.sleep(1)
    server.command_text('server_rule lives 0')
    disabled=wait_row(clients[0],'NET lives ',lambda row:row['remaining']=='-1' and row['down']=='0',
                      'disabled life rule reaches client',timeout,0)
    require(latest(server,'NET player ',0)['health']=='50','disabling lives left the downed body injured')
    server.command_text('server_rule lives 2')
    wait_row(clients[0],'NET lives ',lambda row:row['remaining']=='2','fresh finite life budget',timeout,0)
    for before in (2,1):
        down(clients[0],server,0,timeout)
        eliminated=spend_life(clients[0],server,0,before,timeout)
        if before>1:
            time.sleep(.2)
            clients[0].key(8,True);time.sleep(.3);clients[0].key(8,False)
            wait_row(server,'NET player ',lambda row:int(row['health'])>1,'runtime-rule fixture respawn',timeout,0)
    require(eliminated['out']=='1','runtime-rule fixture did not eliminate player')
    server.command_text('server_rule lives 3')
    restored=wait_row(clients[0],'NET lives ',lambda row:row['remaining']=='3' and row['out']=='0',
                     'changed limit revives eliminated player',timeout,0)
    require(latest(server,'NET player ',0)['health']=='50','changed limit did not revive at50 health')
    require(latest(server,'NET lives ',1)['remaining']=='3','operator change did not reset teammate budget')
    server.command_text('server_rule lives 2')
    wait_row(clients[0],'NET lives ',lambda row:row['remaining']=='2','restore fixture life limit',timeout,0)
    return {'disabled':disabled,'eliminated':eliminated,'restored':restored}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'rednukem')
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--grp',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--timeout',type=float,default=60)
    parser.add_argument('--renderer',type=int,choices=(0,3,5),default=3)
    args=parser.parse_args()
    args.binary,args.assets=args.binary.resolve(),args.assets.resolve()
    args.grp=(args.grp or next((p for p in args.assets.iterdir() if p.name.lower()=='duke3d.grp'),args.assets/'DUKE3D.GRP')).resolve()
    require(os.environ.get('DISPLAY'),'clients require DISPLAY/Xvfb')
    root=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-rules-'))
    if args.output: root.mkdir(parents=True,exist_ok=False)
    source=args.binary
    args.binary=root/'rednukem-tested';shutil.copy2(source,args.binary)
    runner.generate(root/'fixture')
    with (root/'fixture'/'fixture.con').open('a') as script:
        script.write('actor 1405 100\nifp 8 strength 0\nifp 32 endofgame 30\nifdead ifhitspace resetplayer\nenda\n')
    (root/'rotation.txt').write_text('RNET1.MAP\nRNET2.MAP\n')
    library=runner.build_driver(root)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    games=[]
    result={'passed':False,'failures':[],'phases':{},'binary':str(source),
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem rules evidence: {root}',flush=True)
    def game(name,**kwargs):
        item=runner.Game(name,args,root,library,port,**kwargs);games.append(item);return item
    try:
        server=game('server',dedicated=True,extra_arguments=('-gametype','coop','-lives','2',
            '-autoaim','off','-friendlyfire','off','-weaponsstay','markers','-itemsstay','respawn',
            '-monsters','none','-losekeys','off','-maplist',str(root/'rotation.txt')))
        server.wait(lambda text:'NET server role=dedicated' in text,'dedicated startup',args.timeout)
        rules=latest(server,'NET rules ')
        require(rules['gametype']=='coop' and rules['autoaim']=='0' and rules['weapon_mode']=='3'
                and rules['item_mode']=='1' and rules['monster_mode']=='0' and rules['keepkeys']=='1',
                f'CLI rules were not applied: {rules}')
        result['phases']['settings']=rules
        clients=[]
        for index in range(2):
            client=game(f'client-{index}')
            client.wait(lambda text:'NET content ready ' in text,'hot join',args.timeout)
            runner.poll_status(client,lambda row:row['state']=='playing','snapshot',args.timeout)
            client.command_text('bind E "gamefunc_Open"\nbind C "gamefunc_Crouch"')
            clients.append(client)
        runner.poll_status(server,lambda row:row['players']=='2','both players',args.timeout)
        time.sleep(.5)
        for player in range(2):
            require(latest(server,'NET lives ',player)['remaining']=='2','late join received wrong life budget')
        result['phases']['runtime-life-settings']=change_life_settings(server,clients,args.timeout)
        down(clients[0],server,0,args.timeout)
        clients[1].key(8,True)
        try:
            revived=wait_row(server,'NET lives ',lambda row:row['down']=='0','teammate revival',args.timeout,0)
        finally: clients[1].key(8,False)
        require(revived['remaining']=='2','revival spent a life')
        health=latest(server,'NET player ',0)
        require(health['health']=='50',f'revival did not restore 50 health: {health}')
        result['phases']['revive']={'life':revived,'player':health}
        down(clients[0],server,0,args.timeout)
        clients[0].key(8,True);time.sleep(.75);clients[0].key(8,False)
        canceled=wait_row(server,'NET lives ',lambda row:row['giveup']=='0','canceled give-up',args.timeout,0)
        require(canceled['remaining']=='2' and canceled['down']=='1','short hold incorrectly spent a life')
        result['phases']['cancel-give-up']=canceled
        for player,client in enumerate(clients):
            if player: down(client,server,player,args.timeout)
            first=spend_life(client,server,player,2,args.timeout)
            require(first['out']=='0','player eliminated before last life')
            time.sleep(.2)
            client.key(8,True);time.sleep(.3);client.key(8,False)
            wait_row(server,'NET player ',lambda row:int(row['health'])>1,'respawn',args.timeout,player)
            down(client,server,player,args.timeout)
            final=spend_life(client,server,player,1,args.timeout)
            require(final['out']=='1','last life did not enter spectating')
            result['phases'][f'elimination-{player}']=final
        pause=wait_row(server,'NET intermission ',lambda row:row['active']=='1','team-wipe intermission',args.timeout)
        generation=int(server.status()['map_generation'])
        start=time.monotonic()
        clients[0].key(26,True);time.sleep(1);clients[0].key(26,False)
        still=latest(server,'NET intermission ')
        require(still['active']=='1' and still['level_ticks']==pause['level_ticks'],'intermission did not freeze gameplay timer')
        require(12<=int(pause['remaining_seconds'])<=15,'wrong intermission duration')
        runner.poll_status(server,lambda row:int(row['map_generation'])>generation,'team wipe map restart',args.timeout)
        require(time.monotonic()-start>=10,'team wipe bypassed 15-second intermission')
        require(map_name(server.status())=='RNET1.MAP','team wipe advanced rotation instead of restarting')
        for player in range(2):
            reset=wait_row(server,'NET lives ',lambda row:row['remaining']=='2' and row['down']=='0' and row['out']=='0',
                           'fresh lives after team wipe',args.timeout,player)
        result['phases']['intermission']={'start':pause,'after_one_second':still,'reset':reset}
        generation=int(server.status()['map_generation'])
        server.command_text('nextmap')
        runner.poll_status(server,lambda row:int(row['map_generation'])>generation and map_name(row)=='RNET2.MAP',
                           'rotation next map',args.timeout)
        generation=int(server.status()['map_generation'])
        server.command_text('nextmap')
        runner.poll_status(server,lambda row:int(row['map_generation'])>generation and map_name(row)=='RNET1.MAP',
                           'rotation wraps',args.timeout)
        result['phases']['rotation']=server.status()
        for client in clients:
            runner.poll_status(client,lambda row:row['state']=='playing' and int(row['map_generation'])>generation,
                               'clients complete rotation',args.timeout)
        # Jumping is an original CON episode-exit trigger. Both clients receive
        # the real episode-one cinematic; only one skips initially.
        clients[0].key(44,True);time.sleep(.2);clients[0].key(44,False)
        cinema=wait_row(server,'NET intermission ',lambda row:row['cinematic']=='1' and row['viewers']=='2',
                        'coordinated episode cinematic',args.timeout)
        generation=int(server.status()['map_generation'])
        for _ in range(8):
            clients[0].key(8,True);time.sleep(.12);clients[0].key(8,False);time.sleep(.2)
        waiting=wait_row(server,'NET intermission ',lambda row:row['cinematic']=='1' and row['viewers']=='1',
                         'one client skipped independently',args.timeout)
        require(int(server.status()['map_generation'])==generation,'first skip advanced the map before the other viewer')
        require(waiting['level_ticks']==cinema['level_ticks'],'world moved during cinematic')
        late=game('client-cinematic-hotjoin')
        late.wait(lambda text:'NET content ready ' in text,'join during cinematic',args.timeout)
        runner.poll_status(late,lambda row:row['state']=='playing','cinematic hot-join snapshot',args.timeout)
        require(latest(server,'NET intermission ')['viewers']=='1','late join added a cinematic completion obligation')
        clients.append(late)
        for _ in range(8):
            clients[1].key(8,True);time.sleep(.12);clients[1].key(8,False);time.sleep(.2)
        runner.poll_status(server,lambda row:int(row['map_generation'])>generation,
                           'advance after all viewers finished',args.timeout)
        result['phases']['synchronized-cinematic']={'begin':cinema,'waiting':waiting,'next':server.status()}
        # A one-minute limit measures the real server clock, including empty service.
        for client in clients: client.stop()
        runner.poll_status(server,lambda row:row['players']=='0','empty server',args.timeout)
        server.command_text('server_rule timelimit 1\nrestartmap')
        current=server.status()
        generation=int(current['map_generation'])
        target='RNET1.MAP' if map_name(current)=='RNET2.MAP' else 'RNET2.MAP'
        pause=wait_row(server,'NET intermission ',lambda row:row['active']=='1','time-limit ending',args.timeout+65)
        require(12<=int(pause['remaining_seconds'])<=15,'time limit omitted the final countdown')
        runner.poll_status(server,lambda row:int(row['map_generation'])>generation and map_name(row)==target,
                           'time-limit rotation',args.timeout)
        result['phases']['empty-time-limit']={'pause':pause,'next':server.status()}
        result['passed']=True
    except (Failure,OSError,subprocess.SubprocessError,KeyError,IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for item in reversed(games): item.stop()
        result['games']={item.name:item.result() for item in games}
        for item in games:
            if item.process.returncode != 0 or item.forced:
                result['passed']=False
                result['failures'].append(f'{item.name}: unclean shutdown (exit {item.process.returncode}, forced={item.forced})')
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(('PASS' if result['passed'] else 'FAIL')+f': {root / "result.json"}',flush=True)
    for error in result['failures']:print(error,file=sys.stderr)
    return 0 if result['passed'] else 1

if __name__=='__main__':sys.exit(main())
