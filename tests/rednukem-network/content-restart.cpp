// Exercise the real process-restart handoff without game assets or a display.
#include "rnet_restart.h"
#include "rnet_content.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
unsigned checks;
void check(bool result, char const* expression, int line)
{
    ++checks;
    if (!result) { std::fprintf(stderr, "FAIL %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
std::string const host = "201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e";
std::string const password = "a space \" quote \\ $(touch password-executed)";
std::string activatedRoot;
std::string previousAddonRoot;
rnetaddon::Profile activated;

std::string cwd()
{
    char path[4096];
    CHECK(getcwd(path, sizeof(path)) != nullptr);
    return path;
}
rnetaddon::Profile profile()
{
    rnetaddon::Profile value;
    value.name = "Restart test";
    value.selections.push_back({rnetaddon::SelectionKind::Script, "TEST.CON"});
    return value;
}
bool contains(std::vector<std::string> const& arguments, std::string const& value)
{
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}
}

// The real engine adapter is exercised by addon-join.py. This hook verifies
// that the exact validated manifest reaches startup before resource loading.
bool rnetAddonActivateProfile(rnetaddon::Profile const& value, std::string const& root, std::string&)
{
    activated = value;
    activatedRoot = root;
    return true;
}
std::string const& rnetAddonActiveDirectory() { return previousAddonRoot; }

int main(int argc, char** raw)
{
    bool const restarted = argc > 1 && std::strncmp(raw[1], "-addon-restart=", 15) == 0;
    std::string const handoff = restarted ? std::string(raw[1]).substr(15) : "";
    char const* const* argv = raw;
    std::string error;
    CHECK(rnetAddonRestartInitialize(argc, argv, error));
    if (restarted)
    {
        CHECK(rnetAddonRestartActive());
        CHECK(std::getenv("DUKE3DDEF") == nullptr || !*std::getenv("DUKE3DDEF"));
        std::vector<std::string> arguments(argv + 1, argv + argc);
        CHECK(access(handoff.c_str(), F_OK) != 0);
        CHECK(contains(arguments, "-usecwd"));
        CHECK(contains(arguments, "-cfg")); CHECK(contains(arguments, "user settings.cfg"));
        CHECK(contains(arguments, "-j")); CHECK(contains(arguments, "/base/data"));
        CHECK(contains(arguments, "-stunserver")); CHECK(contains(arguments, "off"));
        CHECK(contains(arguments, "-pname")); CHECK(contains(arguments, "LongPlayerName"));
        CHECK(contains(arguments, "-x")); CHECK(contains(arguments, "TEST.CON"));
        CHECK(contains(arguments, "-h")); CHECK(contains(arguments, "effects.def"));
        CHECK(contains(arguments, "-g")); CHECK(contains(arguments, "data.pk3"));
        CHECK(contains(arguments, cwd() + "/addon $(touch path-executed)"));
        CHECK(contains(arguments, "-connect")); CHECK(contains(arguments, "[" + host + "]:23514"));
        CHECK(contains(arguments, "-password")); CHECK(contains(arguments, password));
        CHECK(contains(arguments, "-nologo"));
        CHECK(contains(arguments, "-nosetup"));
        CHECK(contains(arguments, "-noautoload"));
        for (auto const& argument : arguments)
        {
            CHECK(argument.find("OLD") == std::string::npos);
            CHECK(argument.find("dedicated") == std::string::npos);
            CHECK(argument.find("episode") == std::string::npos);
            CHECK(argument.find("playback") == std::string::npos);
        }
        CHECK(rnetaddon::identity(activated) == rnetaddon::identity(profile()));
        CHECK(activatedRoot == cwd());
        CHECK(access("password-executed", F_OK) != 0);
        CHECK(access("path-executed", F_OK) != 0);
        rnetAddonRestartRememberStartup(argc, argv);
        CHECK(!rnetAddonRestartRequest({"-x", "TEST.CON"}, host, 23514, password,
            rnetaddon::identity(profile()), profile(), cwd(), error));
        CHECK(error.find("still incompatible") != std::string::npos);
        CHECK(!rnetAddonRestartPending());
        std::printf("Addon restarted process: %u checks passed\n", checks);
        return 0;
    }

    rnetaddon::Profile const addon = profile();
    std::string const identity = rnetaddon::identity(addon);
    std::string const directory = cwd();
    previousAddonRoot = directory + "/OLD-DIRECTORY";
    CHECK(mkdir(previousAddonRoot.c_str(), 0700) == 0);
    char const* local[] = {raw[0], "-usecwd", "-cfg", "user settings.cfg", "-j=/base/data", "-pname", "LongPlayerName",
        "-j", "OLD-DIRECTORY", "-j", "./OLD-DIRECTORY/",
        "-game_dir", "OLD-DIRECTORY", "-g", "OLD.pk3", "OLD.zip", "-x", "OLD.CON", "-h=OLD.def", "-mh", "OLD-extra.def",
        "-rff=OLD.RFF", "-snd=OLD-SOUNDS.RFF", "-art=OLD-TILES", "-client=OLD.invalid", "-port=1111", "-password=OLD-password",
        "-dedicated", "-server", "8", "-episode", "2", "-playback", "OLD.DEM", "-stunserver", "off"};
    rnetAddonRestartRememberStartup(int(sizeof(local) / sizeof(local[0])), local);
    CHECK(!rnetAddonRestartActive());
    CHECK(!rnetAddonRestartPending());
    CHECK(setenv("DUKE3DDEF", "OLD-environment.def", 1) == 0);
    CHECK(!rnetAddonRestartReady());
    CHECK(!rnetAddonRestartRequest({"-client", "evil.invalid"}, host, 23514, password, identity, addon, directory, error));
    CHECK(!rnetAddonRestartRequest({"-x", "TEST.CON"}, "bad host", 23514, password, identity, addon, directory, error));
    CHECK(!rnetAddonRestartRequest({"-x", "TEST.CON"}, host, 0, password, identity, addon, directory, error));
    CHECK(!rnetAddonRestartRequest({"-x", "TEST.CON"}, host, 23514, std::string(129, 'x'), identity, addon, directory, error));
    CHECK(!rnetAddonRestartRequest({"-x", "TEST.CON"}, host, 23514, password, std::string(32, '0'), addon, directory, error));
    CHECK(!rnetAddonRestartRequest({"-x", "TEST\n.CON"}, host, 23514, password, identity, addon, directory, error));
    CHECK(!rnetAddonRestartRequest({}, host, 23514, password, identity, addon, directory, error));
    CHECK(!rnetAddonRestartPending());

    std::string const malformed = directory + "/malformed";
    { std::ofstream file(malformed); file << "not a handoff"; CHECK(bool(file)); }
    CHECK(chmod(malformed.c_str(), 0644) == 0);
    std::string option = "-addon-restart=" + malformed;
    char const* badArguments[] = {raw[0], option.c_str()};
    char const* const* bad = badArguments;
    int badCount = 2;
    CHECK(!rnetAddonRestartInitialize(badCount, bad, error));
    CHECK(access(malformed.c_str(), F_OK) == 0); // Unsafe files are never consumed.
    CHECK(chmod(malformed.c_str(), 0600) == 0);
    CHECK(!rnetAddonRestartInitialize(badCount, bad, error));
    CHECK(access(malformed.c_str(), F_OK) != 0); // Private malformed handoffs are consumed.
    CHECK(mkfifo(malformed.c_str(), 0600) == 0);
    CHECK(!rnetAddonRestartInitialize(badCount, bad, error));
    CHECK(unlink(malformed.c_str()) == 0); // A nonregular handoff must not block.

    std::vector<std::string> const resources = {"-j", directory + "/addon $(touch path-executed)",
        "-x", "TEST.CON", "-h", "effects.def", "-g", "data.pk3", "-noautoload"};
#if defined(__linux__)
    // A disappearing AppImage/executable must leave a useful error and remove
    // its private password handoff, without asking a shell to launch anything.
    pid_t const failedLaunch = fork();
    CHECK(failedLaunch >= 0);
    if (!failedLaunch)
    {
        std::string const image = directory + "/test.AppImage";
        { std::ofstream file(image); file << "not executed"; CHECK(bool(file)); }
        CHECK(chmod(image.c_str(), 0700) == 0);
        CHECK(setenv("APPIMAGE", image.c_str(), 1) == 0);
        int one = 1;
        char const* const* original = raw;
        CHECK(rnetAddonRestartInitialize(one, original, error));
        CHECK(rnetAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
        CHECK(chmod(image.c_str(), 0600) == 0);
        CHECK(!rnetAddonRestartLaunch(error));
        CHECK(error.find("Cannot restart Rednukem") != std::string::npos);
        CHECK(!rnetAddonRestartPending());
        DIR* files = opendir(std::getenv("TMPDIR"));
        CHECK(files);
        while (dirent* entry = readdir(files))
            CHECK(std::strncmp(entry->d_name, "rednukem-addon-restart-", 22) != 0);
        closedir(files);
        std::printf("Addon failed launch: %u checks passed\n", checks);
        return 0;
    }
    int failureStatus;
    CHECK(waitpid(failedLaunch, &failureStatus, 0) == failedLaunch);
    CHECK(WIFEXITED(failureStatus) && WEXITSTATUS(failureStatus) == 0);
#endif
    CHECK(rnetAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
    CHECK(rnetAddonRestartPending());
    CHECK(!rnetAddonRestartReady()); // The UI gets a real display interval.
    CHECK(!rnetAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
    pid_t const child = fork();
    CHECK(child >= 0);
    if (!child)
    {
        // The game also may change working directory while shutting down.
        CHECK(chdir("/") == 0);
        if (!rnetAddonRestartLaunch(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        return 1; // execv must replace this process.
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::printf("Addon restart parent: %u checks passed\n", checks);
}
