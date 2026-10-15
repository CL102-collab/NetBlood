// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rnet { namespace content {
constexpr size_t MaxMapBytes = 16u * 1024u * 1024u;
// Validate every count/index used by the binary Build v7/v8/v9 parser before it can
// mutate engine arrays. Downloaded maps never reach loadboard unchecked.
inline bool validateMap(const std::vector<uint8_t>& bytes, std::string& error)
{
    auto fail = [&](const char* why) { error = why; return false; };
    if (bytes.size() < 26 || bytes.size() > MaxMapBytes)
        return fail("Map size is invalid");
    auto word = [&](size_t p) { return unsigned(bytes[p]) | (unsigned(bytes[p+1]) << 8); };
    auto signedWord = [&](size_t p) { return int16_t(word(p)); };
    unsigned const version=word(0);
    if (version<7 || version>9 || word(2) != 0) return fail("Multiplayer requires a binary Build version 7, 8, or 9 map");
    unsigned const maxSectors=version==7?1024:4096, maxWalls=version==7?8192:16384, maxSprites=version==7?4096:16384;
    unsigned const sectors = word(20);
    if (!sectors || sectors > maxSectors || 22u + size_t(sectors)*40u + 2u > bytes.size())
        return fail("Map sector count is invalid");
    size_t const wallCountOffset = 22 + size_t(sectors)*40;
    unsigned const walls = word(wallCountOffset);
    size_t const wallStart = wallCountOffset + 2;
    if (!walls || walls > maxWalls || wallStart + size_t(walls)*32u + 2u > bytes.size())
        return fail("Map wall count is invalid");
    size_t const spriteCountOffset = wallStart + size_t(walls)*32;
    unsigned const sprites = word(spriteCountOffset);
    size_t const spriteStart = spriteCountOffset + 2;
    if (sprites > maxSprites || spriteStart + size_t(sprites)*44u != bytes.size())
        return fail("Map sprite count or trailing data is invalid");
    if (signedWord(18) < 0 || word(18) >= sectors) return fail("Map start sector is invalid");
    std::vector<int> owner(walls, -1);
    bool bunches[2][256]{};
    for (unsigned i=0; i<sectors; ++i)
    {
        size_t const p = 22 + size_t(i)*40;
        unsigned const first = word(p), count = word(p+2);
        if (count < 3 || first >= walls || count > walls-first)
            return fail("Map sector wall range is invalid");
        if (word(p+16) >= 30720 || word(p+24) >= 30720)
            return fail("Map sector tile is invalid");
        if(version==9)for(unsigned cf=0;cf<2;++cf)
            if(word(p+12+cf*2)&1024)bunches[cf][bytes[p+22+cf*8]]=true;
        for (unsigned j=first; j<first+count; ++j)
        {
            if (owner[j] != -1) return fail("Map sector wall ranges overlap");
            owner[j] = int(i);
        }
    }
    if(version==9)for(unsigned bunch=0;bunch<256;++bunch)
        if(bunches[0][bunch]!=bunches[1][bunch])return fail("Map TROR bunch has no matching ceiling or floor");
    std::vector<unsigned> incoming(walls,0);
    for (unsigned i=0; i<walls; ++i)
    {
        size_t const p = wallStart + size_t(i)*32;
        unsigned const point = word(p+8);
        int const nextWall = signedWord(p+10), nextSector = signedWord(p+12);
        if (owner[i] < 0 || point >= walls || owner[point] != owner[i])
            return fail("Map wall loop is invalid");
        if (++incoming[point] != 1) return fail("Map wall loops merge or fail to close");
        if ((nextWall == -1) != (nextSector == -1) || nextWall < -1 || nextSector < -1
            || nextWall >= int(walls) || nextSector >= int(sectors))
            return fail("Map portal index is invalid");
        if (nextWall >= 0 && (owner[nextWall] != nextSector
            || signedWord(wallStart + size_t(nextWall)*32+10) != int(i)))
            return fail("Map portal is not reciprocal");
        if (word(p+16) >= 30720 || word(p+18) >= 30720) return fail("Map wall tile is invalid");
        if(version==9)for(unsigned cf=0;cf<2;++cf)if(word(p+14)&(1024u<<cf))
        {
            int const link=signedWord(p+26+cf*4);
            if(link<0||link>=int(walls)||link==int(i)||owner[link]<0)return fail("Map TROR wall index is invalid");
            size_t const other=wallStart+size_t(link)*32;
            unsigned const opposite=1-cf;
            if(!(word(other+14)&(1024u<<opposite))||signedWord(other+26+opposite*4)!=int(i))
                return fail("Map TROR wall link is not reciprocal");
            size_t const sector=22+size_t(owner[i])*40,otherSector=22+size_t(owner[link])*40;
            if(!(word(sector+12+cf*2)&1024)||!(word(otherSector+12+opposite*2)&1024)
                ||bytes[sector+22+cf*8]!=bytes[otherSector+22+opposite*8])
                return fail("Map TROR wall link crosses unrelated bunches");
        }
    }
    std::vector<bool> visited(walls,false);
    for (unsigned i=0; i<walls; ++i) if(!visited[i])
    {
        unsigned point=i,count=0;
        do {visited[point]=true;++count;point=word(wallStart+size_t(point)*32+8);} while(point!=i);
        if(count<3)return fail("Map wall loop has fewer than three points");
    }
    for (unsigned i=0; i<sprites; ++i)
    {
        size_t const p = spriteStart + size_t(i)*44;
        if (word(p+14) >= 30720 || signedWord(p+24) < 0 || word(p+24) >= sectors
            || signedWord(p+26) < 0 || word(p+26) >= 1024)
            return fail("Map sprite tile, sector, or status is invalid");
    }
    error.clear();
    return true;
}
} }
