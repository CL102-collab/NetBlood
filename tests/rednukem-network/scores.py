#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify byte-counter score wraparound and resets through real CON deaths.

Crouching repeatedly kills the fixture player, who automatically respawns.
All score increments come from the normal native death path; there are no
test-only game commands, memory writes or synthetic protocol messages.
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

sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent))
import run as runner
from rules import latest,wait_row,map_name

ROOT=Path(__file__).resolve().parents[2]
require,Failure,records=runner.require,runner.Failure,runner.records


def social(game):
    game.command_text('net_socialstatus')
    rows=records(game.text(),'NET social ')
    require(rows,f'{game.name}: missing scoreboard row')
    return rows[-1]


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
    root=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-scores-'))
    if args.output:root.mkdir(parents=True,exist_ok=False)
    source=args.binary;args.binary=root/'rednukem-tested';shutil.copy2(source,args.binary)
    runner.generate(root/'fixture')
    with (root/'fixture'/'fixture.con').open('a') as script:
        script.write('actor 1405 100\nifdead { ifcount 3 { resetcount resetplayer } break }\n'
                     'ifp 8 { strength 0 resetcount }\nenda\n')
    library=runner.build_driver(root)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    games=[]
    result={'passed':False,'failures':[],'phases':{},'binary':str(source),
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem score evidence: {root}',flush=True)
    def game(name,**kwargs):
        item=runner.Game(name,args,root,library,port,**kwargs);games.append(item);return item
    try:
        server=game('server',dedicated=True,extra_arguments=('-gametype','deathmatch','-scorelimit','300',
                                                          '-maprotation','E1L1,E1L2'))
        server.wait(lambda text:'NET server role=dedicated' in text,'dedicated startup',args.timeout)
        client=game('client')
        client.wait(lambda text:'NET content ready ' in text,'join',args.timeout)
        runner.poll_status(client,lambda row:row['state']=='playing','snapshot',args.timeout)
        client.command_text('bind C "gamefunc_Crouch"')
        client.key(6,True)
        try:
            wait_row(server,'NET score ',lambda row:int(row['total'])<=-300,'300 native self-kills',args.timeout+60,0)
        finally:client.key(6,False);client.input_barrier()
        # Let the final death and roster message settle before comparing values.
        time.sleep(1)
        score=latest(server,'NET score ',0)
        authoritative=int(score['total'])
        require(authoritative<=-300 and int(score['native_frags'])==0,'score did not pass native byte range')
        require((-authoritative)&255==int(score['native_penalties']),f'wide/native counters disagree: {score}')
        received=social(client)
        require(int(received['score'])==authoritative,f'client roster score wrapped: {received}, expected {authoritative}')
        require(latest(server,'NET intermission ')['active']=='0','negative score incorrectly triggered positive score limit')
        result['phases']['wraparound']={'server':score,'client':received}
        generation=int(server.status()['map_generation'])
        server.command_text('restartmap')
        runner.poll_status(client,lambda row:row['state']=='playing' and int(row['map_generation'])>generation,
                           'map restart',args.timeout)
        reset=latest(server,'NET score ',0)
        require(reset['total']=='0' and reset['native_penalties']=='0',f'map restart retained previous score: {reset}')
        result['phases']['map-reset']=reset
        client.key(6,True)
        try:wait_row(server,'NET score ',lambda row:int(row['total'])<=-3,'new map scoring',args.timeout,0)
        finally:client.key(6,False);client.input_barrier()
        client.stop()
        runner.poll_status(server,lambda row:row['players']=='0','departed player',args.timeout)
        replacement=game('replacement')
        replacement.wait(lambda text:'NET content ready ' in text,'new occupant',args.timeout)
        runner.poll_status(replacement,lambda row:row['state']=='playing','replacement snapshot',args.timeout)
        reset=latest(server,'NET score ',0)
        require(reset['total']=='0' and reset['native_penalties']=='0',f'new occupant inherited old score: {reset}')
        result['phases']['occupant-reset']=reset
        generation=int(server.status()['map_generation'])
        server.command_text('nextmap')
        advanced=runner.poll_status(server,lambda row:int(row['map_generation'])>generation,
                                    'episode alias rotation',args.timeout)
        require(map_name(advanced)=='RNET2.MAP',f'episode alias rotation repeated initial map: {advanced}')
        result['phases']['episode-alias-rotation']=advanced
        result['passed']=True
    except (Failure,OSError,subprocess.SubprocessError,KeyError,IndexError) as error:
        result['failures'].append(str(error))
    finally:
        for item in reversed(games):item.stop()
        result['games']={item.name:item.result() for item in games}
        for item in games:
            if item.process.returncode!=0 or item.forced:
                result['passed']=False
                result['failures'].append(f'{item.name}: unclean shutdown (exit {item.process.returncode}, forced={item.forced})')
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(('PASS' if result['passed'] else 'FAIL')+f': {root / "result.json"}',flush=True)
    for error in result['failures']:print(error,file=sys.stderr)
    return 0 if result['passed'] else 1


if __name__=='__main__':sys.exit(main())
