#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the native Linux URL registration code without game assets.

Requires a C++ compiler, gio and desktop-file-validate. All desktop registration
is isolated under temporary HOME/XDG directories; the user's handler is untouched.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
HELPER = r'''
#include "neturireg.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>
int main(int argc, char** argv)
{
    std::string error;
    if (argc == 1)
    {
        if (neturi::registerHandler(error)) return 0;
        std::cerr << error << '\n';
        return 1;
    }
    if (!neturi::prepareLaunch(error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    char* cwd = getcwd(nullptr, 0);
    if (!cwd) return 2;
    std::ofstream file(std::getenv("NETBLOOD_URI_TEST_LOG"), std::ios::binary);
    file << cwd << '\0';
    for (int i = 0; i < argc; ++i) file << argv[i] << '\0';
    std::free(cwd);
    return file ? 0 : 3;
}
'''


def run(argv, env, **kwargs):
    return subprocess.run(argv, env=env, text=True, capture_output=True, **kwargs)


def main():
    for tool in (os.environ.get("CXX", "c++"), "gio", "desktop-file-validate"):
        if not shutil.which(tool):
            raise RuntimeError(f"Required test tool missing: {tool}")
    with tempfile.TemporaryDirectory(prefix="netblood-uri-registration-") as temporary:
        base = Path(temporary)
        source = base / "registration.cpp"
        source.write_text(HELPER)
        executable = base / "registration"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "source/blood/src"), str(source),
            str(ROOT / "source/blood/src/neturireg.cpp"), "-o", str(executable),
        ], check=True)
        tools = base / "tools"
        tools.mkdir()
        fake = tools / "xdg-mime"
        fake.write_text("#!/usr/bin/python3\nimport json, os, sys\n"
                        "open(os.environ['NETBLOOD_XDG_TEST_LOG'], 'w').write(json.dumps(sys.argv[1:]))\n"
                        "sys.exit(int(os.environ.get('NETBLOOD_XDG_TEST_STATUS', '0')))\n")
        fake.chmod(0o755)
        home = base / "home"
        home.mkdir()
        env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(base / "data"),
                   XDG_CONFIG_HOME=str(base / "config"),
                   PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
                   NETBLOOD_XDG_TEST_LOG=str(base / "xdg.json"),
                   NETBLOOD_URI_TEST_LOG=str(base / "launched"))
        expected = ["default", "netblood.desktop", "x-scheme-handler/netblood"]
        uri = "netblood://example.com:23513"
        directories = ["plain", "a space", 'a "quote"', "a \\slash", "$dollar `tick`",
                       "percent %f %u %%", "a=b", "caf\u00e9", "trailing space "]
        for name in directories:
            directory = base / name
            directory.mkdir()
            installed = directory / "NetBlood"
            shutil.copyfile(executable, installed)
            installed.chmod(0o755)
            result = run([str(installed)], env, cwd="/")
            assert result.returncode == 0, (name, result.stderr)
            assert json.loads(Path(env["NETBLOOD_XDG_TEST_LOG"]).read_text()) == expected
            desktop = Path(env["XDG_DATA_HOME"]) / "applications/netblood.desktop"
            result = run(["desktop-file-validate", str(desktop)], env)
            assert result.returncode == 0, (name, result.stdout, result.stderr, desktop.read_text())
            launch_log = Path(env["NETBLOOD_URI_TEST_LOG"])
            launch_log.unlink(missing_ok=True)
            result = run(["gio", "launch", str(desktop), uri], env, cwd="/")
            assert result.returncode == 0, (name, result.stderr, desktop.read_text())
            deadline = time.monotonic() + 5
            expected_launch = [str(directory), str(installed), "-connect-uri", uri, ""]
            actual = None
            while time.monotonic() < deadline:
                if launch_log.exists():
                    actual = launch_log.read_bytes().decode().split("\0")
                    if actual == expected_launch:
                        break
                time.sleep(0.02)
            assert actual == expected_launch, (name, actual)
        directory = base / "unsafe%=path"
        directory.mkdir()
        installed = directory / "NetBlood"
        shutil.copyfile(executable, installed)
        installed.chmod(0o755)
        result = run([str(installed)], env)
        assert result.returncode == 1 and "contains both '%' and '='" in result.stderr
        fallback = dict(env)
        fallback.pop("XDG_DATA_HOME")
        assert run([str(executable)], fallback).returncode == 0
        assert (home / ".local/share/applications/netblood.desktop").is_file()
        for bad in ("relative", str(base / "bad\npath")):
            invalid = dict(env, XDG_DATA_HOME=bad)
            result = run([str(executable)], invalid)
            assert result.returncode == 1 and result.stderr.strip(), result
        invalid = dict(env)
        invalid.pop("HOME")
        invalid.pop("XDG_DATA_HOME")
        assert "Set HOME" in run([str(executable)], invalid).stderr
        invalid = dict(env, PATH=str(base / "missing-tools"))
        assert "xdg-mime is missing" in run([str(executable)], invalid).stderr
        invalid = dict(env, NETBLOOD_XDG_TEST_STATUS="4")
        assert "exit status 4" in run([str(executable)], invalid).stderr
        blocked = base / "not-a-directory"
        blocked.write_text("file")
        invalid = dict(env, XDG_DATA_HOME=str(blocked))
        assert "not a directory" in run([str(executable)], invalid).stderr
        print(f"URL registration: {len(directories)} GIO launches and registration failure cases passed")


if __name__ == "__main__":
    main()
