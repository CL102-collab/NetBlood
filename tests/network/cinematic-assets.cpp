// SPDX-License-Identifier: GPL-2.0-or-later
// Production inventory classification with isolated resource/VFS boundaries.
#include "../../source/blood/src/netcontent.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern IniFile* BloodINI;
namespace {
unsigned checks = 0;
unsigned fileOpens = 0;
int filePosition = 0;
bool declaredIntro = false;
void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)
}

Resource::Resource() : dict(nullptr), count(0) {}
Resource::~Resource() {}
void Resource::Read(DICTNODE* node, void* data) { std::memset(data, 0, node->size); }
Resource gSoundRes;
Resource gSysRes;
char *pUserRFF = nullptr, *pUserSoundRFF = nullptr, *pUserTiles = nullptr;
vec2_16_t tilesiz[MAXTILES] = {};
picanm_t picanm[MAXTILES] = {};
char surfType[kMaxTiles] = {};
IniFile::IniFile(unsigned char*, int, char) {}
IniFile::~IniFile() {}
int IniFile::KeyFind(const char*, const char* key)
{ return declaredIntro && !std::strcmp(key, "CutSceneA") ? 0 : -1; }

buildvfs_kfd kopen4load(const char*, char) { ++fileOpens; filePosition = 0; return 1; }
buildvfs_kfd kopen4loadfrommod(const char* path, char first) { return kopen4load(path, first); }
int32_t kfilelength(buildvfs_kfd) { return 3; }
int32_t kread(buildvfs_kfd, void* data, int32_t length)
{
    int const count = std::min(length, 3 - filePosition);
    std::memset(data, 0, count); filePosition += count; return count;
}
void kclose(buildvfs_kfd) {}

namespace {
IniFile ini(nullptr, 0, 0);
DICTNODE soundNodes[2] = {};

DICTNODE node(const char* name, const char* type, unsigned id = 0, const char* path = nullptr)
{
    DICTNODE entry = {};
    entry.name = const_cast<char*>(name);
    entry.type = const_cast<char*>(type);
    entry.path = const_cast<char*>(path);
    entry.id = id;
    entry.flags = DICT_EXTERNAL;
    return entry;
}

void custom()
{
    contentManifest.clear();
    contentFrozen = false;
    gGameOptions = {};
    gEpisodeCount = 1;
    std::memset(gEpisodeInfo, 0, sizeof(EPISODEINFO) * (kMaxEpisodes + 1));
    BloodINI = &ini;
    declaredIntro = false;
    bINIOverride = true;
    std::strcpy(BloodIniFile, "MOD.INI");
    auto& episode = gEpisodeInfo[0];
    episode.nLevels = 2;
    std::strcpy(episode.cutsceneASmkPath, "cinema/intro.smk");
    std::strcpy(episode.cutsceneAWavPath, "cinema/voice.wav");
    episode.cutsceneAWavRsrcID = 0;
    std::strcpy(episode.cutsceneBSmkPath, "cinema/end.ogv");
    episode.cutsceneBWavRsrcID = 315;
    std::strcpy(episode.levelsInfo[1].cutVideo, "maps/enter.ogv");
    std::strcpy(episode.levelsInfo[1].cutSound, "maps/enter.wav");
    soundNodes[0] = node("OUTRO", "WAV", 315, "voices/outro.wav");
    soundNodes[1] = node("OUTRO", "RAW", 315, "voices/outro.raw");
    gSoundRes.dict = soundNodes;
    gSoundRes.count = 2;
}

void classification()
{
    custom();
    for (const char* path : {"CS1.SMK", "movies/intro.ogv", "VOICE.WAV", "movie/voice.wav",
                             "C:\\Blood\\voice.wav", "maps/enter.wav", "voices/outro.wav"})
        CHECK(optionalCinematicFile(path));
    for (const char* path : {"gameplay.wav", "voice.raw", "outro.raw", "BLOOD.RFF", "SOUNDS.RFF",
                             "movie.qav", "movie.sfx", "movie.dat", "TILES000.ART", "movie.ogv.def", "", static_cast<const char*>(nullptr)})
        CHECK(!optionalCinematicFile(path));
    auto cinematic = node("VOICE", "WAV");
    CHECK(optionalCinematicResource(cinematic, "sound"));
    auto idMovie = node("ALTERNATE", "WAV", 315);
    CHECK(optionalCinematicResource(idMovie, "sound"));
    CHECK(!optionalCinematicResource(idMovie, "system"));
    for (const char* type : {"RAW", "SEQ", "QAV", "DAT", "SFX"}) {
        auto gameplay = node("VOICE", type, 315, "cinema/voice.wav");
        CHECK(!optionalCinematicResource(gameplay, "sound"));
    }
    auto generic = node("GAMEPLAY", "WAV", 101);
    CHECK(!optionalCinematicResource(generic, "sound"));
    // A user's starting map cannot affect which movie packs are required.
    std::strcpy(gGameOptions.szUserMap, "custom.map");
    CHECK(optionalCinematicFile("voice.wav"));
    CHECK(optionalCinematicFile("enter.wav"));
}

void inventory()
{
    custom();
    DICTNODE entries[] = {node("INTRO", "SMK"), node("INTRO", "OGV"), node("VOICE", "WAV"),
                         node("GAMEPLAY", "WAV"), node("VOICE", "RAW", 315, "cinema/voice.wav"),
                         node("OUTRO", "WAV", 315), node("ENTER", "WAV")};
    Resource resources;
    resources.dict = entries; resources.count = sizeof(entries) / sizeof(entries[0]);
    addOtherOverrides(resources, "sound");
    CHECK(contentManifest.size() == 2);
    CHECK(contentManifest[0].name == "GAMEPLAY.WAV");
    CHECK(contentManifest[1].key == "resource:sound:VOICE.RAW:315");
    contentManifest.clear();
    for (auto& entry : entries) entry.flags = 0;
    fileOpens = 0;
    addDeferredOverrides(resources, "sound");
    CHECK(contentManifest.size() == 2 && fileOpens == 2);
    contentManifest.clear(); fileOpens = 0;
    netContentTrackFile("intro.OGV");
    netContentTrackFile("intro.SMK");
    netContentTrackFile("cinema/voice.wav");
    netContentTrackFile("outro.wav");
    CHECK(contentManifest.empty() && fileOpens == 0);
    netContentTrackFile("gameplay.wav");
    netContentTrackFile("voice.raw");
    netContentTrackFile("BLOOD.RFF");
    netContentTrackFile("mod.def");
    CHECK(contentManifest.size() == 4 && fileOpens == 4);
}

void stockFallback()
{
    custom();
    std::memset(gEpisodeInfo, 0, sizeof(EPISODEINFO) * (kMaxEpisodes + 1));
    bINIOverride = false;
    std::strcpy(BloodIniFile, "BLOOD.INI");
    gEpisodeInfo[0].nLevels = 8;
    for (int level = 0; level < 8; ++level)
        std::snprintf(gEpisodeInfo[0].levelsInfo[level].Filename, BMAX_PATH, "e1m%d", level + 1);
    CHECK(optionalCinematicFile("cs1822m.wav"));
    CHECK(optionalCinematicFile("movie/cs2822m.wav"));
    CHECK(!optionalCinematicFile("cs3822m.wav"));
    std::strcpy(gGameOptions.szUserMap, "custom.map");
    CHECK(optionalCinematicFile("cs1822m.wav"));
    declaredIntro = true;
    CHECK(!optionalCinematicFile("cs1822m.wav"));
    bINIOverride = true;
    CHECK(!optionalCinematicFile("cs2822m.wav"));
}

void freezeInventory()
{
    custom();
    // Startup resource scripts can precede level metadata. Their cinematic
    // sources must be removed at freeze; generic WAV and archives must stay.
    gEpisodeCount = 0;
    netContentTrackFile("cinema/voice.wav");
    netContentTrackFile("gameplay.wav");
    CHECK(contentManifest.size() == 2);
    gEpisodeCount = 1;
    initcrc32table();
    netContentCRC();
    bool generic = false, systemArchive = false, soundArchive = false, raw = false;
    for (auto const& asset : contentManifest) {
        CHECK(asset.key != "source:CINEMA/VOICE.WAV");
        generic |= asset.key == "source:GAMEPLAY.WAV";
        systemArchive |= asset.key == "rff:system";
        soundArchive |= asset.key == "rff:sound";
        raw |= asset.key == "resource:sound:OUTRO.RAW:315";
    }
    CHECK(generic && systemArchive && soundArchive && raw);
}
}

int main()
{
    classification();
    inventory();
    stockFallback();
    freezeInventory();
    std::printf("cinematic asset inventory: %u checks passed\n", checks);
}
