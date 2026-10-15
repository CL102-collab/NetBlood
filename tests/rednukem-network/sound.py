#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real CON soundonce/ifnosounds, stop, dedicated loop baseline and SDL playback."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
import time
import wave
from run import ROOT,Game,Failure,require,records,generate,build_driver,poll_status


def status(game):
    game.command_text('net_audiostatus')
    return records(game.text(),'NET audio ')[-1]


def sounds(game):
    game.command_text('net_audiostatus')
    result={}
    for row in records(game.text(),'NET audio_sound '):result[int(row['sound'])]=row
    return result


def wait_sound(game,number,predicate,description,timeout):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        row=sounds(game).get(number,{})
        if row and predicate(row):return row
        time.sleep(.2)
    raise Failure(f'{game.name}: {description}: {row}')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'rednukem')
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--grp',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--timeout',type=float,default=45)
    parser.add_argument('--renderer',type=int,default=3)
    args=parser.parse_args()
    args.assets=args.assets.resolve();args.grp=(args.grp or next(p for p in args.assets.iterdir() if p.name.lower()=='duke3d.grp')).resolve()
    root=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='rednukem-sound-'))
    if args.output:root.mkdir(parents=True,exist_ok=False)
    source=args.binary.resolve();args.binary=root/'rednukem-tested';shutil.copy2(source,args.binary)
    generate(root/'fixture')
    with wave.open(str(root/'fixture'/'tone.wav'),'wb') as audio:
        audio.setparams((1,2,8000,4000,'NONE','not compressed'))
        audio.writeframes(b''.join(struct.pack('<h',int(math.sin(i*2*math.pi*220/8000)*6000)) for i in range(4000)))
    # Original WAV lumps in an RTS WAD exercise ordinary Alt+Fn voice taunts.
    sample=(root/'fixture'/'tone.wav').read_bytes()
    body=b''.join(sample for _ in range(11))
    directory=b''.join(struct.pack('<II8s',12+i*len(sample),len(sample),f'TAUNT{i}'.encode()) for i in range(11))
    (root/'fixture'/'DUKE.RTS').write_bytes(b'IWAD'+struct.pack('<II',11,12+len(body))+body+directory)
    with (root/'fixture'/'fixture.con').open('a') as con:
        con.write('definesound 100 tone.wav 0 0 128 1 0\n'
                  'definesound 101 tone.wav 0 0 128 0 0\n'
                  'definesound 102 tone.wav 0 0 128 128 0\n'
                  'action QUIET 0 1 1 1 1\n'
                  'actor 1405 100\n'
                  'ifp 8 action QUIET\n'
                  'ifaction QUIET { stopsound 100 ifnosounds soundonce 101 }\n'
                  'else soundonce 100\n'
                  'ifp 32 { action 0 soundonce 102 }\n'
                  'enda\n')
    library=build_driver(root)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    games=[];result={'passed':False,'failures':[],'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    print(f'Rednukem sound evidence: {root}',flush=True)
    def game(name,**kwargs):
        item=Game(name,args,root,library,port,**kwargs);games.append(item);return item
    try:
        server=game('server',dedicated=True)
        server.wait(lambda text:'NET server role=dedicated' in text,'server',args.timeout)
        client=game('client-0',audio=True,autoexec_extra='bind C "gamefunc_Crouch"\nbind SPACE "gamefunc_Jump"\n')
        poll_status(client,lambda row:row['state']=='playing','snapshot',args.timeout)
        first=wait_sound(server,100,lambda row:row['started']=='1','logical loop starts once',args.timeout)
        local=wait_sound(client,100,lambda row:int(row['played'])==1 and int(row['local_voices'])==1,'SDL loop playback',args.timeout)
        client.command_text('net_record loop.rnpd')
        time.sleep(1.5)
        client.command_text('net_stoprecord')
        require(sounds(server)[100]['started']=='1','soundonce restarted a live loop every server tick')
        late=game('client-1',audio=True)
        poll_status(late,lambda row:row['state']=='playing','late snapshot',args.timeout)
        baseline=wait_sound(late,100,lambda row:int(row['played'])==1 and int(row['local_voices'])==1,'hotjoin loop baseline',args.timeout)
        require(sounds(server)[100]['started']=='1','hotjoin restarted the existing loop')
        result['loop']={'server':first,'client':local,'hotjoin':baseline}
        for key in (226,60):client.key(key,True) # Left Alt + F3.
        client.input_barrier();time.sleep(.15)
        for key in (60,226):client.key(key,False)
        client.input_barrier();time.sleep(.4)
        require(status(server)['taunts']=='1','Alt+F3 did not reach validated server taunt route')
        require(status(late)['taunts']=='1','teammate did not receive voice taunt')
        require(status(client)['taunts']=='0','voice taunt echoed back to its originating client')
        result['taunt']=True
        late.stop();poll_status(server,lambda row:row['players']=='1','late client departure',args.timeout)
        client.key(6,True);client.input_barrier()
        wait_sound(server,101,lambda row:int(row['started'])>=3,'headless ifnosounds one-shot expiry',args.timeout)
        values=sounds(server)
        require(status(server)['loops']=='0','stopped loop remained in logical sound state')
        played=sounds(client)
        require(int(values[101]['started'])<20,'one-shot expired every tick instead of its sample duration')
        require(played[100]['local_voices']=='0','stop event did not stop local loop')
        require(int(played[101]['played'])>=2,'one-shot events did not play on SDL dummy device')
        result['oneshot']={'server':values[101],'client':played[101]}
        client.key(6,False);client.input_barrier()
        # Duke-tag sound delegates spatial playback to global playback. Exactly
        # one server event must result, despite those nested sound API calls.
        client.key(44,True);client.input_barrier();time.sleep(.3);client.key(44,False);client.input_barrier()
        wait_sound(client,100,lambda row:int(row['played'])>=2 and int(row['local_voices'])==1,'loop restart after stop',args.timeout)
        time.sleep(1.5)
        tagged=wait_sound(client,102,lambda row:int(row['played'])>=1,'Duke-tag global playback',args.timeout)
        source=sounds(server)[102]
        require(tagged['received']==source['started'] and tagged['played']==source['started'],'nested Duke-tag call duplicated sound events')
        result['duketag']={'server':source,'client':tagged}
        replay=Game('packet-playback',args,root,library,playback=True,audio=True);games.append(replay)
        replay.wait(lambda text:'E1L1: NETWORK ROOM ONE' in text,'offline startup',args.timeout)
        replay.command_text(f'net_playback "{client.path / "loop.rnpd"}"')
        replay.wait(lambda text:'NET playback finished' in text,'packet playback',args.timeout)
        result['demo_loop']=wait_sound(replay,100,lambda row:int(row['played'])==1 and int(row['local_voices'])==1,
                                      'recording started mid-loop must seed playback sound',args.timeout)
        replay.stop();client.stop();server.stop()
        # A listen host plays locally while it publishes. DTAG delegation and
        # device replacement must not publish another copy through nested APIs.
        host=game('listen-host',audio=True,autoexec_extra='bind SPACE "gamefunc_Jump"\n',
                  launch_arguments=['-server','8','-port',str(port),'-v1','-l1','-s1'])
        host.wait(lambda text:'NET server role=listen' in text,'listen startup',args.timeout)
        peer=game('listen-client',audio=True)
        poll_status(peer,lambda row:row['state']=='playing','listen snapshot',args.timeout)
        wait_sound(peer,100,lambda row:int(row['played'])==1,'listen loop baseline',args.timeout)
        require(sounds(host)[100]['started']=='1','listen host local loop republished recursively')
        host.key(44,True);host.input_barrier();time.sleep(.3);host.key(44,False);host.input_barrier();time.sleep(1.5)
        received=wait_sound(peer,102,lambda row:int(row['played'])>=1,'listen Duke-tag sound',args.timeout)
        sent=sounds(host)[102]
        require(received['received']==sent['started'] and received['played']==sent['started'],
                'listen-host nested sound calls duplicated network events')
        result['listen_duketag']={'server':sent,'client':received}
        require('NET sound unavailable' not in server.text(),'fixture sound duration was unavailable')
        result['passed']=True
    except (Failure,OSError,KeyError,IndexError) as error:result['failures'].append(str(error))
    finally:
        for item in reversed(games):item.stop()
        for item in games:
            if item.process.returncode!=0 or item.forced:result['passed']=False;result['failures'].append(item.name+': unclean shutdown')
        result['games']={item.name:item.result() for item in games}
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(('PASS' if result['passed'] else 'FAIL')+f': {root/"result.json"}',flush=True)
    for failure in result['failures']:print(failure,file=sys.stderr)
    return 0 if result['passed'] else 1

if __name__=='__main__':sys.exit(main())
