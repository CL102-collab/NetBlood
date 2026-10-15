"""Original, reproducible Duke Build maps and minimal CON for network checks."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import struct
import zlib

SECTOR = struct.Struct('<hhiiHHhhbBBBhhbBBBBBhhh')
WALL = struct.Struct('<iihhhHhhbBBBBBhhh')
SPRITE = struct.Struct('<iiiHhbBBBBBbb10h')


def room_map(second=False):
    """One original room with eight player starts; no monsters or random actors."""
    length = 10240 if second else 8192
    points = ((0, 0), (length, 0), (length, 8192), (0, 8192))
    data = bytearray(struct.pack('<iiiihhH', 7, 1024, 4096, -10240, 0, 0, 1))
    data += SECTOR.pack(0, 4, -16384, 0, 0, 0, 80, 0, 8, 0, 0, 0,
                        89, 0, 8, 0, 0, 0, 0, 0, 0, 0, -1)
    data += struct.pack('<H', 4)
    for index, (x, y) in enumerate(points):
        data += WALL.pack(x, y, (index + 1) % 4, -1, -1, 0,
                          1, 0, 8, 0, 16, 16, 0, 0, 0, 0, -1)
    data += struct.pack('<H', 8)
    for index in range(8):
        data += SPRITE.pack(1024, 1024 + 768 * index, 0, 0, 1405,
                            0, 0, 32, 0, 32, 32, 0, 0,
                            0, 0, 0, -1, 0, 0, 0, 0, 0, -1)
    return bytes(data)


def generate(directory):
    directory = Path(directory)
    directory.mkdir(parents=True)
    maps = {'RNET1.MAP': room_map(), 'RNET2.MAP': room_map(True)}
    for name, data in maps.items():
        (directory / name).write_bytes(data)
    script = ('definevolumename 0 NETWORK FIXTURE\n'
              'defineskillname 0 FIXTURE\n'
              'definelevelname 0 0 RNET1.MAP 00:00 00:00 NETWORK ROOM ONE\n'
              'definelevelname 0 1 RNET2.MAP 00:00 00:00 NETWORK ROOM TWO\n'
              'gamestartup 512 10 100 0 768 768 53200 176 1780 2500 650 3880 2500 2500 2048 '
              '200 50 200 50 50 50 99 10 99 50 1 3 0 64 0\n')
    (directory / 'fixture.con').write_text(script)
    # Explicitly select our original map pack as group0. Duke shareware assets
    # can then supply art without restricting the generated maps to E1L1 art.
    group = b'KenSilverman' + struct.pack('<I', len(maps))
    group += b''.join(struct.pack('<12sI', name.encode(), len(data)) for name, data in maps.items())
    group += b''.join(maps.values())
    (directory / 'fixture.grp').write_bytes(group)
    (directory / 'fixture.grpinfo').write_text(
        f'grpinfo {{ name "Original network fixture" crc {zlib.crc32(group)} '
        f'size {len(group)} flags GAMEFLAG_DUKE dependency 0 }}\n')
    metadata = {'license': 'Original generated map and CON; no copied game assets.',
                'maps': {name: hashlib.sha256(data).hexdigest() for name, data in maps.items()}}
    (directory / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata
