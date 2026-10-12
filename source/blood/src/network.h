//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT

This file is part of NBlood.

NBlood is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#pragma once
#include "compat.h"
#include "build.h"
#include "controls.h"

enum PACKETMODE {
    PACKETMODE_0 = 0,
    PACKETMODE_1,
    PACKETMODE_2,
    PACKETMODE_3,
};

enum NETWORKMODE {
    NETWORK_NONE = 0,
    NETWORK_SERVER,
    NETWORK_CLIENT
};

enum PLAYERKEYSMODE {
    LOSTONDEATH = 0,
    KEPTONRESPAWN,
    SHARED
};

#define kNetDefaultPort 23513

extern bool gStartNewGame;
extern PACKETMODE gPacketMode;
extern ClockTicks gNetFifoClock;
extern int gNetFifoTail;
extern int gNetFifoHead[8];
extern int gPredictTail;
extern int gNetFifoMasterTail;
extern GINPUT gFifoInput[256][8];
extern int myMinLag[8];
extern int otherMinLag;
extern int myMaxLag;
extern unsigned int gChecksum[4];
extern unsigned int gCheckFifo[256][8][4];
extern int gCheckHead[8];
extern int gSendCheckTail;
extern int gCheckTail;
extern int gInitialNetPlayers;
extern int gBufferJitter;
extern int gPlayerReady[8];
extern bool bNoResend;
extern bool gRobust;
extern bool bOutOfSync;
extern bool ready2send;
extern NETWORKMODE gNetMode;
extern char gNetAddress[32];
extern int gNetPort;


struct PKT_STARTGAME {
    short version;
    char gameType, difficulty, monsterSettings, weaponSettings, itemSettings, respawnSettings;
    char episodeId, levelId;
    int unk;
    char userMap, userMapName[BMAX_PATH];
    int weaponsV10x;
    bool bFriendlyFire;
    PLAYERKEYSMODE bPlayerKeys;
};

extern PKT_STARTGAME gPacketStartGame;


void netResetState(void);
void netResetToSinglePlayer(void);
void netBroadcastMessage(int nPlayer, const char *pzMessage);
void netWaitForEveryone(char a1);
void netBroadcastFrag(const char *pzString);
void netGetPackets(void);
void netBroadcastTaunt(int nPlayer, int nTaunt);
void CalcGameChecksum(void);
void netBroadcastPlayerLogoff(int nPlayer);
void netBroadcastMyLogoff(bool bRestart);
void netInitialize(bool bConsole);
void netBroadcastPlayerInfo(int nPlayer);
void netCheckSync(void);
void netMasterUpdate(void);
void netGetInput(void);
void netPlayerQuit(int nPlayer);
void netUpdate(void);
void netDeinitialize(void);
void netBroadcastNewGame(void);

// Authoritative protocol lifecycle; offline FIFO/demo behavior is unchanged.
extern int gNetPacketCopies;
extern int gNetSnapshotRate;
bool netIsLive();
bool netIsServer();
bool netIsClient();
bool netClientHasSnapshot();
bool netServerBeginTick();
void netServerEndTick();
void netOnLevelStarted();
void netClientPump();
void netPrintStatus();
void netDisconnect();
bool netStartPacketRecording(const char *path);
void netStopPacketRecording();
bool netStartPacketPlayback(const char *path);
bool netDemoReplayPacket(const uint8_t *data, size_t size);

void netBroadcastEvent(const uint8_t *data, size_t size);
bool netInServerTick();
uint32_t netAppliedCommandSequence(int player);
void netPrepareCurrentMapSettings();
