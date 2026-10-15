#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify native attacker credit is cleared by co-op revival.

Quick Kick emits a CON radius attack (which excludes its emitting player).
Crouch supplies a later self-inflicted death. The test first proves the radius
attack credits its real owner, then checks revival clears that attribution.
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
from rules import down,spend_life,latest,wait_row

ROOT=Path(__file__).resolve().parents[2]
require,Failure=runner.require,runner.Failure


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
    root=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-revive-score-'))
    if args.output:root.mkdir(parents=True,exist_ok=False)
    source=args.binary;args.binary=root/'rednukem-tested';shutil.copy2(source,args.binary)
    runner.generate(root/'fixture')
    with (root/'fixture'/'fixture.con').open('a') as script:
        script.write('actor 1405 100\nifhitweapon resetcount\n'
                     'ifp 512 hitradius 2048 1000 1000 1000 1000\n'
                     'ifp 8 strength 0\nifdead ifhitspace resetplayer\nenda\n')
    library=runner.build_driver(root)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    games=[]
    result={'passed':False,'failures':[],'phases':{},'binary':str(source),
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem revival score evidence: {root}',flush=True)
    def game(name,**kwargs):
        item=runner.Game(name,args,root,library,port,**kwargs);games.append(item);return item
    try:
        server=game('server',dedicated=True,extra_arguments=('-gametype','coop','-friendlyfire','on','-lives','4'))
        server.wait(lambda text:'NET server role=dedicated' in text,'dedicated startup',args.timeout)
        clients=[]
        for index in range(2):
            client=game(f'client-{index}')
            client.wait(lambda text:'NET content ready ' in text,'join',args.timeout)
            runner.poll_status(client,lambda row:row['state']=='playing','snapshot',args.timeout)
            client.command_text('bind E "gamefunc_Open"\nbind C "gamefunc_Crouch"\nbind Q "gamefunc_Quick_Kick"')
            clients.append(client)
        def attack():
            clients[1].key(20,True)
            try:wait_row(server,'NET lives ',lambda row:row['down']=='1','native radius attack downs player0',args.timeout,0)
            finally:clients[1].key(20,False);clients[1].input_barrier()
            time.sleep(1)
            require(latest(server,'NET lives ',1)['down']=='0','radius emitter hit itself')
        attack()
        spend_life(clients[0],server,0,4,args.timeout)
        credited=wait_row(server,'NET score ',lambda row:row['total']=='1','native attacker receives first frag',args.timeout,1)
        result['phases']['native-attribution']=credited
        time.sleep(.2)
        clients[0].key(8,True);time.sleep(.3);clients[0].key(8,False)
        wait_row(server,'NET player ',lambda row:int(row['health'])>1,'victim respawn',args.timeout,0)
        attack()
        clients[1].key(8,True)
        try:wait_row(server,'NET lives ',lambda row:row['down']=='0','attacker revives victim',args.timeout,0)
        finally:clients[1].key(8,False);clients[1].input_barrier()
        require(latest(server,'NET player ',0)['health']=='50','revive health was not50')
        time.sleep(1)
        down(clients[0],server,0,args.timeout)
        spend_life(clients[0],server,0,3,args.timeout)
        suicide=wait_row(server,'NET score ',lambda row:row['total']=='-1','self-kill penalty after revival',args.timeout,0)
        attacker=latest(server,'NET score ',1)
        require(attacker['total']=='1',f'old attacker was incorrectly credited again: {attacker}')
        result['phases']['revived-self-kill']={'victim':suicide,'previous-attacker':attacker}
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
