// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise production cinematic selection independently of installed movies.
#include "build.h"
#include "levels.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

extern IniFile* BloodINI;
namespace {
unsigned assertions = 0;
std::set<std::string> declaredKeys;
void check(bool condition, const char* expression, int line)
{
    ++assertions;
    if (!condition) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)
}

// Only INI key presence is queried by these selectors. Playback, file access,
// and the remainder of levels.cpp are discarded by the linker's gc-sections.
IniFile::IniFile(unsigned char*, int, char) {}
IniFile::~IniFile() {}
int IniFile::KeyFind(const char* section, const char* key)
{
    return declaredKeys.count(std::string(section) + "/" + key) ? 0 : -1;
}

namespace {
IniFile ini(nullptr, 0, 0);

void stock()
{
    gEpisodeCount = 4;
    gGameOptions = {};
    std::memset(gEpisodeInfo, 0, sizeof(EPISODEINFO) * (kMaxEpisodes + 1));
    std::strcpy(BloodIniFile, "BLOOD.INI");
    bINIOverride = false;
    BloodINI = &ini;
    declaredKeys.clear();
    int const counts[] = {8, 9, 8, 9};
    for (int episode = 0; episode < 4; ++episode) {
        gEpisodeInfo[episode].nLevels = counts[episode];
        for (int level = 0; level < counts[episode]; ++level)
            std::snprintf(gEpisodeInfo[episode].levelsInfo[level].Filename, BMAX_PATH,
                          "e%dm%d", episode + 1, level + 1);
    }
    gEpisodeInfo[3].cutALevel = 7;
}

void stockMovies()
{
    stock();
    int const endings[] = {2, 3, 4, 6};
    for (int episode = 0; episode < 4; ++episode) {
        for (int level = 0; level < gEpisodeInfo[episode].nLevels; ++level) {
            std::vector<LevelCinematic> scenes;
            levelCollectIntroCinematics(episode, level, scenes);
            int const intro = episode == 0 && level == 0 ? 1 : episode == 3 && level == 7 ? 5 : 0;
            CHECK(scenes.size() == (intro ? 1u : 0u));
            if (intro) {
                CHECK(scenes[0].video == "CS" + std::to_string(intro) + ".SMK");
                CHECK(scenes[0].audio == "CS" + std::to_string(intro) + "822M.WAV");
                CHECK(scenes[0].audioResource == 0);
                CHECK(!scenes[0].mapScene);
            }
            LevelCinematic scene;
            CHECK(levelGetOutroCinematic(episode, level, scene));
            CHECK(scene.video == "CS" + std::to_string(endings[episode]) + ".SMK");
            CHECK(scene.audio == "CS" + std::to_string(endings[episode]) + "822M.WAV");
            CHECK(scene.audioResource == 0);
            CHECK(!scene.mapScene);
        }
    }
    // Installation-relative paths and case do not disable a stock episode.
    std::strcpy(BloodIniFile, "installed\\blood.ini");
    std::vector<LevelCinematic> scenes;
    levelCollectIntroCinematics(0, 0, scenes);
    CHECK(scenes.size() == 1 && scenes[0].video == "CS1.SMK");
}

void explicitMovies()
{
    stock();
    bINIOverride = true;
    std::strcpy(BloodIniFile, "MOD.INI");
    auto& episode = gEpisodeInfo[1];
    episode.cutALevel = 2;
    std::strcpy(episode.cutsceneASmkPath, "mod/intro.ogv");
    std::strcpy(episode.cutsceneAWavPath, "mod/narration.wav");
    episode.cutsceneAWavRsrcID = 0;
    std::strcpy(episode.cutsceneBSmkPath, "mod/end.smk");
    episode.cutsceneBWavRsrcID = 471;
    std::strcpy(episode.levelsInfo[2].cutVideo, "mod/map.ogv");
    std::strcpy(episode.levelsInfo[2].cutSound, "mod/map.wav");
    std::vector<LevelCinematic> scenes;
    LevelCinematic preceding;
    preceding.video = "queued/outro.smk";
    scenes.push_back(preceding);
    levelCollectIntroCinematics(1, 2, scenes);
    CHECK(scenes.size() == 3);
    CHECK(scenes[0].video == "queued/outro.smk");
    CHECK(scenes[1].video == "mod/intro.ogv" && scenes[1].audio == "mod/narration.wav");
    CHECK(scenes[1].audioResource == 0);
    CHECK(!scenes[1].mapScene);
    CHECK(scenes[2].video == "mod/map.ogv" && scenes[2].audio == "mod/map.wav");
    CHECK(scenes[2].audioResource == 0);
    CHECK(scenes[2].mapScene);
    LevelCinematic end;
    CHECK(levelGetOutroCinematic(1, 2, end));
    CHECK(end.video == "mod/end.smk" && end.audio.empty() && end.audioResource == 471);
    CHECK(!end.mapScene);
    scenes.clear();
    levelCollectIntroCinematics(1, 1, scenes);
    CHECK(scenes.empty());
    // A standalone user map retains its own scene without inheriting the
    // episode intro from the metadata slot reused for that map.
    std::strcpy(gGameOptions.szUserMap, "custom.map");
    levelCollectIntroCinematics(1, 2, scenes);
    CHECK(scenes.size() == 1 && scenes[0].video == "mod/map.ogv" && scenes[0].mapScene);
    scenes.clear();
    levelCollectIntroCinematics(1, 2, scenes, true);
    CHECK(scenes.size() == 2 && scenes[0].video == "mod/intro.ogv" && scenes[1].mapScene);
    gGameOptions.szUserMap[0] = 0;
    scenes.clear();
    // Map scenes remain available independently of an episode introduction.
    episode.cutsceneASmkPath[0] = 0;
    levelCollectIntroCinematics(1, 2, scenes);
    CHECK(scenes.size() == 1 && scenes[0].video == "mod/map.ogv" && scenes[0].mapScene);
}

void fallbackBoundaries()
{
    for (int mode = 0; mode < 6; ++mode) {
        stock();
        switch (mode) {
        case 0: bINIOverride = true; break;
        case 1: std::strcpy(BloodIniFile, "OTHER.INI"); break;
        case 2: gEpisodeInfo[0].nLevels = 7; break;
        case 3: std::strcpy(gEpisodeInfo[0].levelsInfo[4].Filename, "CUSTOM"); break;
        case 4: BloodINI = nullptr; break;
        case 5:
            declaredKeys.insert("Episode1/CutSceneA");
            declaredKeys.insert("Episode1/CutSceneB");
            break;
        }
        std::vector<LevelCinematic> scenes;
        levelCollectIntroCinematics(0, 0, scenes);
        CHECK(scenes.empty());
        LevelCinematic end;
        end.video = "stale";
        CHECK(!levelGetOutroCinematic(0, 0, end));
        CHECK(end.video.empty() && end.audio.empty() && end.audioResource == -1);
    }
    stock();
    gEpisodeCount = 5;
    gEpisodeInfo[4].nLevels = 1;
    for (auto const& position : {std::pair<int, int>{-1, 0}, {0, -1}, {5, 0}, {0, 8}, {4, 0}}) {
        std::vector<LevelCinematic> scenes;
        levelCollectIntroCinematics(position.first, position.second, scenes);
        CHECK(scenes.empty());
        LevelCinematic end;
        CHECK(!levelGetOutroCinematic(position.first, position.second, end));
    }
}
}

int main()
{
    stockMovies();
    explicitMovies();
    fallbackBoundaries();
    std::printf("cinematic level selection: %u checks passed\n", assertions);
}
