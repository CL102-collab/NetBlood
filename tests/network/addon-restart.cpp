// Exercise the real process-restart handoff without game assets or a display.
#include "netaddon_restart.h"
#include "netaddon_engine.h"

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
std::string const zeroTierNetwork = "8056c2e21c000001";
std::string activatedRoot;
std::string previousAddonRoot;
netaddon::Profile activated;

std::string cwd()
{
    char path[4096];
    CHECK(getcwd(path, sizeof(path)) != nullptr);
    return path;
}
netaddon::Profile profile()
{
    netaddon::Profile value;
    value.name = "Restart test";
    value.selections.push_back({netaddon::SelectionKind::Ini, "TEST.INI"});
    return value;
}
bool contains(std::vector<std::string> const& arguments, std::string const& value)
{
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}
}

// The real engine adapter is exercised by addon-join.py. This hook verifies
// that the exact validated manifest reaches startup before resource loading.
bool netAddonActivateProfile(netaddon::Profile const& value, std::string const& root, std::string&)
{
    activated = value;
    activatedRoot = root;
    return true;
}
std::string const& netAddonActiveDirectory() { return previousAddonRoot; }

int main(int argc, char** raw)
{
    bool const restarted = argc > 1 && std::strncmp(raw[1], "-addon-restart=", 15) == 0;
    std::string const handoff = restarted ? std::string(raw[1]).substr(15) : "";
    char const* const* argv = raw;
    std::string error;
    CHECK(netAddonRestartInitialize(argc, argv, error));
    if (restarted)
    {
        CHECK(netAddonRestartActive());
        CHECK(std::getenv("BLOODDEF") == nullptr || !*std::getenv("BLOODDEF"));
        std::vector<std::string> arguments(argv + 1, argv + argc);
        CHECK(access(handoff.c_str(), F_OK) != 0);
        CHECK(contains(arguments, "-usecwd"));
        CHECK(contains(arguments, "-cfg=user settings.cfg"));
        CHECK(contains(arguments, "-j=/base/data"));
        CHECK(contains(arguments, "-stunserver=off"));
        CHECK(contains(arguments, "-pname=LongPlayerName"));
        CHECK(contains(arguments, "-ini=TEST.INI"));
        CHECK(contains(arguments, "-h=effects.def"));
        CHECK(contains(arguments, "-g=data.pk3"));
        CHECK(contains(arguments, "-game_dir=/"));
        CHECK(contains(arguments, "-j=" + cwd() + "/addon $(touch path-executed)"));
        CHECK(contains(arguments, "-client=" + host));
        CHECK(contains(arguments, "-port=23514"));
        CHECK(contains(arguments, "-password=" + password));
        char const* expectedNetwork = std::getenv("NETBLOOD_TEST_EXPECT_ZEROTIER");
        unsigned overlayOptions = 0;
        for (auto const& argument : arguments)
            if (argument.compare(0, 10, "-zerotier=") == 0)
            {
                ++overlayOptions;
                CHECK(expectedNetwork && argument == "-zerotier=" + std::string(expectedNetwork));
            }
        CHECK(overlayOptions == (expectedNetwork ? 1u : 0u));
        CHECK(contains(arguments, "-quick"));
        CHECK(contains(arguments, "-nodemo"));
        CHECK(contains(arguments, "-nosetup"));
        CHECK(contains(arguments, "-noautoload"));
        for (auto const& argument : arguments)
        {
            CHECK(argument.find("OLD") == std::string::npos);
            CHECK(argument.find("dedicated") == std::string::npos);
            CHECK(argument.find("episode") == std::string::npos);
            CHECK(argument.find("playback") == std::string::npos);
        }
        CHECK(netaddon::identity(activated) == netaddon::identity(profile()));
        CHECK(activatedRoot == cwd());
        CHECK(access("password-executed", F_OK) != 0);
        CHECK(access("path-executed", F_OK) != 0);
        netAddonRestartRememberStartup(argc, argv);
        CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, host, 23514, password,
            netaddon::identity(profile()), profile(), cwd(), error));
        CHECK(error.find("still incompatible") != std::string::npos);
        CHECK(!netAddonRestartPending());
        std::printf("Addon restarted process: %u checks passed\n", checks);
        return 0;
    }

    netaddon::Profile const addon = profile();
    std::string const identity = netaddon::identity(addon);
    std::string const directory = cwd();
    previousAddonRoot = directory + "/OLD-DIRECTORY";
    CHECK(mkdir(previousAddonRoot.c_str(), 0700) == 0);
    char const* local[] = {raw[0], "-usecwd", "-cfg", "user settings.cfg", "-j=/base/data", "-pname", "LongPlayerName",
        "-j", "OLD-DIRECTORY", "-j", "./OLD-DIRECTORY/",
        "-game_dir", "OLD-DIRECTORY", "-g", "OLD.pk3", "OLD.zip", "-ini", "OLD.INI", "-h=OLD.def", "-mh", "OLD-extra.def",
        "-rff=OLD.RFF", "-snd=OLD-SOUNDS.RFF", "-art=OLD-TILES", "-client=OLD.invalid", "-port=1111", "-password=OLD-password",
        "-dedicated", "-server", "8", "-episode", "2", "-playback", "OLD.DEM", "-stunserver", "off",
        "-zerotier", "OLD-network", "-zerotier=OLD-network"};
    netAddonRestartRememberStartup(int(sizeof(local) / sizeof(local[0])), local);
    CHECK(!netAddonRestartActive());
    CHECK(!netAddonRestartPending());
    CHECK(setenv("BLOODDEF", "OLD-environment.def", 1) == 0);
    CHECK(!netAddonRestartReady());
    CHECK(!netAddonRestartRequest({"-client", "evil.invalid"}, host, 23514, password, identity, addon, directory, error));
    CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, "bad host", 23514, password, identity, addon, directory, error));
    CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, host, 0, password, identity, addon, directory, error));
    CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, host, 23514, std::string(129, 'x'), identity, addon, directory, error));
    CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, host, 23514, password, std::string(32, '0'), addon, directory, error));
    CHECK(!netAddonRestartRequest({"-ini", "TEST\n.INI"}, host, 23514, password, identity, addon, directory, error));
    CHECK(!netAddonRestartRequest({}, host, 23514, password, identity, addon, directory, error));
    for (std::string const& invalid : {std::string("short"), std::string(17, 'a'),
        std::string("8056c2e21c00000g"), std::string("8056c2e21c00000\n"),
        std::string("8056c2e2\0c000001", 16)})
    {
        CHECK(!netAddonRestartRequest({"-ini", "TEST.INI"}, host, 23514, password,
            identity, addon, directory, error, invalid));
        CHECK(error.find("ZeroTier network ID") != std::string::npos);
        CHECK(!netAddonRestartPending());
    }
    CHECK(!netAddonRestartPending());

    std::string const malformed = directory + "/malformed";
    { std::ofstream file(malformed); file << "not a handoff"; CHECK(bool(file)); }
    CHECK(chmod(malformed.c_str(), 0644) == 0);
    std::string option = "-addon-restart=" + malformed;
    char const* badArguments[] = {raw[0], option.c_str()};
    char const* const* bad = badArguments;
    int badCount = 2;
    CHECK(!netAddonRestartInitialize(badCount, bad, error));
    CHECK(access(malformed.c_str(), F_OK) == 0); // Unsafe files are never consumed.
    CHECK(chmod(malformed.c_str(), 0600) == 0);
    CHECK(!netAddonRestartInitialize(badCount, bad, error));
    CHECK(access(malformed.c_str(), F_OK) != 0); // Private malformed handoffs are consumed.
    CHECK(mkfifo(malformed.c_str(), 0600) == 0);
    CHECK(!netAddonRestartInitialize(badCount, bad, error));
    CHECK(unlink(malformed.c_str()) == 0); // A nonregular handoff must not block.

    std::vector<std::string> const resources = {"-game_dir", "/", "-j", directory + "/addon $(touch path-executed)",
        "-ini", "TEST.INI", "-h", "effects.def", "-g", "data.pk3", "-noautoload"};
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
        CHECK(netAddonRestartInitialize(one, original, error));
        CHECK(netAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
        CHECK(chmod(image.c_str(), 0600) == 0);
        CHECK(!netAddonRestartLaunch(error));
        CHECK(error.find("Cannot restart NetBlood") != std::string::npos);
        CHECK(!netAddonRestartPending());
        DIR* files = opendir(std::getenv("TMPDIR"));
        CHECK(files);
        while (dirent* entry = readdir(files))
            CHECK(std::strncmp(entry->d_name, "netblood-addon-restart-", 22) != 0);
        closedir(files);
        std::printf("Addon failed launch: %u checks passed\n", checks);
        return 0;
    }
    int failureStatus;
    CHECK(waitpid(failedLaunch, &failureStatus, 0) == failedLaunch);
    CHECK(WIFEXITED(failureStatus) && WEXITSTATUS(failureStatus) == 0);
#endif
    // A downloaded addon must reconnect over the same virtual network, while
    // a later ordinary Internet restart must discard stale startup overlay
    // arguments. Exercise both through the production private handoff and exec.
    pid_t const overlayChild = fork();
    CHECK(overlayChild >= 0);
    if (!overlayChild)
    {
        CHECK(setenv("NETBLOOD_TEST_EXPECT_ZEROTIER", zeroTierNetwork.c_str(), 1) == 0);
        CHECK(netAddonRestartRequest(resources, host, 23514, password, identity,
            addon, directory, error, zeroTierNetwork));
        CHECK(netAddonRestartPending());
        CHECK(chdir("/") == 0);
        if (!netAddonRestartLaunch(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        return 1;
    }
    int overlayStatus;
    CHECK(waitpid(overlayChild, &overlayStatus, 0) == overlayChild);
    CHECK(WIFEXITED(overlayStatus) && WEXITSTATUS(overlayStatus) == 0);
    CHECK(netAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
    CHECK(netAddonRestartPending());
    CHECK(!netAddonRestartReady()); // The UI gets a real display interval.
    CHECK(!netAddonRestartRequest(resources, host, 23514, password, identity, addon, directory, error));
    pid_t const child = fork();
    CHECK(child >= 0);
    if (!child)
    {
        // The game also may change working directory while shutting down.
        CHECK(chdir("/") == 0);
        if (!netAddonRestartLaunch(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        return 1; // execv must replace this process.
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::printf("Addon restart parent: %u checks passed\n", checks);
}
