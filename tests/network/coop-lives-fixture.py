#!/usr/bin/env python3
"""Legal Blood 1.21 map for co-op deaths and complete level reset checks.

The eight indexed single-player/co-op starts face east on a safe platform.
Walking forward enters a fatal pit; walking backward as slot 0 collects a
shotgun, shells, skull key, armor and TNT, well away from the spawn point. Farther
west, a movement-blocking but shoot-through portal cages a stationary green
tentacle. Its short-range attack cannot reach the player on the safe platform.

Suggested controls for slot 0: hold S until x is approximately -6500, release,
use TurnAround, then hold Fire with the shotgun to kill the caged tentacle.
TurnAround again restores the eastward heading for the ordinary W death route.
Stop movement by observing authoritative positions rather than a fixed delay.
Players in other slots can walk directly into the pit without touching items.
Slot 1 starts 512 units south of slot 0, close enough to assist a downed player.
For a safe-platform self-down, collect the TNT, return to slot 0's start, select
the TNT weapon and hold Fire until its fuse expires. Slot 1 can approach and
use Open to revive; friendly fire is unnecessary for the self-inflicted blast.

No cheats, modern-map extensions, triggers, respawners or custom assets are used.
The pickups and monster must be restored by loading the map, not by spawning a
replacement player. Items are not at spawn, so fresh inventory stays observable.
"""

import struct
import zlib


STARTS = ((-2048, -7168, 0), (-2048, -6656, 0)) + tuple(
    (x, y, 0) for x in (-3072, -4096) for y in (-7680, -7168, -6656))
PICKUP_POSITION = (-5120, -7168, 0)
MONSTER_POSITION = (-10240, -7168, 0)
PICKUP_TYPES = (41, 68, 100, 140, 63)  # Sawed-off, shells, skull key, armor, TNT box.
MONSTER_TYPE = 222  # Green tentacle: stationary, with a short-range attack.


def _u16(data, offset, value):
    struct.pack_into("<H", data, offset, value & 0xffff)


def _u32(data, offset, value):
    struct.pack_into("<I", data, offset, value & 0xffffffff)


def _sprite(position, sector, status, kind, tile=0, size=64, angle=0, extra=-1):
    data = bytearray(44)
    for offset, value in zip((0, 4, 8), position):
        _u32(data, offset, value)
    # Blood's physics/autoaim GetSpriteExtents assumes centered face sprites.
    # Match dbInsertSprite/map-editor defaults so Build's hitscan does too.
    _u16(data, 12, 128)  # CSTAT_SPRITE_YCENTER.
    _u16(data, 14, tile)
    data[20:22] = bytes((size, size))
    _u16(data, 24, sector)
    _u16(data, 26, status)
    _u16(data, 28, angle)
    _u16(data, 30, -1)  # No owner.
    _u16(data, 38, kind)
    _u16(data, 42, extra)
    return data


def _extended(reference, data1=0):
    data = bytearray(56)
    _u16(data, 0, reference)  # Loader replaces the 14-bit reference.
    _u16(data, 16, data1)
    _u16(data, 30, -1)  # No AI target.
    _u16(data, 46, -1)  # No burn source.
    return data


def lives_map(revision=0):
    """Return BLM6 bytes with 3 sectors, 12 walls, 8 starts, 5 items, 1 enemy.

    Platform: x=-8192..0, y=-8192..-6144, floor z=0, ceiling z=-32768.
    East pit: x=0..32768, same y range, floor z=262144.
    Tentacle room: x=-12288..-8192, same y range, floor z=0.
    Wall pair 3/9 prevents movement across the west portal without obstructing
    sight or hitscan weapons. The monster's starting angle is 1024 (west).
    """
    data = bytearray(6 + 37 + 2)  # Header and one sky offset (skybits=0).
    data[:4] = b"BLM\x1a"
    _u16(data, 4, 0x600)
    for offset, value in zip((6, 10, 14), STARTS[0]):
        _u32(data, offset, value)
    _u32(data, 6 + 0x1b, revision)
    _u16(data, 6 + 0x1f, 3)
    _u16(data, 6 + 0x21, 12)
    _u16(data, 6 + 0x23, 14)

    for index, floor_z in enumerate((0, 262144, 0)):
        sector = bytearray(40)
        _u16(sector, 0, index * 4)
        _u16(sector, 2, 4)
        _u32(sector, 4, -32768)
        _u32(sector, 8, floor_z)
        _u16(sector, 38, -1)
        data += sector

    vertices = ((-8192, -8192), (0, -8192), (0, -6144), (-8192, -6144),
                (0, -8192), (32768, -8192), (32768, -6144), (0, -6144),
                (-12288, -8192), (-8192, -8192), (-8192, -6144), (-12288, -6144))
    portals = {1: (7, 1), 7: (1, 0), 3: (9, 2), 9: (3, 0)}
    for index, (x, y) in enumerate(vertices):
        wall = bytearray(32)
        _u32(wall, 0, x)
        _u32(wall, 4, y)
        _u16(wall, 8, (index // 4) * 4 + ((index + 1) % 4))
        next_wall, next_sector = portals.get(index, (-1, -1))
        _u16(wall, 10, next_wall)
        _u16(wall, 12, next_sector)
        _u16(wall, 14, 1 if index in (3, 9) else 0)  # Block actors, not shots.
        _u16(wall, 18, -1)
        wall[22:24] = bytes((8, 8))
        _u16(wall, 30, -1)
        data += wall

    for slot, position in enumerate(STARTS):
        # warpInit consumes type 1 for co-op, indexed by XSPRITE.data1. Type 2
        # would work only in bloodbath/teams and leave co-op starts overlapping.
        data += _sprite(position, 0, 0, 1, extra=slot + 1)
        data += _extended(slot, slot)

    for kind, tile, size in ((41, 559, 48), (68, 812, 48),
                             (100, 2552, 32), (140, 2628, 64), (63, 809, 48)):
        data += _sprite(PICKUP_POSITION, 0, 3, kind, tile, size)

    data += _sprite(MONSTER_POSITION, 2, 6, MONSTER_TYPE, angle=1024, extra=9)
    data += _extended(13)
    return bytes(data) + struct.pack("<I", zlib.crc32(data))
