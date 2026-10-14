// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"
#include "../../build/src/engine_priv.h"
#include "common_game.h"
#include "levels.h"
#include "netaddon_engine.h"
#include "netcontent.h"
#include <algorithm>
#include <map>
#include <set>
#ifdef _WIN32
# include "windows_inc.h"
#else
# include <sys/stat.h>
#endif

extern char *pUserRFF, *pUserSoundRFF, *pUserTiles;
extern bool (*g_preloadAddonArchives)(std::string &error);

namespace {
std::string startupDirectory, addonDirectory, profileError;
std::vector<std::string> commandArchives, sourceFiles;
netaddon::Profile profile;
bool finalized = false, explicitDefinition = false;
netaddon::Profile activationProfile;
std::string activationDirectory;
bool activating = false;
std::map<std::string, std::string> activatedPaths;
bool usePackagedDefaultDefinition = false;
bool archivesPreloaded = false;

std::string portable(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.compare(0, 2, "./") == 0) path.erase(0, 2);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}
std::string lower(std::string text)
{
    for (auto &ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}
std::string basename(const std::string &path)
{ return path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1); }
std::string parent(const std::string &path)
{ auto const slash = path.find_last_of('/'); return slash == std::string::npos ? "." : path.substr(0, slash); }
std::string absolute(const std::string &path)
{
    if (path.empty()) return {};
    char buffer[BMAX_PATH];
#ifdef _WIN32
    auto const length = GetFullPathNameA(path.c_str(), sizeof(buffer), buffer, nullptr);
    if (!length || length >= sizeof(buffer)) return {};
#else
    std::string result = path;
    if (path[0] != '/')
    {
        if (!getcwd(buffer, sizeof(buffer))) return {};
        result = std::string(buffer) + "/" + path;
    }
    if (result.size() >= sizeof(buffer)) return {};
    Bstrcpy(buffer, result.c_str());
#endif
    // Bcanonicalisefilename historically always removes the last component,
    // regardless of its removefn argument. Preserve actual resource filenames.
    if (Bcorrectfilename(buffer, 0)) return {};
    return portable(buffer);
}
bool directory(const std::string &path)
{
#ifdef _WIN32
    auto const attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}
bool regular(const std::string &path)
{
#ifdef _WIN32
    auto const attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
#endif
}
bool within(const std::string &path, const std::string &root)
{
    return !root.empty() && path.size() > root.size() &&
#ifdef _WIN32
        lower(path.substr(0, root.size())) == lower(root) &&
#else
        path.compare(0, root.size(), root) == 0 &&
#endif
        path[root.size()] == '/';
}
bool originalName(const std::string &path)
{
    auto const name = lower(basename(path));
    if (name == "blood.rff" || name == "sounds.rff" || name == "gui.rff" ||
        name == "blood.ini" || name == "nblood.pk3") return true;
    return name.size() == 12 && name.compare(0, 5, "tiles") == 0 &&
           name.compare(8, 4, ".art") == 0 && name[5] >= '0' && name[5] <= '9' &&
           name[6] >= '0' && name[6] <= '9' && name[7] >= '0' && name[7] <= '9';
}
bool archive(const std::string &path)
{
    auto const ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
    return ext == ".grp" || ext == ".zip" || ext == ".ssi" || ext == ".pk3" || ext == ".pk4";
}

#ifndef USE_PHYSFS
int32_t resolveActivatedRead(const char *filename, char **resolved)
{
    auto const path = portable(filename ? filename : "");
    auto const found = activatedPaths.find(lower(path));
    if (found == activatedPaths.end())
        return usePackagedDefaultDefinition && lower(path) == "blood.def" ? 2 : 0;
    *resolved = Xstrdup(found->second.c_str());
    return 1;
}
#endif

std::string resolve(const std::string &path, bool modFirst = true)
{
    if (modFirst && !addonDirectory.empty() && netaddon::validPath(portable(path)))
    {
        auto const candidate = addonDirectory + "/" + portable(path);
        if (regular(candidate)) return absolute(candidate);
    }
    if ((!path.empty() && path[0] == '/') || (path.size() > 1 && path[1] == ':'))
        return regular(path) ? absolute(path) : std::string();
    char *found = nullptr;
    if (findfrompath(path.c_str(), &found) == 0)
    {
        auto const result = absolute(found);
        Xfree(found);
        if (regular(result)) return result;
    }
    return {};
}

std::string logical(const std::string &requested, const std::string &resolved)
{
    if (within(resolved, addonDirectory)) return resolved.substr(addonDirectory.size() + 1);
    auto const value = portable(requested);
    return netaddon::validPath(value) ? value : basename(value);
}

bool addFile(const std::string &name, const std::string &source, std::string &error)
{
    if (!netaddon::validPath(name) || !netaddon::allowedFile(name))
    { error = "Addon contains an unsupported filename: " + name; return false; }
    netaddon::File file;
    file.path = name;
    if (!netaddon::inspectFile(source, file, error)) return false;
    for (size_t i = 0; i < profile.files.size(); ++i)
        if (lower(profile.files[i].path) == lower(file.path))
        {
            if (profile.files[i].path == file.path && profile.files[i].size == file.size && profile.files[i].md5 == file.md5)
                return true;
            error = "Addon files collide at the portable path " + file.path;
            return false;
        }
    if (profile.files.size() == netaddon::MaxFiles)
    { error = "Addon has too many files for automatic downloading"; return false; }
    profile.files.push_back(std::move(file));
    sourceFiles.push_back(source);
    return true;
}

bool scan(const std::string &root, const std::string &relative, bool allowOriginal, unsigned depth, std::string &error)
{
    if (depth > 16) { error = "Addon directory nesting exceeds 16 levels"; return false; }
    auto const path = relative.empty() ? root : root + "/" + relative;
    auto *dir = Bopendir(path.c_str());
    if (!dir) { error = "Cannot read addon directory " + path; return false; }
    std::vector<std::string> entries;
    while (auto const *entry = Breaddir(dir))
        if (entry->name[0] != '.') entries.emplace_back(entry->name);
    Bclosedir(dir);
    std::sort(entries.begin(), entries.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    for (auto const &entry : entries)
    {
        auto const name = relative.empty() ? entry : relative + "/" + entry;
        auto const source = root + "/" + name;
        if (directory(source))
        {
            // Do not accidentally package sibling addons, recordings, or the download cache.
            auto const label = lower(entry);
            if (label == "addons" || label == "mods" || label == "autoload" || label == "netdownloads" || label == "netaddons") continue;
            if (!scan(root, name, allowOriginal, depth + 1, error)) return false;
        }
        else if (regular(source) && netaddon::allowedFile(name) && (allowOriginal || !originalName(name)))
        {
            // Only the selected campaign INI is needed. Other .ini files may
            // contain launcher or personal settings even inside an addon tree;
            // explicitly tracked definition dependencies are added separately.
            auto const folded = lower(name);
            if (folded.size() >= 4 && folded.compare(folded.size() - 4, 4, ".ini") == 0 &&
                lower(source) != lower(resolve(BloodIniFile))) continue;
            if (!addFile(name, source, error)) return false;
        }
    }
    return true;
}

bool selection(netaddon::SelectionKind kind, const std::string &requested, bool required, std::string &error)
{
    if (requested.empty()) return true;
    auto const source = resolve(requested);
    auto const name = logical(requested, source);
    if (!netaddon::validPath(name)) { error = "Addon option cannot be made portable: " + requested; return false; }
    if (!source.empty())
    {
        // Base game files remain supplied by the local installation. A selected
        // addon directory may intentionally contain replacement files with the same names.
        if ((!originalName(name) || within(source, addonDirectory)) && !addFile(name, source, error)) return false;
    }
    else
    {
        auto const handle = kopen4loadfrommod(requested.c_str(), 0);
        if (handle == buildvfs_kfd_invalid)
        {
            if (!required) return true;
            error = "Cannot locate addon resource " + requested; return false;
        }
        kclose(handle); // Included archive supplies this relative selection.
    }
    profile.selections.push_back({kind, name});
    return true;
}

void candidate(std::vector<std::string> &roots, const std::string &value)
{
    auto const path = absolute(value);
    if (!path.empty() && directory(path) && std::find(roots.begin(), roots.end(), path) == roots.end()) roots.push_back(path);
}
void children(std::vector<std::string> &roots, const std::string &path)
{
    if (!directory(path)) return;
    auto *dir = Bopendir(path.c_str());
    if (!dir) return;
    while (auto const *entry = Breaddir(dir))
        if (entry->name[0] != '.' && roots.size() < 512) candidate(roots, path + "/" + entry->name);
    Bclosedir(dir);
}
}

void netAddonCaptureArguments(int argc, const char *const *argv)
{
    startupDirectory = absolute(".");
    commandArchives.clear();
    explicitDefinition = false;
    static const std::set<std::string> valueOptions = {
        "map", "nodudes", "playback", "record", "setupfile", "skill", "ini", "control", "vector", "pname",
        "art", "snd", "rff", "maxalloc", "server", "client", "cachesize", "game_dir", "cfg", "port", "h", "mh", "j",
        "c", "conf", "s", "gametype", "maxplayers", "timelimit", "scorelimit", "autoaim", "weaponsstay", "maplist",
        "episode", "difficulty", "losekeys", "mapvote", "friendlyfire", "holsterweapon", "chaseview", "lives", "stunserver", "password"
    };
    for (int i = 1; i < argc; ++i)
    {
        std::string value = argv[i] ? argv[i] : "";
        auto const equals = value.find('=');
        auto option = lower(value.substr(0, equals));
        if (!option.empty() && (option[0] == '-' || option[0] == '/')) option.erase(0, 1);
        if (option == "g" || option == "grp")
        {
            if (equals != std::string::npos) value.erase(0, equals + 1);
            else if (i + 1 < argc) value = argv[++i];
            else continue;
            if (value.find('.') == std::string::npos) value += ".grp";
            commandArchives.push_back(value);
        }
        else if (!value.empty() && (value[0] == '-' || value[0] == '/') && valueOptions.count(option))
        {
            if (option == "h") explicitDefinition = true;
            if (equals == std::string::npos && i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '/') ++i;
        }
        else if (!value.empty() && value[0] != '-' && archive(value)) commandArchives.push_back(value);
    }
}

bool netAddonActivateProfile(const netaddon::Profile &required, const std::string &root, std::string &error)
{
    if (finalized) { error = "Addon resources have already been initialized"; return false; }
    if (!netaddon::validate(required, error)) return false;
    if (!required.files.empty() && (root.empty() || !netaddon::matches(root, required, error))) return false;
    activationProfile = required;
    activationDirectory = root.empty() ? std::string() : absolute(root);
    usePackagedDefaultDefinition = std::none_of(required.selections.begin(), required.selections.end(),
        [](const netaddon::Selection &item) { return item.kind == netaddon::SelectionKind::Definition; });
    activatedPaths.clear();
    for (auto const &file : required.files)
        activatedPaths.emplace(lower(file.path), activationDirectory + "/" + file.path);
#ifndef USE_PHYSFS
    g_resolveReadPath = resolveActivatedRead;
#endif
    g_preloadAddonArchives = netAddonPreloadArchives;
    activating = true;
    return true;
}

bool netAddonPreloadArchives(std::string &error)
{
    if (!activating || archivesPreloaded) return true;
    for (auto const &item : activationProfile.selections)
        if (item.kind == netaddon::SelectionKind::PreloadArchive && initgroupfile(item.path.c_str()) < 0)
        { error = "Cannot load addon archive " + item.path; return false; }
    archivesPreloaded = true;
    return true;
}

bool netAddonFinalize(std::string &error)
{
    if (finalized) { error = profileError; return error.empty(); }
    finalized = true;
    auto build = [&]() -> bool {
        if (activating)
        {
            if (!activationProfile.files.empty() && !netaddon::matches(activationDirectory, activationProfile, error)) return false;
            std::vector<std::string> expectedArchives;
            for (auto const &item : activationProfile.selections)
            {
                const char *actual = nullptr;
                switch (item.kind)
                {
                    case netaddon::SelectionKind::Ini: actual = BloodIniFile; break;
                    case netaddon::SelectionKind::Rff: actual = pUserRFF; break;
                    case netaddon::SelectionKind::Snd: actual = pUserSoundRFF; break;
                    case netaddon::SelectionKind::Art: actual = pUserTiles; break;
                    case netaddon::SelectionKind::Definition: actual = G_DefFile(); break;
                    case netaddon::SelectionKind::DefinitionModule: break;
                    case netaddon::SelectionKind::Archive: expectedArchives.push_back(item.path); break;
                    case netaddon::SelectionKind::PreloadArchive:
                        if (!archivesPreloaded) { error = "Addon archives were not loaded before definitions"; return false; }
                        break;
                }
                if (actual && portable(actual) != item.path)
                { error = "Restart did not activate addon resource " + item.path; return false; }
                if (!actual && item.kind != netaddon::SelectionKind::Archive && item.kind != netaddon::SelectionKind::PreloadArchive &&
                    item.kind != netaddon::SelectionKind::DefinitionModule)
                { error = "Restart omitted addon resource " + item.path; return false; }
            }
            if (expectedArchives != commandArchives)
            { error = "Restart did not preserve addon archive load order"; return false; }
            std::vector<std::string> expectedModules, actualModules;
            for (auto const &item : activationProfile.selections)
                if (item.kind == netaddon::SelectionKind::DefinitionModule) expectedModules.push_back(item.path);
            for (auto const *item : g_defModules) actualModules.push_back(portable(item));
            if (expectedModules != actualModules)
            { error = "Restart did not preserve addon definition module load order"; return false; }
            profile = activationProfile;
            addonDirectory = activationDirectory;
            for (auto const &file : profile.files) sourceFiles.push_back(addonDirectory + "/" + file.path);
            return true;
        }
        bool explicitDirectory = g_modDir[0] && std::strcmp(g_modDir, "/") != 0;
        if (explicitDirectory)
        {
            addonDirectory = absolute(g_modDir);
            if (!directory(addonDirectory)) addonDirectory = absolute(startupDirectory + "/" + g_modDir);
            if (!directory(addonDirectory)) { error = "Cannot locate selected addon directory " + std::string(g_modDir); return false; }
        }
        auto const ini = resolve(BloodIniFile);
        if (addonDirectory.empty() && !originalName(BloodIniFile) && !ini.empty()) addonDirectory = parent(ini);
        profile.name = addonDirectory.empty() ? "Blood" : basename(addonDirectory);
        if (profile.name.empty()) profile.name = "Addon";
        if (!addonDirectory.empty() && !scan(addonDirectory, "", explicitDirectory, 0, error)) return false;

        // G_LoadGroupsInDir uses extension order followed by sorted filenames.
        static const char *const extensions[] = {".grp", ".zip", ".ssi", ".pk3", ".pk4"};
        if (explicitDirectory && g_modDir[0] != '/')
            for (auto const *extension : extensions)
                for (auto const &file : profile.files)
                    if (file.path.find('/') == std::string::npos && lower(file.path).size() >= std::strlen(extension) &&
                        lower(file.path).compare(file.path.size() - std::strlen(extension), std::strlen(extension), extension) == 0)
                        profile.selections.push_back({netaddon::SelectionKind::PreloadArchive, file.path});
        for (auto const &path : commandArchives)
            if (!selection(netaddon::SelectionKind::Archive, path, true, error)) return false;
        if (!selection(netaddon::SelectionKind::Ini, BloodIniFile, true, error)) return false;
        if (pUserRFF && !selection(netaddon::SelectionKind::Rff, pUserRFF, true, error)) return false;
        if (pUserSoundRFF && !selection(netaddon::SelectionKind::Snd, pUserSoundRFF, true, error)) return false;
        if (explicitDefinition || g_defNamePtr)
        { if (!selection(netaddon::SelectionKind::Definition, G_DefFile(), true, error)) return false; }
        else if (!resolve(G_DefFile()).empty())
        { if (!selection(netaddon::SelectionKind::Definition, G_DefFile(), false, error)) return false; }
        for (auto const *path : g_defModules)
            if (!selection(netaddon::SelectionKind::DefinitionModule, path, true, error)) return false;
        if (pUserTiles)
        {
            auto const name = logical(pUserTiles, {});
            if (!netaddon::validPath(name)) { error = "Addon ART prefix is not portable"; return false; }
            profile.selections.push_back({netaddon::SelectionKind::Art, name});
            for (unsigned index = 0; index < MAXARTFILES_BASE; ++index)
            {
                char suffix[16]; Bsnprintf(suffix, sizeof(suffix), "%03u.ART", index);
                auto const source = resolve(std::string(pUserTiles) + suffix);
                if (!source.empty() && !addFile(name + suffix, source, error)) return false;
            }
        }
        // Loose DEF/RFS dependencies outside the selected directory must follow
        // their original relative paths. Archive-internal dependencies stay packed.
        for (auto const &asset : netContentManifest())
        {
            if ((asset.key.compare(0, 7, "source:") != 0 && asset.key.compare(0, 9, "resource:") != 0) || originalName(asset.name)) continue;
            auto const source = resolve(asset.name);
            if (!source.empty() && !addFile(logical(asset.name, source), source, error)) return false;
        }
        // Keep profile identity independent of enumeration order; sources stay
        // paired with their files for bounded network read requests.
        std::vector<std::pair<netaddon::File, std::string>> sorted;
        for (size_t i = 0; i < profile.files.size(); ++i) sorted.emplace_back(std::move(profile.files[i]), std::move(sourceFiles[i]));
        std::sort(sorted.begin(), sorted.end(), [](const std::pair<netaddon::File, std::string> &a,
                                                const std::pair<netaddon::File, std::string> &b) { return a.first.path < b.first.path; });
        profile.files.clear(); sourceFiles.clear();
        for (auto &entry : sorted) { profile.files.push_back(std::move(entry.first)); sourceFiles.push_back(std::move(entry.second)); }
        return netaddon::validate(profile, error);
    };
    bool const success = build();
    if (!success) profileError = error;
    return success;
}

const netaddon::Profile &netAddonProfile() { return profile; }
const std::string &netAddonActiveDirectory() { return addonDirectory; }
const std::string &netAddonSource(size_t index)
{ static const std::string empty; return index < sourceFiles.size() ? sourceFiles[index] : empty; }

bool netAddonFindInstalled(const netaddon::Profile &required, std::string &found, std::string &error)
{
    if (!netaddon::validate(required, error)) return false;
    if (required.files.empty()) { found.clear(); return true; }
    std::vector<std::string> roots;
    candidate(roots, addonDirectory);
    candidate(roots, startupDirectory);
    candidate(roots, ".");
#ifndef USE_PHYSFS
    for (char *path = listsearchpath(1); path && roots.size() < 256; path = listsearchpath(0)) candidate(roots, path);
#endif
    auto const initial = roots;
    for (auto const &root : initial)
    {
        children(roots, root + "/mods"); children(roots, root + "/addons");
        // Most Blood addon instructions install a named directory beside the game.
        if (root == startupDirectory) children(roots, root);
    }
    std::string ignored;
    for (auto const &root : roots)
        if (netaddon::matches(root, required, ignored)) { found = root; error.clear(); return true; }
    error.clear(); found.clear(); return false;
}

bool netAddonResourceArguments(const netaddon::Profile &required, const std::string &root,
                              std::vector<std::string> &args, std::string &error)
{
    if (!netaddon::validate(required, error)) return false;
    args = {"-game_dir", "/", "-noautoload"};
    if (!root.empty()) { args.push_back("-j"); args.push_back(root); }
    for (auto const &item : required.selections)
    {
        const char *option = nullptr;
        switch (item.kind)
        {
            case netaddon::SelectionKind::Ini: option = "-ini"; break;
            case netaddon::SelectionKind::Rff: option = "-rff"; break;
            case netaddon::SelectionKind::Snd: option = "-snd"; break;
            case netaddon::SelectionKind::Art: option = "-art"; break;
            case netaddon::SelectionKind::Definition: option = "-h"; break;
            case netaddon::SelectionKind::DefinitionModule: option = "-mh"; break;
            case netaddon::SelectionKind::Archive: option = "-g"; break;
            case netaddon::SelectionKind::PreloadArchive: continue; // Restored by G_LoadGroups before first-pass DEF parsing.
        }
        if (!option) { error = "Unknown addon resource selection"; return false; }
        args.push_back(option); args.push_back(item.path);
    }
    return true;
}
