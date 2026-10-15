// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "rnet_addon.h"
#include <cstddef>
#include <cstdint>
#include <string>

// Called only by the private, locally generated restart handoff.
bool rnetAddonActivateProfile(const rnetaddon::Profile&, const std::string&, std::string&);
const std::string& rnetAddonActiveDirectory();
// Common startup code is shared with Mapster; its optional callbacks avoid
// introducing networking dependencies into the editor executable.
extern void (*rnetTrackGroup)(const char*, bool, bool);
extern bool (*rnetPreloadArchives)(std::string&);

namespace rnet { namespace content {
bool earlyArguments(int& argc, const char* const*& argv, std::string& error);
void captureArguments(int argc, const char* const* argv);
void startup();
void offer(int player);
void handle(int player, uint8_t type, const uint8_t* bytes, size_t size);
void pump();
void mapChanged();
void mapLoaded(char searchFirst);
void shutdown();
const char* resolveMap(const char* name);
bool validateMapForLoad(const char* name, std::string& error, char searchFirst = 0);
bool restartPending();
bool restartReady();
bool restartActive();
bool launchRestart(std::string& error);
// Capture actual successful loads, preserving group precedence. Base archives
// are required locally and are never transferred. DEF loadgrp is a preload.
void trackGroup(const char* path, bool base = false, bool preload = false);
void trackFile(const char* path);
void captureSelections();
bool preloadArchives(std::string& error);
} }
