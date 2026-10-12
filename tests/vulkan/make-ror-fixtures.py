#!/usr/bin/env python3
"""Generate original Blood room-over-room scenes and destination-only controls.

Each scene has separate, disconnected XY rooms joined with Blood's upper/lower
stack markers. A control changes only material in the room beyond the portal;
its visible difference therefore proves the destination was actually rendered.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib


spec = importlib.util.spec_from_file_location('fixtures', Path(__file__).with_name('make-fixtures.py'))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)

DESTINATION_TILE = 28006
ROOM_OFFSET = 16384


def marker(upper: bool, sector: int, link: int) -> bytes:
    """Vanilla 56-byte XSPRITE, with only its data1 link number populated."""
    # Field widths before data1 in db.cpp::dbLoadMap's XSPRITE reader.
    widths = (14, 1, 17, 10, 10, 8, 1, 1, 2, 12, 12, 1, 1, 2, 2, 1, 1,
              8, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 2, 5, 1, 1, 1, 1)
    data1_bit = sum(widths)
    assert data1_bit == 128
    extra = bytearray(56)
    struct.pack_into('<h', extra, data1_bit // 8, link)
    sprite = fixtures.SPRITE.pack(
        sector * ROOM_OFFSET + 4096, 4096, 0 if upper else -12288,
        32768, 0, 0, 0, 32, 0, 64, 64, 0, 0,
        sector, 0, 0, -1, 0, 0, 0, 11 if upper else 12, 0, 1)
    return sprite + extra


def ror_map(*, ceiling=False, colored=False, levels=2) -> bytes:
    assert levels in (2, 3)
    start_sector = levels - 1 if ceiling else 0
    destination_sector = 0 if ceiling else levels - 1
    sprites = []
    for sector in range(levels - 1):
        sprites += [marker(True, sector, sector + 1), marker(False, sector + 1, sector + 1)]
    data = bytearray(b'BLM\x1a' + struct.pack('<H', 0x600))
    data += fixtures.MAP_HEADER.pack(start_sector * ROOM_OFFSET + 1024, 4096, -4096,
                                     0, start_sector, 0, 32, 0, 0, 1,
                                     levels, 4 * levels, len(sprites))
    data += struct.pack('<h', 0)
    for sector in range(levels):
        ceiling_tile = 504 if sector > 0 else fixtures.CEILING_TILE
        floor_tile = 504 if sector < levels - 1 else fixtures.FLOOR_TILE
        if colored and sector == destination_sector:
            if ceiling_tile != 504:
                ceiling_tile = DESTINATION_TILE
            if floor_tile != 504:
                floor_tile = DESTINATION_TILE
        data += fixtures.SECTOR.pack(sector * 4, 4, -12288, 0, 0, 0,
                                     ceiling_tile, 0, 4, 0, 0, 0,
                                     floor_tile, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1)
    for sector in range(levels):
        tile = DESTINATION_TILE if colored and sector == destination_sector else fixtures.WALL_TILE
        coordinates = ((0, 0), (8192, 0), (8192, 8192), (0, 8192))
        for index, (x, y) in enumerate(coordinates):
            data += fixtures.WALL.pack(x + sector * ROOM_OFFSET, y,
                                       sector * 4 + (index + 1) % 4, -1, -1,
                                       0, tile, 0, index * 2, 0, 32, 32, 0, 0, 0, 0, -1)
    data += b''.join(sprites)
    data += struct.pack('<I', zlib.crc32(data))
    validate_map(data, levels, ceiling)
    return bytes(data)


def validate_map(data: bytes, levels: int, ceiling: bool) -> None:
    header = fixtures.MAP_HEADER.unpack_from(data, 6)
    assert header[-3:] == (levels, levels * 4, (levels - 1) * 2)
    assert header[4] == (levels - 1 if ceiling else 0)
    assert len(data) == 45 + 40 * levels + 32 * 4 * levels + (44 + 56) * (levels - 1) * 2 + 4
    assert zlib.crc32(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
    for sector in range(levels):
        packed = fixtures.SECTOR.unpack_from(data, 45 + sector * 40)
        assert packed[:2] == (sector * 4, 4) and packed[-1] == -1
        assert packed[6] == 504 if sector > 0 else packed[6] != 504
        assert packed[12] == 504 if sector < levels - 1 else packed[12] != 504
        for wall in range(4):
            packed = fixtures.WALL.unpack_from(data, 45 + levels * 40 + (sector * 4 + wall) * 32)
            assert packed[2] == sector * 4 + (wall + 1) % 4
            assert packed[3:5] == (-1, -1) and packed[-1] == -1
    offset = 45 + levels * (40 + 4 * 32)
    for index in range((levels - 1) * 2):
        sprite = fixtures.SPRITE.unpack_from(data, offset + index * 100)
        assert sprite[13] == (index + 1) // 2 and sprite[20] == (12 if index & 1 else 11)
        assert sprite[-1] == 1
        assert struct.unpack_from('<h', data, offset + index * 100 + 44 + 16)[0] == index // 2 + 1


def generate_suite(directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    definitions = ['include "blood.def"']
    for tile, kind in ((fixtures.WALL_TILE, 'wall'), (fixtures.FLOOR_TILE, 'floor'),
                       (fixtures.CEILING_TILE, 'ceiling')):
        name = f'ror-{kind}.png'
        fixtures.write_new_or_identical(directory / name, fixtures.pattern_png(kind))
        definitions += [f'tilefromtexture {tile} {{ file "{name}" }}',
                        f'texture {tile} {{ pal 0 {{ file "{name}" }} }}']
    fixtures.write_new_or_identical(directory / 'ror-destination.png', fixtures.checker_png())
    definitions += [f'tilefromtexture {DESTINATION_TILE} {{ file "ror-destination.png" }}',
                    f'texture {DESTINATION_TILE} {{ pal 0 {{ file "ror-destination.png" }} }}']
    fixtures.write_new_or_identical(directory / 'ror.def', ('\n'.join(definitions) + '\n').encode())
    scenes = {}
    effects = []
    for name, ceiling, levels in (('VRFLOOR', False, 2), ('VRCEIL', True, 2),
                                  ('VRDEEP', False, 3), ('VRHIGH', True, 3)):
        baseline = name + '0'
        scenes[baseline] = ror_map(ceiling=ceiling, levels=levels)
        scenes[name] = ror_map(ceiling=ceiling, colored=True, levels=levels)
        effects.append({'baseline': baseline, 'modified': name, 'minimum_mean_error': 0.1,
                        'feature': f'{levels}-level {"ceiling" if ceiling else "floor"} ROR destination visibility'})
    for name, data in scenes.items():
        fixtures.write_new_or_identical(directory / f'{name}.MAP', data)
    manifest = {'maps': list(scenes), 'defs': 'ror.def', 'effect_checks': effects,
                'features': {name: ['Blood stack-marker room-over-room', 'disconnected destination room']
                             for name in scenes},
                'license': 'Original generated fixture data; no copied game assets.',
                'map_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in scenes.items()}}
    fixtures.write_new_or_identical(directory / 'fixtures.json', (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = generate_suite(args.output.resolve())
    print(f'Generated {len(result["maps"])} original ROR fixtures in {args.output.resolve()}')
