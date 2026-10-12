// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"
#include "../../build/src/engine_priv.h"
#include "common_game.h"
#include "crc32.h"
#include "globals.h"
#include "resource.h"
#include "sound.h"
#include "tile.h"
#include "netcontent.h"
#include "netprotocol.h"
#include <algorithm>
#include <string>
#include <vector>

extern char *pUserRFF, *pUserSoundRFF, *pUserTiles;

namespace {
void addNumber(uint32_t& crc, uint32_t value)
{
    uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    crc = Bcrc32(bytes, sizeof(bytes), crc);
}
void addText(uint32_t& crc, const char* text)
{
    addNumber(crc, uint32_t(Bstrlen(text)));
    crc = Bcrc32(text, Bstrlen(text), crc);
}
void addFile(uint32_t& crc, const char* path)
{
    const auto file = kopen4loadfrommod(path, 0);
    if (file == buildvfs_kfd_invalid) { addNumber(crc, 0); return; }
    addNumber(crc, 1);
    addNumber(crc, kfilelength(file));
    uint8_t buffer[65536];
    int count;
    while ((count = kread(file, buffer, sizeof(buffer))) > 0)
        crc = Bcrc32(buffer, count, crc);
    kclose(file);
}
void addOverrides(uint32_t& crc, Resource& resources)
{
    std::vector<DICTNODE*> nodes;
    for (unsigned i = 0; i < resources.count; ++i)
    {
        auto* node = &resources.dict[i];
        if (!node->name || !node->type || !(node->flags & (DICT_EXTERNAL | DICT_BUFFER))) continue;
        // Maps are identified separately at each level change. Only resources
        // that affect simulation or event interpretation belong in this set.
        if (!Bstrcasecmp(node->type, "QAV") || !Bstrcasecmp(node->type, "SEQ") ||
            !Bstrcasecmp(node->type, "DAT") || !Bstrcasecmp(node->type, "SFX")) nodes.push_back(node);
    }
    std::sort(nodes.begin(), nodes.end(), [](const DICTNODE* a, const DICTNODE* b) {
        int order = Bstrcasecmp(a->type, b->type);
        if (!order) order = Bstrcasecmp(a->name, b->name);
        return order ? order < 0 : a->id < b->id;
    });
    addNumber(crc, nodes.size());
    for (auto* node : nodes)
    {
        std::string name = std::string(node->name) + "." + node->type;
        for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        addText(crc, name.c_str()); addNumber(crc, node->id); addNumber(crc, node->size);
        std::vector<uint8_t> original(node->size);
        // Cached QAVs contain mutable drawing fields. Read original resource
        // bytes instead, without disturbing cache locks or gameplay state.
        if (node->size) { resources.Read(node, original.data()); crc = Bcrc32(original.data(), original.size(), crc); }
    }
}
}

uint32_t netContentCRC()
{
    static bool initialized = false;
    static uint32_t fingerprint = 0;
    if (initialized) return fingerprint;
    uint32_t crc = 0;
    addText(crc, "NBlood authoritative content v1");
    addNumber(crc, netwire::Version);
    addFile(crc, pUserRFF ? pUserRFF : "BLOOD.RFF");
    addFile(crc, pUserSoundRFF ? pUserSoundRFF : "SOUNDS.RFF");
    for (unsigned index = 0; index < MAXARTFILES_BASE; ++index)
    {
        char path[BMAX_PATH];
        Bsnprintf(path, sizeof(path), "%s%03u.ART", pUserTiles ? pUserTiles : "TILES", index);
        addNumber(crc, index); addFile(crc, path);
    }
    addOverrides(crc, gSysRes); addOverrides(crc, gSoundRes);
    // Include effective collision/animation metadata after ART/DEF overrides.
    for (int i = 0; i < MAXTILES; ++i)
    {
        addNumber(crc, uint16_t(tilesiz[i].x)); addNumber(crc, uint16_t(tilesiz[i].y));
        addNumber(crc, picanm[i].num); addNumber(crc, uint8_t(picanm[i].xofs));
        addNumber(crc, uint8_t(picanm[i].yofs)); addNumber(crc, picanm[i].sf);
    }
    crc = Bcrc32(surfType, sizeof(surfType), crc);
    fingerprint = crc; initialized = true;
    return fingerprint;
}
