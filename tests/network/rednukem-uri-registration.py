#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the shared platform URL-handler suite against Rednukem's handler.

All associations and launch probes use temporary HOME and XDG directories.
The suite covers desktop quoting, percent signs, AppImage paths, errors, and
Discord launch registration without changing the user's associations.
"""
from pathlib import Path

suite = Path(__file__).with_name("uri-registration.py").read_text()
suite = suite.replace('"neturireg.h"', '"rnet_uri.h"')
suite = suite.replace("neturi::", "rnet::uri::")
suite = suite.replace("source/blood/src/neturireg.cpp", "source/rr/src/rnet_urireg.cpp")
suite = suite.replace("source/blood/src", "source/rr/src")
suite = suite.replace("NetBlood", "Rednukem").replace("netblood", "rednukem")
exec(compile(suite, str(Path(__file__).with_name("uri-registration.py")), "exec"), globals())
