#!/usr/bin/env python3
"""Generate original Blood maps, MD3/KVX models, and PNG rendering fixtures.

Formats follow source/build/include/mdsprite.h and the repository's loaders.
Generation checks binary layout and round-trips voxel occupancy without running
the renderer. These fixtures contain no copied game data.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


def write_new_or_identical(path: Path, data: bytes) -> None:
    path = Path(path)
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f"Preserving different existing fixture: {path}")
        return
    path.write_bytes(data)


def checker_png(size: int = 32) -> bytes:
    """Opaque magenta/cyan/yellow/blue checker, with a white orientation corner."""
    colors = ((240, 30, 180), (20, 220, 230), (245, 215, 30), (40, 60, 240))
    scanlines = bytearray()
    for y in range(size):
        scanlines.append(0)  # PNG filter none.
        for x in range(size):
            c = colors[((x // 8) + 2 * (y // 8)) % len(colors)]
            if x < 4 and y < 4:
                c = (255, 255, 255)
            scanlines.extend(c)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">2I5B", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
            + chunk(b"IEND", b""))


def pyramid_md3(skin_name: str = "fixture-checker.png") -> bytes:
    """One static frame named idle; six triangles, 18 flat-shaded vertices.

    Base is 32x32 model units at z=0, apex z=48. DEF scale=1 and repeats=64
    produce about 328 Build XY units width in the existing model transform.
    Vertex duplication gives every face independent UVs and a correct normal.
    """
    points = ((-16, -16, 0), (16, -16, 0), (16, 16, 0), (-16, 16, 0), (0, 0, 48))
    # Outward normals; positive winding viewed from outside the solid.
    faces = ((0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4), (0, 3, 2), (0, 2, 1))
    triangles = bytearray()
    uv = bytearray()
    xyzn = bytearray()
    for face_id, face in enumerate(faces):
        a, b, c = (points[i] for i in face)
        ab = tuple(b[i] - a[i] for i in range(3))
        ac = tuple(c[i] - a[i] for i in range(3))
        normal = (ab[1]*ac[2] - ab[2]*ac[1], ab[2]*ac[0] - ab[0]*ac[2], ab[0]*ac[1] - ab[1]*ac[0])
        length = math.sqrt(sum(v*v for v in normal))
        nx, ny, nz = (v / length for v in normal)
        # Engine decodes first byte as polar angle, second as azimuth, /255.
        polar = round(math.acos(max(-1.0, min(1.0, nz))) * 255 / (2*math.pi)) & 255
        azimuth = round(math.atan2(ny, nx) * 255 / (2*math.pi)) & 255
        # MD3 stores clockwise fronts: source/tools/src/md3_export.py
        # md3Triangle.Save reverses Blender's outward CCW face indices.
        triangles.extend(struct.pack("<3i", face_id*3, face_id*3+2, face_id*3+1))
        for p, st in zip((a, b, c), ((0.0, 1.0), (1.0, 1.0), (0.5, 0.0))):
            uv.extend(struct.pack("<2f", *st))
            xyzn.extend(struct.pack("<3h2B", *(int(v*64) for v in p), polar, azimuth))
    shader = struct.pack("<64si", skin_name.encode("ascii"), 0)
    ofstris = 108
    ofsshaders = ofstris + len(triangles)
    ofsuv = ofsshaders + len(shader)
    ofsxyzn = ofsuv + len(uv)
    ofsend = ofsxyzn + len(xyzn)
    surface = (struct.pack("<4s64s10i", b"IDP3", b"pyramid", 0, 1, 1, 18, 6,
                           ofstris, ofsshaders, ofsuv, ofsxyzn, ofsend)
               + triangles + shader + uv + xyzn)
    frame = struct.pack("<10f16s", -16, -16, 0, 16, 16, 48, 0, 0, 24, math.sqrt(16*16+16*16+24*24), b"idle")
    # Standard disk header is 108 bytes. The x86_64 loader reads four extra
    # alignment bytes, then seeks to these absolute offsets, so this is valid.
    ofsframes, ofstags, ofssurfs = 108, 164, 164
    end = ofssurfs + len(surface)
    header = struct.pack("<4si64s9i", b"IDP3", 15, b"fixture-pyramid", 0, 1, 0, 1, 0,
                         ofsframes, ofstags, ofssurfs, end)
    data = header + frame + surface
    validate_md3(data)
    return data


def validate_md3(data: bytes) -> dict:
    ident, version, name, flags, frames, tags, surfaces, skins, of, ot, os, end = struct.unpack_from("<4si64s9i", data)
    assert ident == b"IDP3" and version == 15 and frames == 1 and tags == 0 and surfaces == 1
    assert of == 108 and ot == os == of + 56 and end == len(data)
    assert struct.unpack_from("<10f16s", data, of)[-1].rstrip(b"\0") == b"idle"
    si, sn, sf, nf, ns, nv, nt, tri, shader, uv, xyz, se = struct.unpack_from("<4s64s10i", data, os)
    assert si == b"IDP3" and nf == 1 and ns == 1 and nv == 18 and nt == 6
    assert tri == 108 and shader == tri + nt*12 and uv == shader + ns*68
    assert xyz == uv + nv*8 and se == xyz + nv*nf*8 and os + se == end
    for i in range(nt):
        indices = struct.unpack_from("<3i", data, os + tri + i*12)
        assert all(0 <= index < nv for index in indices)
        vertices = [struct.unpack_from("<3h2B", data, os + xyz + index*8) for index in indices]
        a, b, c = vertices
        ab, ac = [b[j]-a[j] for j in range(3)], [c[j]-a[j] for j in range(3)]
        cross = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
        polar, azimuth = (a[j] * (2*math.pi/255) for j in (3, 4))
        normal = (math.sin(polar)*math.cos(azimuth), math.sin(polar)*math.sin(azimuth), math.cos(polar))
        # Clockwise disk faces have geometric cross products opposite the
        # packed outward shading normal, as in the repository's exporter.
        assert sum(cross[j]*normal[j] for j in range(3)) < 0
    for i in range(nv):
        assert all(math.isfinite(v) for v in struct.unpack_from("<2f", data, os + uv + i*8))
    return {"bytes": len(data), "frame_offset": of, "surface_offset": os,
            "surface_relative_offsets": {"triangles": tri, "shader": shader, "uv": uv, "xyzn": xyz, "end": se},
            "vertices": nv, "triangles": nt, "frame": "idle"}


def asymmetric_kvx(palette_rgb6: bytes | None = None, colors: tuple[int, ...] = (1, 2, 3, 4)) -> bytes:
    """6x5x8 solid base, offset upright and a sideways arm, one mip level.

    Optional palette is 768 RGB bytes each in 0..63. For renderers that use
    the game palette by index, pass the fixture's own palette and color indices.
    All default indices are <128 because the old loader indexes with char.
    Every occupied voxel is a separate one-voxel slab with exact face flags.
    """
    sx, sy, sz = 6, 5, 8
    assert len(colors) == 4 and all(0 <= c < 128 for c in colors)
    occupied = {}
    for x in range(sx):
        for y in range(sy):
            for z in range(sz):
                if z >= 6 or (x < 2 and y < 2) or (x < 5 and y < 2 and 2 <= z < 4):
                    occupied[x, y, z] = colors[(x + 2*y + z) % 4]
    palette = bytearray(768 if palette_rgb6 is None else palette_rgb6)
    assert len(palette) == 768 and max(palette) <= 63
    if palette_rgb6 is None:
        for index, color in zip(colors, ((60, 7, 45), (5, 55, 57), (61, 53, 7), (10, 15, 60))):
            palette[index*3:index*3+3] = bytes(color)
    # x offsets are relative to the START of the x-offset table (file offset28).
    table_bytes = (sx+1)*4 + sx*(sy+1)*2
    slabs = bytearray()
    xoffsets, xyoffsets = [], []
    for x in range(sx):
        xoffsets.append(table_bytes + len(slabs))
        xstart = len(slabs)
        for y in range(sy):
            xyoffsets.append(len(slabs) - xstart)
            for z in range(sz):
                if (x, y, z) not in occupied:
                    continue
                neighbors = ((x-1, y, z), (x+1, y, z), (x, y-1, z), (x, y+1, z), (x, y, z-1), (x, y, z+1))
                visibility = sum(1 << bit for bit, p in enumerate(neighbors) if p not in occupied)
                slabs.extend((z, 1, visibility, occupied[x, y, z]))
        xyoffsets.append(len(slabs) - xstart)
    xoffsets.append(table_bytes + len(slabs))
    mipbytes = 24 + table_bytes + len(slabs)
    data = (struct.pack("<7i", mipbytes, sx, sy, sz, sx*128, sy*128, sz*128)
            + struct.pack(f"<{len(xoffsets)}i", *xoffsets)
            + struct.pack(f"<{len(xyoffsets)}H", *xyoffsets)
            + slabs + palette)
    parsed = validate_kvx(data)
    assert parsed["occupied"] == occupied
    return data


def validate_kvx(data: bytes) -> dict:
    mipbytes, sx, sy, sz, px, py, pz = struct.unpack_from("<7i", data)
    assert len(data) == 4 + mipbytes + 768 and 0 < sz < 128
    xoff = struct.unpack_from(f"<{sx+1}i", data, 28)
    xyoff = struct.unpack_from(f"<{sx*(sy+1)}H", data, 28 + (sx+1)*4)
    assert xoff[0] == (sx+1)*4 + sx*(sy+1)*2 and 28 + xoff[-1] == 4 + mipbytes
    found = {}
    for x in range(sx):
        assert xyoff[x*(sy+1)] == 0
        assert xoff[x+1] - xoff[x] == xyoff[x*(sy+1)+sy]
        for y in range(sy):
            position = 28 + xoff[x] + xyoff[x*(sy+1)+y]
            end = 28 + xoff[x] + xyoff[x*(sy+1)+y+1]
            last_z = 0
            while position < end:
                z, count, flags = struct.unpack_from("3B", data, position)
                assert count > 0 and last_z <= z and z + count <= sz and flags < 64
                assert position + 3 + count <= end
                for dz, color in enumerate(data[position+3:position+3+count]):
                    found[x, y, z+dz] = color
                position += 3 + count
                last_z = z + count
            assert position == end
    return {"bytes": len(data), "mip_bytes": mipbytes, "dimensions": [sx, sy, sz],
            "pivot": [px/256, py/256, pz/256], "slab_data_offset": 28+xoff[0],
            "voxel_count": len(found), "occupied": found}


def write_fixtures(directory: Path, model_tile: int = 30000, voxel_tile: int = 30001,
                   *, palette_rgb6: bytes | None = None, colors: tuple[int, ...] = (1, 2, 3, 4)) -> dict:
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    files = {"fixture-pyramid.md3": pyramid_md3(), "fixture-checker.png": checker_png(),
             "fixture-asymmetric.kvx": asymmetric_kvx(palette_rgb6, colors)}
    # Keep skin BEFORE frame: defs.cpp advances modelskin if a skin follows a frame.
    definition = f'''// Generated original assets. Put this directory on the search path.
tilefromtexture {model_tile} {{ file "fixture-checker.png" }}
tilefromtexture {voxel_tile} {{ file "fixture-checker.png" }}
model "fixture-pyramid.md3" {{
    scale 1.0
    skin {{ pal 0 file "fixture-checker.png" surface 0 }}
    frame {{ name "idle" tile {model_tile} smoothduration 0 }}
}}
voxel "fixture-asymmetric.kvx" {{ tile {voxel_tile} scale 4.0 }}
'''
    files["fixture-models.def"] = definition.encode("ascii")
    for name, data in files.items():
        write_new_or_identical(directory / name, data)
    metadata = {"md3": validate_md3(files["fixture-pyramid.md3"]),
                "kvx": {k: v for k, v in validate_kvx(files["fixture-asymmetric.kvx"]).items() if k != "occupied"},
                "model_tile": model_tile, "voxel_tile": voxel_tile,
                "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, data in files.items()}}
    write_new_or_identical(directory / "fixture-models.json", (json.dumps(metadata, indent=2) + "\n").encode())
    return metadata


MAP_HEADER = struct.Struct('<iiihhhiibihhh')
SECTOR = struct.Struct('<hhiiHHhhbBBBhhbBBBBBhhh')
WALL = struct.Struct('<iihhhHhhbBBBBBhhh')
SPRITE = struct.Struct('<iiiHhbBBBBBbb10h')
assert (MAP_HEADER.size, SECTOR.size, WALL.size, SPRITE.size) == (37, 40, 32, 44)

WALL_TILE, FLOOR_TILE, CEILING_TILE, SPRITE_TILE = 28000, 28001, 28002, 28003
MODEL_TILE, VOXEL_TILE = 28004, 28005


def pattern_png(kind: str, size: int = 64) -> bytes:
    """Original RGBA patterns, including holes for masked/translucent sprites."""
    colors = {'wall': ((30, 95, 100), (155, 170, 165)),
              'floor': ((25, 35, 75), (145, 135, 55)),
              'ceiling': ((24, 27, 40), (80, 90, 115)),
              'sprite': ((220, 35, 165), (25, 225, 220))}[kind]
    scanlines = bytearray()
    for y in range(size):
        scanlines.append(0)
        for x in range(size):
            color = colors[((x // 8) + (y // 8)) & 1]
            alpha = 0 if kind == 'sprite' and abs(x-y) > 12 and abs(x+y-size+1) > 12 else 255
            scanlines.extend((*color, alpha))

    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(scanlines, 9)) + chunk(b'IEND', b''))


def map_sprite(tile, x=4096, y=4096, z=0, *, repeat=64, cstat=0, angle=0, pal=0):
    # Decoration, no XSPRITE extension or AI/trigger behavior.
    return SPRITE.pack(x, y, z, cstat, tile, 0, pal, 32, 0, repeat, repeat, 0, 0,
                       0, 0, angle, -1, 0, 0, 0, 0, 0, -1)


def room_map(sprites=(), *, slope=0, mirror=False, visibility=32):
    # Clockwise Build sector; positive X faces the wall opposite the spawn.
    coordinates = ((0, 0), (8192, 0), (8192, 8192), (0, 8192))
    data = bytearray(b'BLM\x1a' + struct.pack('<H', 0x600))
    data += MAP_HEADER.pack(1024, 4096, -4096, 0, 0, 0, visibility, 0, 0, 1, 1, 4, len(sprites))
    data += struct.pack('<h', 0)  # One sky tile offset, no encryption or extensions.
    data += SECTOR.pack(0, 4, -12288, 0, 0, 2 if slope else 0,
                        CEILING_TILE, 0, 4, 0, 0, 0,
                        FLOOR_TILE, slope, 0, 0, 0, 0, 0, 0, 0, 0, -1)
    for index, (x, y) in enumerate(coordinates):
        tile = 504 if mirror and index == 1 else WALL_TILE
        data += WALL.pack(x, y, (index + 1) % 4, -1, -1, 0, tile, 0,
                          index * 2, 0, 32, 32, 0, 0, 0, 0, -1)
    data += b''.join(sprites)
    data += struct.pack('<I', zlib.crc32(data))
    validate_map(data)
    return bytes(data)


def validate_map(data):
    assert data[:6] == b'BLM\x1a\x00\x06'
    header = MAP_HEADER.unpack_from(data, 6)
    sectors, walls, sprites = header[-3:]
    assert sectors == 1 and walls == 4
    assert len(data) == 6 + 37 + 2 + sectors * 40 + walls * 32 + sprites * 44 + 4
    assert zlib.crc32(data[:-4]) == struct.unpack_from('<I', data, len(data)-4)[0]
    offset = 6 + 37 + 2
    sector = SECTOR.unpack_from(data, offset)
    assert sector[:2] == (0, 4) and sector[-1] == -1
    for index in range(walls):
        wall = WALL.unpack_from(data, offset + 40 + index * 32)
        assert wall[2] == (index+1) % 4 and wall[3:5] == (-1, -1) and wall[-1] == -1
    for index in range(sprites):
        sprite = SPRITE.unpack_from(data, offset + 40 + walls * 32 + index * 44)
        assert sprite[13:15] == (0, 0) and sprite[-1] == -1


def generate_suite(directory):
    directory.mkdir(parents=True, exist_ok=True)
    assets = write_fixtures(directory, MODEL_TILE, VOXEL_TILE, colors=(32, 64, 96, 112))
    for kind in ('wall', 'floor', 'ceiling', 'sprite'):
        write_new_or_identical(directory / f'fixture-{kind}.png', pattern_png(kind))
    definitions = ['include "blood.def"', 'include "fixture-models.def"']
    for tile, kind in ((WALL_TILE, 'wall'), (FLOOR_TILE, 'floor'),
                       (CEILING_TILE, 'ceiling'), (SPRITE_TILE, 'sprite')):
        definitions.append(f'tilefromtexture {tile} {{ file "fixture-{kind}.png" }}')
    # Walls and masked sprites use RGBA hightiles; floor/ceiling exercise indexed
    # ART palette lookup and shade tables generated by tilefromtexture.
    for tile, kind in ((WALL_TILE, 'wall'), (SPRITE_TILE, 'sprite')):
        definitions.append(f'texture {tile} {{ pal 0 {{ file "fixture-{kind}.png" }} }}')
    write_new_or_identical(directory / 'fixtures.def', ('\n'.join(definitions) + '\n').encode())

    model = map_sprite(MODEL_TILE)
    voxel = map_sprite(VOXEL_TILE)
    transparent = [map_sprite(SPRITE_TILE, 4608, 2304, repeat=24),
                   map_sprite(SPRITE_TILE, 4096, 4096, repeat=24, cstat=2),
                   map_sprite(SPRITE_TILE, 4608, 5888, repeat=24, cstat=2 | 512),
                   map_sprite(SPRITE_TILE, 6400, 4096, repeat=24, cstat=16, angle=512),
                   map_sprite(SPRITE_TILE, 3072, 5632, -64, repeat=32, cstat=32)]
    scenes = {
        'VFEMPTY': (room_map(), ['indexed floor/ceiling', 'RGBA walls', 'HUD']),
        'VFMODEL': (room_map([model]), ['MD3 model', 'model skin']),
        'VFHACK': (room_map([model]), ['MD3 model', 'maphack rotation/pivot/position']),
        'VFVOX': (room_map([voxel]), ['KVX voxel', 'indexed voxel palette']),
        'VFSPRITE': (room_map(transparent), ['masked sprites', 'translucent sprites', 'wall/floor aligned sprites']),
        'VFSLOPE': (room_map(slope=256), ['sloped floor']),
        'VFMIRROR': (room_map([model], mirror=True), ['wall mirror', 'MD3 reflected model']),
        'VFFOG': (room_map(visibility=4096), ['distance fog/shading']),
        'VFZERO': (room_map(visibility=0), ['zero-visibility fog edge case']),
    }
    for name, (data, features) in scenes.items():
        write_new_or_identical(directory / f'{name}.MAP', data)
    write_new_or_identical(directory / 'VFHACK.mhk',
                          b'sprite 0\nangoff 384\npitch 128\nmdpivxoff 4\nmdposyoff 1024\n')
    effects = [{'baseline': 'VFEMPTY', 'modified': name, 'minimum_mean_error': 0.02, 'feature': feature}
               for name, feature in (('VFMODEL', 'model visibility'), ('VFVOX', 'voxel visibility'),
                                     ('VFSPRITE', 'sprite visibility'), ('VFSLOPE', 'slope'), ('VFFOG', 'fog'),
                                     ('VFZERO', 'zero-visibility fog'))]
    effects += [{'baseline': 'VFMODEL', 'modified': 'VFHACK', 'minimum_mean_error': 0.02, 'feature': 'maphack'},
                {'baseline': 'VFMODEL', 'modified': 'VFMIRROR', 'minimum_mean_error': 0.02, 'feature': 'mirror'}]
    manifest = {'maps': list(scenes), 'defs': 'fixtures.def',
                'features': {name: features for name, (data, features) in scenes.items()},
                'requires_maphack': ['VFHACK'], 'effect_checks': effects,
                'assets': assets, 'license': 'Original generated fixture data; no copied game assets.',
                'map_sha256': {name: hashlib.sha256(data).hexdigest() for name, (data, features) in scenes.items()}}
    write_new_or_identical(directory / 'fixtures.json', (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = generate_suite(args.output.resolve())
    print(f'Generated {len(result["maps"])} original rendering fixtures in {args.output.resolve()}')
