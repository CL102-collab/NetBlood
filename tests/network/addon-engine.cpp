// SPDX-License-Identifier: GPL-2.0-or-later
// Real inventory adapter and cache primitives with isolated startup/VFS globals.
#include "../../source/blood/src/netaddon_engine.cpp"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

char g_modDir[BMAX_PATH] = "/";
char BloodIniFile[BMAX_PATH] = "BLOOD.INI";
char *pUserRFF = nullptr, *pUserSoundRFF = nullptr, *pUserTiles = nullptr;
char *g_defNamePtr = nullptr;
GrowArray<char *> g_defModules;
int32_t (*g_resolveReadPath)(const char *, char **) = nullptr;
bool (*g_preloadAddonArchives)(std::string &) = nullptr;
namespace loguru {
[[noreturn]] void log_and_abort(int, const char *expression, const char *file, unsigned line, const char *, ...)
{ std::fprintf(stderr, "Engine allocator assertion: %s (%s:%u)\n", expression, file, line); std::abort(); }
}

namespace {
unsigned checks = 0;
std::vector<std::string> searchRoots;
std::vector<netassets::Asset> assets;
std::vector<std::string> loadedArchives;
bool archiveLoadFails = false;
void check(bool result, const char *expression, int line)
{
    ++checks;
    if (!result) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(expr) check(bool(expr), #expr, __LINE__)
void write(const std::string &path, const char *data)
{
    auto *file = std::fopen(path.c_str(), "wb"); CHECK(file);
    CHECK(std::fwrite(data, 1, std::strlen(data), file) == std::strlen(data));
    CHECK(std::fclose(file) == 0);
}
void reset()
{
    finalized = false; activating = false; explicitDefinition = false;
    profile = {}; activationProfile = {};
    addonDirectory.clear(); activationDirectory.clear(); profileError.clear();
    commandArchives.clear(); sourceFiles.clear(); assets.clear();
    activatedPaths.clear(); g_resolveReadPath = nullptr; usePackagedDefaultDefinition = false;
    archivesPreloaded = false; g_preloadAddonArchives = nullptr; loadedArchives.clear(); archiveLoadFails = false;
    std::strcpy(g_modDir, "/"); std::strcpy(BloodIniFile, "BLOOD.INI");
    g_defNamePtr = nullptr;
}
bool has(const std::string &path)
{
    return std::any_of(profile.files.begin(), profile.files.end(), [&](const netaddon::File &file) { return file.path == path; });
}
}

const char *G_DefFile() { return g_defNamePtr ? g_defNamePtr : "blood.def"; }
const std::vector<netassets::Asset> &netContentManifest() { return assets; }
int32_t findfrompath(const char *name, char **found)
{
    if (regular(name)) { *found = Xstrdup(name); return 0; }
    for (auto const &root : searchRoots)
        if (regular(root + "/" + name)) { *found = Xstrdup((root + "/" + name).c_str()); return 0; }
    return -1;
}
char *listsearchpath(int32_t first)
{
    static size_t index = 0;
    if (first) index = 0; else ++index;
    return index < searchRoots.size() ? const_cast<char *>(searchRoots[index].c_str()) : nullptr;
}
buildvfs_kfd kopen4loadfrommod(const char *path, char)
{
    return std::strcmp(path, "BLOOD.INI") == 0 ? 1 : buildvfs_kfd_invalid;
}
void kclose(buildvfs_kfd) {}
int initgroupfile(const char *path)
{ loadedArchives.emplace_back(path); return archiveLoadFails ? -1 : 0; }

int main(int argc, char **argv)
{
    engineCreateAllocator();
    CHECK(argc == 2);
    std::string temporary = std::string(argv[1]) + "/fixture.XXXXXX";
    char *root = mkdtemp(&temporary[0]); CHECK(root);
    CHECK(chdir(root) == 0);
    CHECK(mkdir("addons", 0700) == 0); CHECK(mkdir("addons/TestMod", 0700) == 0);
    CHECK(mkdir("addons/TestMod/maps", 0700) == 0);
    write("addons/TestMod/CAMPAIGN.INI", "[Episode1]\nMap1=maps/test\n");
    write("addons/TestMod/addon.def", "// addon definition\n");
    write("addons/TestMod/Zeta.pk3", "zip fake Z"); write("addons/TestMod/alpha.pk3", "zip fake a");
    write("addons/TestMod/maps/test.map", "map test");
    write("addons/TestMod/client.cfg", "Password=private");
    write("addons/TestMod/launcher.ini", "Password=private");
    write("addons/TestMod/launcher.exe", "executable");
    write("outside.ini", "not addon data");
    CHECK(symlink((std::string(root) + "/outside.ini").c_str(), "addons/TestMod/secret.ini") == 0);
    CHECK(symlink(root, "addons/TestMod/outside") == 0);
    std::string error;
    const char *arguments[] = {"nblood", "-game_dir", "addons/TestMod", "-ini", "CAMPAIGN.INI",
                              "-h", "addon.def", "-password", "not-an-archive.pk3"};
    netAddonCaptureArguments(sizeof(arguments) / sizeof(arguments[0]), arguments);
    std::strcpy(g_modDir, "addons/TestMod"); std::strcpy(BloodIniFile, "CAMPAIGN.INI");
    g_defNamePtr = const_cast<char *>("addon.def");
    CHECK(netAddonFinalize(error)); CHECK(error.empty()); CHECK(profile.files.size() == 5);
    CHECK(has("CAMPAIGN.INI")); CHECK(has("maps/test.map")); CHECK(!has("secret.ini"));
    CHECK(!has("client.cfg")); CHECK(!has("launcher.ini")); CHECK(!has("launcher.exe"));
    CHECK(profile.selections.size() == 4);
    CHECK(profile.selections[0].path == "alpha.pk3"); CHECK(profile.selections[1].path == "Zeta.pk3");
    CHECK(netAddonSource(0).find(root) == 0); CHECK(regular(netAddonSource(0)));
    CHECK(netAddonSource(10000).empty());
    auto const required = profile;
    auto const expectedId = netaddon::identity(required);
    std::string installed;
    CHECK(netAddonFindInstalled(required, installed, error));
    CHECK(installed == std::string(root) + "/addons/TestMod");
    std::vector<std::string> args;
    CHECK(netAddonResourceArguments(required, installed, args, error));
    CHECK(args.size() == 9 && args[0] == "-game_dir" && args[1] == "/" && args[2] == "-noautoload");
    CHECK(args[3] == "-j" && args[4] == installed);

    reset();
    // Restart restores exact server selection order, independent of its directory's display name.
    CHECK(netAddonActivateProfile(required, installed, error));
    write("CAMPAIGN.INI", "stale local addon must not shadow activation");
    char *resolved = nullptr;
    CHECK(g_resolveReadPath && g_resolveReadPath("campaign.ini", &resolved) == 1);
    CHECK(std::string(resolved) == installed + "/CAMPAIGN.INI"); Xfree(resolved);
    CHECK(g_resolveReadPath("client.cfg", &resolved) == 0);
    CHECK(g_preloadAddonArchives && g_preloadAddonArchives(error));
    CHECK(loadedArchives == std::vector<std::string>({"alpha.pk3", "Zeta.pk3"}));
    CHECK(g_preloadAddonArchives(error) && loadedArchives.size() == 2);
    std::vector<const char *> restart = {"nblood"};
    for (auto const &arg : args) restart.push_back(arg.c_str());
    netAddonCaptureArguments(int(restart.size()), restart.data());
    std::strcpy(BloodIniFile, "CAMPAIGN.INI"); g_defNamePtr = const_cast<char *>("addon.def");
    CHECK(netAddonFinalize(error)); CHECK(netaddon::identity(netAddonProfile()) == expectedId);
    CHECK(netAddonSource(0).find(installed) == 0);

    reset();
    const char *vanilla[] = {"nblood"}; netAddonCaptureArguments(1, vanilla);
    CHECK(netAddonFinalize(error)); CHECK(profile.files.empty());
    CHECK(profile.selections.size() == 1 && profile.selections[0].path == "BLOOD.INI");
    CHECK(netAddonFindInstalled(profile, installed, error)); CHECK(installed.empty());
    CHECK(netAddonResourceArguments(profile, installed, args, error));
    CHECK(args.size() == 5 && args[3] == "-ini" && args[4] == "BLOOD.INI");
    auto const stock = profile;
    reset();
    CHECK(netAddonActivateProfile(stock, "", error));
    CHECK(g_resolveReadPath("blood.def", &resolved) == 2);
    CHECK(g_resolveReadPath("BLOOD.RFF", &resolved) == 0);
    CHECK(g_resolveReadPath("nblood.cfg", &resolved) == 0);

    const char *quick[] = {"nblood", "-quick", "-g=archive.pk3", "-nodudes", "-grp", "second.grp"};
    netAddonCaptureArguments(6, quick);
    CHECK(commandArchives == std::vector<std::string>({"archive.pk3", "second.grp"}));

    reset();
    // Absolute game_dir is a search directory, but the engine does not autoload
    // its archive files. Advertising those as mounted changes the addon.
    netAddonCaptureArguments(1, vanilla);
    std::strcpy(g_modDir, (std::string(root) + "/addons/TestMod").c_str());
    std::strcpy(BloodIniFile, "CAMPAIGN.INI");
    CHECK(netAddonFinalize(error));
    CHECK(std::none_of(profile.selections.begin(), profile.selections.end(), [](const netaddon::Selection &item) {
        return item.kind == netaddon::SelectionKind::PreloadArchive;
    }));

    reset();
    // An INI-only launch finds nearby addon files without distributing installed base data.
    write("BLOOD.RFF", "base data"); write("SOUNDS.RFF", "base sounds"); write("TILES000.ART", "base art");
    write("LOCAL.INI", "custom campaign"); write("local.map", "custom level");
    netAddonCaptureArguments(1, vanilla); std::strcpy(BloodIniFile, "LOCAL.INI");
    CHECK(netAddonFinalize(error)); CHECK(has("LOCAL.INI")); CHECK(has("local.map"));
    CHECK(!has("BLOOD.RFF")); CHECK(!has("SOUNDS.RFF")); CHECK(!has("TILES000.ART"));
    CHECK(!has("addons/TestMod/CAMPAIGN.INI"));
    CHECK(!has("outside.ini")); CHECK(!has("CAMPAIGN.INI"));

    reset();
    CHECK(netAddonActivateProfile(required, std::string(root) + "/addons/TestMod", error));
    archiveLoadFails = true;
    CHECK(!netAddonPreloadArchives(error)); CHECK(error.find("alpha.pk3") != std::string::npos);

    reset();
    CHECK(netAddonActivateProfile(required, std::string(root) + "/addons/TestMod", error));
    write("addons/TestMod/maps/test.map", "changed after handoff");
    CHECK(!netAddonFinalize(error)); CHECK(!error.empty());
    std::printf("addon engine inventory: %u checks passed\n", checks);
    engineDestroyAllocator();
    return 0;
}
