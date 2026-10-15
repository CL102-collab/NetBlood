#!/usr/bin/env python3
"""Real mod download, private restart/reconnect, and current-map download checks."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import tempfile
from run import Game, Failure, build_driver, generate, inspect_demo, poll_status, records, require, ROOT
from fixtures import room_map


class ContentGame(Game):
    def command_text(self, commands, timeout=10):
        if self.dedicated:
            return super().command_text(commands, timeout)
        self.command_serial += 1
        marker=f'NETWORK_TEST_COMMAND_{self.command_serial}'
        # Activated server resources cannot provide loose console scripts.
        # Type real console characters instead of invoking an exec test file.
        events='53 1\n53 0\n'
        for line in (commands+'\necho '+marker).splitlines():
            events+=''.join(f'-3 {ord(ch)}\n' for ch in line)
            events+='40 1\n40 0\n'
        events+='53 1\n53 0\n'
        os.write(self.events,events.encode())
        self.wait(lambda text:marker in text,marker,timeout)
        self.input_barrier(timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'rednukem')
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--renderer', type=int, default=3)
    args = parser.parse_args()
    args.assets=args.assets.resolve()
    args.grp=next(path for path in args.assets.iterdir() if path.name.lower()=='duke3d.grp')
    root=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-content-'))
    if args.output: root.mkdir(parents=True,exist_ok=False)
    source=args.binary.resolve(); args.binary=root/'rednukem-tested'; shutil.copy2(source,args.binary)
    generate(root/'fixture'); library=build_driver(root)
    addon_con=(root/'fixture'/'fixture.con').read_bytes().replace(b'NETWORK FIXTURE',b'DOWNLOADED NETWORK MOD').replace(b'NETWORK ROOM ONE',b'DOWNLOADED ROOM ONE')
    payload=bytes((i*13+i//17)&255 for i in range(300123))
    files={'addon.con':addon_con,'payload.bin':payload}
    addon=b'KenSilverman'+struct.pack('<I',len(files))
    addon+=b''.join(struct.pack('<12sI',name.encode(),len(data)) for name,data in files.items())
    addon+=b''.join(files.values())
    addon_path=root/'server-only-addon.grp'; addon_path.write_bytes(addon)
    map_path=root/'server-only.map';map_path.write_bytes(room_map(True))
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    games=[];result={'passed':False,'failures':[],'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem content evidence: {root}',flush=True)
    try:
        server=ContentGame('server',args,root,library,port,dedicated=True,
                    extra_arguments=['-g',str(addon_path),'-x','addon.con'])
        games.append(server)
        server.wait(lambda text:'NET server role=dedicated' in text,'server startup',args.timeout)
        client=ContentGame('client',args,root,library,port);games.append(client)
        client.wait(lambda text:'NET content ready ' in text,'download, restart, reconnect and level load',args.timeout)
        poll_status(client,lambda row:row['state']=='playing','authoritative play after restart',args.timeout)
        cache=list((client.path/'.rednukem-addon-cache').glob('*/server-only-addon.grp'))
        require(len(cache)==1 and cache[0].read_bytes()==addon,'downloaded archive differs from server bytes')
        require('E1L1: DOWNLOADED ROOM ONE' in client.text(),'downloaded CON was not activated')
        require('NET transport connected' in client.text(),'client did not reconnect')
        require('fixture-password' not in client.text(),'password was exposed in client log')
        client.command_text('net_record content.rnpd')
        initial_tick=int(server.status()['tick'])
        poll_status(server,lambda row:int(row['tick'])>=initial_tick+30,'first recorded map',args.timeout)
        before=int(server.status()['map_generation'])
        shutil.copy2(map_path,server.path/map_path.name)
        server.command_text('map '+map_path.name)
        poll_status(client,lambda row:int(row['map_generation'])>before and row['state']=='playing',
                    'map download after live rotation',args.timeout)
        maps=list((client.path/'.rednukem-addon-cache').glob('*/server-only.map'))
        require(len(maps)==1 and maps[0].read_bytes()==map_path.read_bytes(),'downloaded map differs from server bytes')
        downloaded_tick=int(server.status()['tick'])
        poll_status(server,lambda row:int(row['tick'])>=downloaded_tick+30,'recorded downloaded map',args.timeout)
        client.command_text('net_stoprecord')
        result['demo']=inspect_demo(client.path/'content.rnpd')
        result.update(addon_sha256=hashlib.sha256(addon).hexdigest(),map_sha256=hashlib.sha256(map_path.read_bytes()).hexdigest(),
                      client_status=client.status(),server_status=server.status(),cached_addon=str(cache[0]),cached_map=str(maps[0]))
        client.command_text('net_disconnect\nnet_playback content.rnpd')
        client.wait(lambda text:'NET playback finished ' in text,'cached map packet playback',args.timeout)
        client.command_text('net_status')
        playback=records(client.text(),'NET snapshot ')[-1]
        require(playback['hash']==result['demo']['last_snapshot']['hash'],'cached map replay differs from recording')
        result['playback_snapshot']=playback
        client.stop();server.stop()
        for game in games:require(game.process.returncode==0 and not game.forced,f'{game.name}: unclean shutdown')
        result['passed']=True
    except (Failure,OSError,ValueError,IndexError) as error: result['failures'].append(str(error))
    finally:
        for game in reversed(games):game.stop()
        result['games']={game.name:game.result() for game in games}
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(('PASS' if result['passed'] else 'FAIL')+': '+str(root/'result.json'),flush=True)
    for error in result['failures']:print(error,flush=True)
    return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
