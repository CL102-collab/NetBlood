// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"
#include "../../build/src/engine_priv.h"
#include "common_game.h"
#include "crc32.h"
#include "globals.h"
#include "levels.h"
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
bool contentFrozen = false;
std::vector<netassets::Asset> contentManifest;

std::string canonicalName(std::string name)
{
    for (auto& ch : name)
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    return name;
}

// Expose a useful game-relative filename, never an absolute server install path
// or a path that could be interpreted as a console escape by the receiving UI.
std::string displayName(const char* path)
{
    std::string name = path ? path : "";
    std::replace(name.begin(), name.end(), '\\', '/');
    while (name.compare(0, 2, "./") == 0) name.erase(0, 2);
    if ((!name.empty() && name[0] == '/') || name.find(':') != std::string::npos ||
        name.compare(0, 3, "../") == 0 || name.find("/../") != std::string::npos)
        name.erase(0, name.find_last_of('/') + 1);
    for (auto& ch : name)
        if (uint8_t(ch) < 32 || uint8_t(ch) == 127) ch = '?';
    return name;
}

bool cinematicVideoType(const char* type)
{
    return type && (!Bstrcasecmp(type, "SMK") || !Bstrcasecmp(type, "OGV"));
}

bool sameCinematicFile(const char* path, const std::string& declared)
{
    if (!path || !*path || declared.empty()) return false;
    auto name = canonicalName(displayName(path));
    auto expected = canonicalName(displayName(declared.c_str()));
    // Movie lookup also searches a basename under movie/, movies/, video/,
    // and videos/, including old DOS absolute paths in installed INI files.
    name.erase(0, name.find_last_of('/') + 1);
    expected.erase(0, expected.find_last_of('/') + 1);
    return name == expected;
}

std::vector<LevelCinematic> cinematicAudio()
{
    std::vector<LevelCinematic> scenes;
    for (int episode = 0; episode < gEpisodeCount; ++episode)
    {
        auto const& info = gEpisodeInfo[episode];
        if (info.nLevels > 0)
        {
            LevelCinematic outro;
            if (levelGetOutroCinematic(episode, 0, outro)) scenes.push_back(std::move(outro));
        }
        for (int level = 0; level < info.nLevels; ++level)
            levelCollectIntroCinematics(episode, level, scenes, true);
    }
    return scenes;
}

bool optionalCinematicFile(const char* path)
{
    const char* extension = path ? Bstrrchr(path, '.') : nullptr;
    if (!extension) return false;
    if (cinematicVideoType(extension + 1)) return true;
    if (Bstrcasecmp(extension + 1, "WAV")) return false;
    for (auto const& scene : cinematicAudio())
    {
        if (sameCinematicFile(path, scene.audio)) return true;
        if (scene.audioResource <= 0) continue;
        for (unsigned i = 0; i < gSoundRes.count; ++i)
        {
            auto const& node = gSoundRes.dict[i];
            if (node.id != unsigned(scene.audioResource) || !node.type || !node.name || Bstrcasecmp(node.type, "WAV")) continue;
            auto const name = std::string(node.name) + ".WAV";
            if (sameCinematicFile(path, name) || (node.path && sameCinematicFile(path, node.path))) return true;
        }
    }
    return false;
}

bool optionalCinematicResource(const DICTNODE& node, const char* domain)
{
    if (cinematicVideoType(node.type)) return true;
    // RAW samples and gameplay metadata remain mandatory, even when their
    // filenames or numeric IDs coincide with a movie's WAV accompaniment.
    if (!node.type || Bstrcasecmp(node.type, "WAV")) return false;
    auto const name = std::string(node.name) + ".WAV";
    if (optionalCinematicFile(name.c_str()) || (node.path && optionalCinematicFile(node.path))) return true;
    if (!std::strcmp(domain, "sound"))
        for (auto const& scene : cinematicAudio())
            if (scene.audioResource > 0 && node.id == unsigned(scene.audioResource)) return true;
    return false;
}

void rememberAsset(std::string key, const char* path, uint64_t size, const netassets::Digest& md5)
{
    netassets::Asset asset;
    asset.key = std::move(key);
    asset.name = displayName(path);
    asset.size = size;
    asset.md5 = md5;
    if (asset.name.empty()) return;
    auto existing = std::find_if(contentManifest.begin(), contentManifest.end(),
        [&](const netassets::Asset& entry) { return entry.key == asset.key; });
    if (existing == contentManifest.end()) contentManifest.push_back(std::move(asset));
    else *existing = std::move(asset);
}

bool readAsset(const char* path, bool searchMod, netassets::Asset& asset)
{
    auto const file = searchMod ? kopen4loadfrommod(path, 0) : kopen4load(path, 0);
    if (file == buildvfs_kfd_invalid) return false;
    auto const size = kfilelength(file);
    netassets::MD5 md5;
    uint64_t bytes = 0;
    uint8_t buffer[65536];
    int count;
    while ((count = kread(file, buffer, sizeof(buffer))) > 0)
    {
        md5.update(buffer, count);
        bytes += count;
    }
    kclose(file);
    if (size < 0 || bytes != uint64_t(size)) return false;
    asset.name = displayName(path);
    asset.size = bytes;
    asset.md5 = md5.finish();
    return true;
}

bool safeRelativeName(const std::string& name)
{
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos)
        return false;
    size_t pos = 0;
    while (pos < name.size())
    {
        auto const end = name.find_first_of("/\\", pos);
        auto const component = name.substr(pos, end == std::string::npos ? end : end - pos);
        if (component.empty() || component == "." || component == "..") return false;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return name.back() != '/' && name.back() != '\\';
}

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
void addFile(uint32_t& crc, const char* path, const std::string& key)
{
    const auto file = kopen4loadfrommod(path, 0);
    if (file == buildvfs_kfd_invalid) { addNumber(crc, 0); return; }
    addNumber(crc, 1);
    const auto size = kfilelength(file);
    addNumber(crc, size);
    netassets::MD5 md5;
    uint64_t bytes = 0;
    uint8_t buffer[65536];
    int count;
    while ((count = kread(file, buffer, sizeof(buffer))) > 0)
    {
        crc = Bcrc32(buffer, count, crc);
        md5.update(buffer, count);
        bytes += count;
    }
    kclose(file);
    if (size >= 0 && bytes == uint64_t(size)) rememberAsset(key, path, bytes, md5.finish());
}
void addOverrides(uint32_t& crc, Resource& resources, const char* domain)
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
        netassets::MD5 md5;
        md5.update(original.data(), original.size());
        rememberAsset(std::string("resource:") + domain + ":" + canonicalName(displayName(name.c_str())) + ":" + std::to_string(node->id),
                      node->path ? node->path : name.c_str(), node->size, md5.finish());
    }
}

void addOtherOverrides(Resource& resources, const char* domain)
{
    for (unsigned i = 0; i < resources.count; ++i)
    {
        auto* node = &resources.dict[i];
        if (!node->name || !node->type || !(node->flags & (DICT_EXTERNAL | DICT_BUFFER))) continue;
        // Maps use the separate on-demand download protocol. These other
        // resources include RAW sound samples and palette/voxel replacements.
        if (!Bstrcasecmp(node->type, "MAP") || !Bstrcasecmp(node->type, "QAV") ||
            !Bstrcasecmp(node->type, "SEQ") || !Bstrcasecmp(node->type, "DAT") ||
            !Bstrcasecmp(node->type, "SFX") || optionalCinematicResource(*node, domain)) continue;
        auto const name = canonicalName(std::string(node->name) + (node->type[0] ? "." : "") + node->type);
        std::vector<uint8_t> original(node->size);
        if (node->size) resources.Read(node, original.data());
        netassets::MD5 md5;
        md5.update(original.data(), original.size());
        rememberAsset(std::string("resource:") + domain + ":" + canonicalName(displayName(name.c_str())) + ":" + std::to_string(node->id),
                      node->path ? node->path : name.c_str(), node->size, md5.finish());
    }
}

void addDeferredOverrides(Resource& resources, const char* domain)
{
    for (unsigned i = 0; i < resources.count; ++i)
    {
        auto const* node = &resources.dict[i];
        if (!node->name || !node->type || (node->flags & (DICT_EXTERNAL | DICT_BUFFER)) ||
            !Bstrcasecmp(node->type, "MAP") || optionalCinematicResource(*node, domain)) continue;
        // Name-based Resource::Lookup will prefer this loose file when first
        // used, potentially after the startup manifest has been frozen. Probe
        // with the same VFS precedence without changing the resource index.
        auto const name = std::string(node->name) + (node->type[0] ? "." : "") + node->type;
        netassets::Asset asset;
        if (readAsset(name.c_str(), true, asset))
            rememberAsset(std::string("resource:") + domain + ":" + canonicalName(displayName(name.c_str())) + ":" + std::to_string(node->id),
                          name.c_str(), asset.size, asset.md5);
    }
}
}

uint32_t netContentCRC()
{
    static uint32_t fingerprint = 0;
    if (contentFrozen) return fingerprint;
    // Resource scripts may have tracked loose audio before levelLoadDefaults
    // established which files accompany cinematics. Reclassify those sources
    // now, while keeping complete archives and gameplay resources mandatory.
    contentManifest.erase(std::remove_if(contentManifest.begin(), contentManifest.end(),
        [](const netassets::Asset& asset) {
            return asset.key.compare(0, 7, "source:") == 0 && optionalCinematicFile(asset.name.c_str());
        }), contentManifest.end());
    uint32_t crc = 0;
    addText(crc, "NBlood authoritative content v1");
    addNumber(crc, netwire::Version);
    addFile(crc, pUserRFF ? pUserRFF : "BLOOD.RFF", "rff:system");
    addFile(crc, pUserSoundRFF ? pUserSoundRFF : "SOUNDS.RFF", "rff:sound");
    for (unsigned index = 0; index < MAXARTFILES_BASE; ++index)
    {
        char path[BMAX_PATH];
        Bsnprintf(path, sizeof(path), "%s%03u.ART", pUserTiles ? pUserTiles : "TILES", index);
        addNumber(crc, index); addFile(crc, path, "art:" + std::to_string(index));
    }
    addOverrides(crc, gSysRes, "system"); addOverrides(crc, gSoundRes, "sound");
    addOtherOverrides(gSysRes, "system"); addOtherOverrides(gSoundRes, "sound");
    addDeferredOverrides(gSysRes, "system"); addDeferredOverrides(gSoundRes, "sound");
    // Include effective collision/animation metadata after ART/DEF overrides.
    for (int i = 0; i < MAXTILES; ++i)
    {
        addNumber(crc, uint16_t(tilesiz[i].x)); addNumber(crc, uint16_t(tilesiz[i].y));
        addNumber(crc, picanm[i].num); addNumber(crc, uint8_t(picanm[i].xofs));
        addNumber(crc, uint8_t(picanm[i].yofs)); addNumber(crc, picanm[i].sf);
    }
    crc = Bcrc32(surfType, sizeof(surfType), crc);
    std::sort(contentManifest.begin(), contentManifest.end(),
        [](const netassets::Asset& a, const netassets::Asset& b) { return a.key < b.key; });
    fingerprint = crc; contentFrozen = true;
    return fingerprint;
}

const std::vector<netassets::Asset>& netContentManifest()
{
    netContentCRC();
    return contentManifest;
}

void netContentTrackFile(const char* path, bool searchMod)
{
    if (contentFrozen || !path || !*path || optionalCinematicFile(path)) return;
    auto const key = "source:" + canonicalName(displayName(path));
    if (std::any_of(contentManifest.begin(), contentManifest.end(),
            [&](const netassets::Asset& entry) { return entry.key == key; })) return;
    netassets::Asset asset;
    if (readAsset(path, searchMod, asset)) rememberAsset(key, path, asset.size, asset.md5);
}

void netContentTrackDefinition(const char* path)
{
    netContentTrackFile(path);
}

std::vector<netassets::Issue> netContentDifferences(const std::vector<netassets::Asset>& required)
{
    auto issues = netassets::compareManifest(required, netContentManifest());
    for (auto& issue : issues)
    {
        if (issue.kind != netassets::IssueKind::Missing || !safeRelativeName(issue.required.name)) continue;
        netassets::Asset local;
        // Only probe names from recognized asset slots, never arbitrary remote
        // keys. No bytes or hashes from these probes are returned to the server.
        auto const& key = issue.required.key;
        bool const source = key.compare(0, 7, "source:") == 0;
        if (!source && key.compare(0, 4, "rff:") != 0 && key.compare(0, 4, "art:") != 0 &&
            key.compare(0, 9, "resource:") != 0) continue;
        if (!readAsset(issue.required.name.c_str(), !source, local)) continue;
        local.key = key;
        issue.kind = local.size == issue.required.size && local.md5 == issue.required.md5
            ? netassets::IssueKind::NotLoaded : netassets::IssueKind::Mismatch;
        issue.local = std::move(local);
    }
    return issues;
}
