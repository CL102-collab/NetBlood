// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "netassets.h"

// Retains the complete report after network teardown; never opens asset paths.
void netContentReportShow(std::vector<netassets::Issue> issues, const std::string &reason);
void netContentReportClear();
// Draws a paginated report when no menu is covering it. Returns true if drawn.
bool netContentReportDraw();
