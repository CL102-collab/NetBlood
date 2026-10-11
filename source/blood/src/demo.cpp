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
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "common_game.h"
#include "keyboard.h"
#include "control.h"
#include "osd.h"
#include "mmulti.h"

#include "blood.h"
#include "controls.h"
#include "demo.h"
#include "demo_validate.h"
#include "fire.h"
#include "gamemenu.h"
#include "globals.h"
#include "levels.h"
#include "menu.h"
#include "messages.h"
#include "misc.h"
#include "music.h"
#include "network.h"
#include "player.h"
#include "screen.h"
#include "sound.h"
#include "view.h"

bool gDemoRunValidation = false;
bool gDemoValidationPassed = false;

static int nValidationPassed = 0;
static int nValidationSkipped = 0;
static uint32_t nValidationInputs = 0;

EDUKE32_NORETURN void DemoValidationError(const char *message, const char *demoName)
{
    LOG_F(ERROR, "1.21 Validation Failed: %s%s%s", message, demoName ? ": " : "", demoName ? demoName : "");
    ShutDown();
    Bexit(1);
}

static const char *DemoValidationSkipReason(const char *name)
{
    // Preserve NotBlood's compiler exclusions; never derive skips from a failed run.
#if !defined USING_LTO
    if (!Bstrcasecmp(name, "/validatedemos/TEST038.DEM"))
        return "upstream exclusion without LTO";
#endif
#if defined(__GNUC__) && __GNUC__ >= 15
    if (!Bstrcasecmp(name, "/validatedemos/TEST085.DEM")
        || !Bstrcasecmp(name, "/validatedemos/TEST096.DEM")
        || !Bstrcasecmp(name, "/validatedemos/TEST098.DEM"))
        return "upstream exclusion with GCC 15 or newer";
#endif
    UNREFERENCED_PARAMETER(name);
    return NULL;
}

static const DEMOVALIDATE *DemoValidationInfo(const char *name)
{
    for (auto const &entry : gDemoValidate)
        if (!Bstrcasecmp(name, entry.zName))
            return &entry;
    return NULL;
}

static void DemoValidationCheckHeader(const DEMOHEADER &header, int fileLength, const char *name)
{
    auto const *info = DemoValidationInfo(name);
    if (!info)
        DemoValidationError("No expected result for demo", name);
    if (header.signature != 0x1a4d4544 || header.nVersion != BloodVersion)
        DemoValidationError("Expected a DOS 1.21 demo", name);
    if (header.nInputCount <= 0 || header.nInputCount != info->nInputTicks)
        DemoValidationError("Recorded input count does not match baseline", name);
    if (header.nNetPlayers != 1 || header.nMyConnectIndex != 0
        || header.nConnectHead != 0 || header.connectPoints[0] != -1)
        DemoValidationError("Expected a single-player demo", name);
    // Some upstream fixtures intentionally shorten nInputCount to a checkpoint
    // while retaining the remaining complete input records in the file.
    int const inputBytes = fileLength - int(sizeof(DEMOHEADER) + sizeof(GAMEOPTIONSLEGACY));
    if (inputBytes < 22 * header.nInputCount || inputBytes % 22 != 0)
        DemoValidationError("Truncated demo inputs", name);
}

int nBuild = 0;

void ReadGameOptionsLegacy(GAMEOPTIONS &gameOptions, GAMEOPTIONSLEGACY &gameOptionsLegacy)
{
    gameOptions.nGameType = gameOptionsLegacy.nGameType;
    gameOptions.nDifficulty = gameOptionsLegacy.nDifficulty;
    gameOptions.nDifficultyQuantity = gameOptionsLegacy.nDifficulty;
    gameOptions.nDifficultyHealth = gameOptionsLegacy.nDifficulty;
    gameOptions.nEpisode = gameOptionsLegacy.nEpisode;
    gameOptions.nLevel = gameOptionsLegacy.nLevel;
    strcpy(gameOptions.zLevelName, gameOptionsLegacy.zLevelName);
    strcpy(gameOptions.zLevelSong, gameOptionsLegacy.zLevelSong);
    gameOptions.nTrackNumber = gameOptionsLegacy.nTrackNumber;
    strcpy(gameOptions.szSaveGameName, gameOptionsLegacy.szSaveGameName);
    strcpy(gameOptions.szUserGameName, gameOptionsLegacy.szUserGameName);
    gameOptions.nSaveGameSlot = gameOptionsLegacy.nSaveGameSlot;
    gameOptions.picEntry = gameOptionsLegacy.picEntry;
    gameOptions.uMapCRC = gameOptionsLegacy.uMapCRC;
    gameOptions.nMonsterSettings = gameOptionsLegacy.nMonsterSettings;
    gameOptions.uGameFlags = gameOptionsLegacy.uGameFlags;
    gameOptions.uNetGameFlags = gameOptionsLegacy.uNetGameFlags;
    gameOptions.nWeaponSettings = gameOptionsLegacy.nWeaponSettings;
    gameOptions.nItemSettings = gameOptionsLegacy.nItemSettings;
    gameOptions.nRespawnSettings = gameOptionsLegacy.nRespawnSettings;
    gameOptions.nTeamSettings = gameOptionsLegacy.nTeamSettings;
    gameOptions.nMonsterRespawnTime = gameOptionsLegacy.nMonsterRespawnTime;
    gameOptions.nWeaponRespawnTime = gameOptionsLegacy.nWeaponRespawnTime;
    gameOptions.nItemRespawnTime = gameOptionsLegacy.nItemRespawnTime;
    gameOptions.nSpecialRespawnTime = gameOptionsLegacy.nSpecialRespawnTime;
}

CDemo gDemo;

CDemo::CDemo()
{
    nBuild = 4;
    at0 = 0;
    at1 = 0;
    at3 = 0;
    hPFile = -1;
    hRFile = NULL;
    atb = 0;
    pFirstDemo = NULL;
    pCurrentDemo = NULL;
    nDemosFound = 0;
    at2 = 0;
    memset(&atf, 0, sizeof(atf));
    m_bLegacy = false;
}

CDemo::~CDemo()
{
    at0 = 0;
    at1 = 0;
    at3 = 0;
    atb = 0;
    memset(&atf, 0, sizeof(atf));
    if (hPFile >= 0)
    {
        kclose(hPFile);
        hPFile = -1;
    }
    if (hRFile != NULL)
    {
        fclose(hRFile);
        hRFile = NULL;
    }
    auto pNextDemo = pFirstDemo;
    for (auto pDemo = pFirstDemo; pDemo != NULL; pDemo = pNextDemo)
    {
        pNextDemo = pDemo->pNext;
        delete pDemo;
    }
    pFirstDemo = NULL;
    pCurrentDemo = NULL;
    nDemosFound = 0;
    m_bLegacy = false;
}

bool CDemo::Create(const char *pzFile)
{
    char buffer[BMAX_PATH];
    char vc = 0;
    if (at0 || at1)
        ThrowError("CDemo::Create called during demo record/playback process.");
    if (!pzFile)
    {
        for (int i = 0; i < 8 && !vc; i++)
        {
            if (G_ModDirSnprintf(buffer, BMAX_PATH, "%s0%02d.dem", BloodIniPre, i))
                return false;
            if (access(buffer, F_OK) != -1)
                vc = 1;
        }
        if (vc == 1)
        {
            hRFile = fopen(buffer, "wb");
            if (hRFile == NULL)
                return false;
        }
    }
    else
    {
        G_ModDirSnprintfLite(buffer, BMAX_PATH, pzFile);
        hRFile = fopen(buffer, "wb");
        if (hRFile == NULL)
            return false;
    }
    at0 = 1;
    atb = 0;
    return true;
}

void CDemo::Write(GINPUT *pPlayerInputs)
{
    dassert(pPlayerInputs != NULL);
    if (!at0)
        return;
    if (atb == 0)
    {
        atf.signature = 0x1a4d4445; // '\x1aMDE';
        atf.nVersion = BYTEVERSION;
        atf.nBuild = nBuild;
        atf.nInputCount = 0;
        atf.nNetPlayers = gNetPlayers;
        atf.nMyConnectIndex = myconnectindex;
        atf.nConnectHead = connecthead;
        memcpy(atf.connectPoints, connectpoint2, sizeof(atf.connectPoints));
        memcpy(&m_gameOptions, &gGameOptions, sizeof(gGameOptions));
#if B_BIG_ENDIAN == 1
        atf.signature = B_LITTLE32(atf.signature);
        atf.nVersion = B_LITTLE16(atf.nVersion);
        atf.nBuild = B_LITTLE32(atf.nBuild);
        atf.nInputCount = B_LITTLE32(atf.nInputCount);
        atf.nNetPlayers = B_LITTLE32(atf.nNetPlayers);
        atf.nMyConnectIndex = B_LITTLE16(atf.nMyConnectIndex);
        atf.nConnectHead = B_LITTLE16(atf.nConnectHead);
        for (int i = 0; i < 8; i++)
            atf.connectPoints[i] = B_LITTLE16(atf.connectPoints[i]);
#endif
        fwrite(&atf, sizeof(DEMOHEADER), 1, hRFile);
#if B_BIG_ENDIAN == 1
        m_gameOptions.nEpisode = B_LITTLE32(m_gameOptions.nEpisode);
        m_gameOptions.nLevel = B_LITTLE32(m_gameOptions.nLevel);
        m_gameOptions.nTrackNumber = B_LITTLE32(m_gameOptions.nTrackNumber);
        m_gameOptions.nSaveGameSlot = B_LITTLE16(m_gameOptions.nSaveGameSlot);
        m_gameOptions.picEntry = B_LITTLE32(m_gameOptions.picEntry);
        m_gameOptions.uMapCRC = B_LITTLE32(m_gameOptions.uMapCRC);
        m_gameOptions.uGameFlags = B_LITTLE32(m_gameOptions.uGameFlags);
        m_gameOptions.uNetGameFlags = B_LITTLE32(m_gameOptions.uNetGameFlags);
        m_gameOptions.nMonsterRespawnTime = B_LITTLE32(m_gameOptions.nMonsterRespawnTime);
        m_gameOptions.nWeaponRespawnTime = B_LITTLE32(m_gameOptions.nWeaponRespawnTime);
        m_gameOptions.nItemRespawnTime = B_LITTLE32(m_gameOptions.nItemRespawnTime);
        m_gameOptions.nSpecialRespawnTime = B_LITTLE32(m_gameOptions.nSpecialRespawnTime);
#endif
        fwrite(&m_gameOptions, sizeof(GAMEOPTIONS), 1, hRFile);
    }
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
    {
        memcpy(&at1aa[atb&1023], &pPlayerInputs[p], sizeof(GINPUT));
        atb++;
        if((atb&(kInputBufferSize-1))==0)
            FlushInput(kInputBufferSize);
    }
}

void CDemo::Close(void)
{
    if (at0)
    {
        if (atb&(kInputBufferSize-1))
            FlushInput(atb&(kInputBufferSize-1));
        atf.nInputCount = atb;
#if B_BIG_ENDIAN == 1
        atf.nInputCount = B_LITTLE32(atf.nInputCount);
#endif
        fseek(hRFile, 0, SEEK_SET);
        fwrite(&atf, sizeof(DEMOHEADER), 1, hRFile);
        fwrite(&m_gameOptions, sizeof(GAMEOPTIONS), 1, hRFile);
    }
    if (hPFile >= 0)
    {
        kclose(hPFile);
        hPFile = -1;
    }
    if (hRFile != NULL)
    {
        fclose(hRFile);
        hRFile = NULL;
    }
    at0 = 0;
    at1 = 0;
    m_bLegacy = false;
}

bool CDemo::SetupPlayback(const char *pzFile)
{
    at0 = 0;
    at1 = 0;
    const char *demoName = pzFile ? pzFile : (pCurrentDemo ? pCurrentDemo->zName : NULL);
    if (gDemoRunValidation && !demoName)
        DemoValidationError("No validation demo selected");
    if (pzFile)
    {
        hPFile = kopen4loadfrommod(pzFile, 0);
        if (hPFile == -1)
        {
            if (gDemoRunValidation)
                DemoValidationError("Cannot open validation demo", demoName);
            return false;
        }
    }
    else
    {
        if (!pCurrentDemo)
            return false;
        hPFile = kopen4loadfrommod(pCurrentDemo->zName, 0);
        if (hPFile == -1)
        {
            if (gDemoRunValidation)
                DemoValidationError("Cannot open validation demo", demoName);
            return false;
        }
    }
    int const headerBytes = kread(hPFile, &atf, sizeof(DEMOHEADER));
    if (gDemoRunValidation && headerBytes != sizeof(DEMOHEADER))
        DemoValidationError("Truncated demo header", demoName);
#if B_BIG_ENDIAN == 1
    atf.signature = B_LITTLE32(atf.signature);
    atf.nVersion = B_LITTLE16(atf.nVersion);
    atf.nBuild = B_LITTLE32(atf.nBuild);
    atf.nInputCount = B_LITTLE32(atf.nInputCount);
    atf.nNetPlayers = B_LITTLE32(atf.nNetPlayers);
    atf.nMyConnectIndex = B_LITTLE16(atf.nMyConnectIndex);
    atf.nConnectHead = B_LITTLE16(atf.nConnectHead);
    for (int i = 0; i < 8; i++)
        atf.connectPoints[i] = B_LITTLE16(atf.connectPoints[i]);
#endif
    if (gDemoRunValidation)
        DemoValidationCheckHeader(atf, kfilelength(hPFile), demoName);
    // if (aimHeight.signature != '\x1aMED' && aimHeight.signature != '\x1aMDE')
    if (atf.signature != 0x1a4d4544 && atf.signature != 0x1a4d4445)
        return 0;
    m_bLegacy = atf.signature == 0x1a4d4544;
    if (m_bLegacy)
    {
        GAMEOPTIONSLEGACY gameOptions;
        if (BloodVersion != atf.nVersion)
            return 0;
        int const optionsBytes = kread(hPFile, &gameOptions, sizeof(GAMEOPTIONSLEGACY));
        if (gDemoRunValidation)
        {
            if (optionsBytes != sizeof(GAMEOPTIONSLEGACY))
                DemoValidationError("Truncated game options", demoName);
            if (!memchr(gameOptions.zLevelName, 0, sizeof(gameOptions.zLevelName))
                || !memchr(gameOptions.zLevelSong, 0, sizeof(gameOptions.zLevelSong)))
                DemoValidationError("Unterminated string in game options", demoName);
            if (gameOptions.nGameType != kGameTypeSinglePlayer || unsigned(gameOptions.nDifficulty) > 4)
                DemoValidationError("Invalid game options", demoName);
            // DOS demos may leave these obsolete save-name fields uninitialized.
            // NotBlood also ignores them when reading its validation fixtures.
            gameOptions.szSaveGameName[0] = gameOptions.szUserGameName[0] = 0;
            m_gameOptions = gSingleGameOptions;
        }
        ReadGameOptionsLegacy(m_gameOptions, gameOptions);
    }
    else
    {
        if (BYTEVERSION != atf.nVersion)
            return 0;
        kread(hPFile, &m_gameOptions, sizeof(GAMEOPTIONS));
    }
#if B_BIG_ENDIAN == 1
    m_gameOptions.nEpisode = B_LITTLE32(m_gameOptions.nEpisode);
    m_gameOptions.nLevel = B_LITTLE32(m_gameOptions.nLevel);
    m_gameOptions.nTrackNumber = B_LITTLE32(m_gameOptions.nTrackNumber);
    m_gameOptions.nSaveGameSlot = B_LITTLE16(m_gameOptions.nSaveGameSlot);
    m_gameOptions.picEntry = B_LITTLE32(m_gameOptions.picEntry);
    m_gameOptions.uMapCRC = B_LITTLE32(m_gameOptions.uMapCRC);
    m_gameOptions.uGameFlags = B_LITTLE32(m_gameOptions.uGameFlags);
    m_gameOptions.uNetGameFlags = B_LITTLE32(m_gameOptions.uNetGameFlags);
    m_gameOptions.nMonsterRespawnTime = B_LITTLE32(m_gameOptions.nMonsterRespawnTime);
    m_gameOptions.nWeaponRespawnTime = B_LITTLE32(m_gameOptions.nWeaponRespawnTime);
    m_gameOptions.nItemRespawnTime = B_LITTLE32(m_gameOptions.nItemRespawnTime);
    m_gameOptions.nSpecialRespawnTime = B_LITTLE32(m_gameOptions.nSpecialRespawnTime);
#endif
    at0 = 0;
    at1 = 1;
    if (gDemoRunValidation)
    {
        timerInit(CLOCKTICKSPERSECOND * 500);
        SoundToggle = MusicToggle = 0;
    }
    return 1;
}

void CDemo::ProcessKeys(void)
{
    switch (gInputMode)
    {
    case INPUT_MODE_1:
        gGameMenuMgr.Process();
        break;
    case INPUT_MODE_2:
        gPlayerMsg.ProcessKeys();
        break;
    case INPUT_MODE_0:
    {
        char nKey;
        while ((nKey = keyGetScan()) != 0)
        {
	        char UNUSED(alt) = keystatus[sc_LeftAlt] | keystatus[sc_RightAlt];
	        char UNUSED(ctrl) = keystatus[sc_LeftControl] | keystatus[sc_RightControl];
            switch (nKey)
            {
            case sc_Escape:
                if (!CGameMenuMgr::m_bActive)
                {
                    gGameMenuMgr.Push(&menuMain, -1);
                    at2 = 1;
                }
                break;
            case sc_F12:
                gViewIndex = connectpoint2[gViewIndex];
                if (gViewIndex == -1)
                    gViewIndex = connecthead;
                gView = &gPlayer[gViewIndex];
                break;
            }
        }
        if (!nKey && CONTROL_JoystickEnabled)
        {
            static int32_t joyold = 0;
            int32_t joy = JOYSTICK_GetControllerButtons() == (1 << CONTROLLER_BUTTON_START);
            if (joy && !joyold)
            {
                JOYSTICK_ClearAllButtons();
                if (!CGameMenuMgr::m_bActive)
                {
                    gGameMenuMgr.Push(&menuMain, -1);
                    at2 = 1;
                }
            }
            joyold = joy;
        }
        break;
    default:
        gInputMode = INPUT_MODE_0;
        break;
    }
    }
}

void CDemo::Playback(void)
{
    const DEMOVALIDATE *pValidateInfo = NULL;
    CONTROL_BindsEnabled = false;
    ready2send = 0;
    int v4 = 0;
    if (!gDemoRunValidation && !CGameMenuMgr::m_bActive)
    {
        gGameMenuMgr.Push(&menuMain, -1);
        at2 = 1;
    }
    if (gDemoRunValidation)
        gInputMode = INPUT_MODE_1;
    gNetFifoClock = totalclock;
    gViewMode = 3;
_DEMOPLAYBACK:
    pValidateInfo = NULL;
    while (at1 && !gQuitGame)
    {
        while (totalclock >= gNetFifoClock && !gQuitGame)
        {
            if (!v4)
            {
                viewResizeView(gViewSize);
                viewSetMessage("");
                gNetPlayers = atf.nNetPlayers;
                atb = atf.nInputCount;
                myconnectindex = atf.nMyConnectIndex;
                connecthead = atf.nConnectHead;
                for (int i = 0; i < 8; i++)
                    connectpoint2[i] = atf.connectPoints[i];
                memset(gNetFifoHead, 0, sizeof(gNetFifoHead));
                gNetFifoTail = 0;
                //memcpy(connectpoint2, aimHeight.connectPoints, sizeof(aimHeight.connectPoints));
                memcpy(&gGameOptions, &m_gameOptions, sizeof(GAMEOPTIONS));
                if (gDemoRunValidation)
                    gGameOptions.uGameFlags &= ~kGameFlagContinuing;
                gGameOptions.nDifficultyQuantity = gGameOptions.nDifficulty;
                gGameOptions.nDifficultyHealth = gGameOptions.nDifficulty;
                gSkill = gGameOptions.nDifficulty;
                for (int i = 0; i < 8; i++)
                    playerInit(i, 0);
                // Legacy playback retains collision state across levels. Isolate
                // validation cases without changing sprite allocation in gameplay.
                if (gDemoRunValidation)
                    memset(gSpriteHit, 0, sizeof(gSpriteHit));
                StartLevel(&gGameOptions);
                for (int i = 0; i < 8; i++)
                {
                    gProfile[i].nAutoAim = 1;
                    gProfile[i].nWeaponSwitch = 1;
                }
                if (gDemoRunValidation)
                {
                    pValidateInfo = pCurrentDemo ? DemoValidationInfo(pCurrentDemo->zName) : NULL;
                    if (!pValidateInfo || atf.nInputCount != pValidateInfo->nInputTicks)
                        DemoValidationError("No matching expected result", pCurrentDemo ? pCurrentDemo->zName : NULL);
                    gProfile[myconnectindex].nAutoAim = pValidateInfo->nAutoAim;
                    LOG_F(INFO, "Validating demo: %s", pCurrentDemo->zName);
                }
            }
            ready2send = 0;
            if (!gDemoRunValidation)
                OSD_DispatchQueued();
            if (!gDemo.at1)
                break;
            if (!gDemoRunValidation)
                ProcessKeys();
            for (int p = connecthead; p >= 0; p = connectpoint2[p])
            {
                if ((v4&1023) == 0)
                {
                    unsigned int nSize = atb-v4;
                    if (nSize > kInputBufferSize)
                        nSize = kInputBufferSize;
                    if (gDemoRunValidation && handleevents() && quitevent)
                        DemoValidationError("Validation interrupted", pCurrentDemo->zName);
                    ReadInput(nSize);
                }
                memcpy(&gFifoInput[gNetFifoHead[p]&255], &at1aa[v4&1023], sizeof(GINPUT));
                gNetFifoHead[p]++;
                v4++;
                if (v4 >= atf.nInputCount)
                {
                    ready2send = 0;
                    if (gDemoRunValidation)
                    {
                        // Upstream baselines are sampled before processing the last input.
                        // Moving these checks after ProcessFrame would invalidate every baseline.
                        auto const *playerSprite = gPlayer[myconnectindex].pSprite;
                        if (!pValidateInfo || !playerSprite || playerSprite->extra <= 0 || playerSprite->extra >= kMaxXSprites)
                            DemoValidationError("Invalid player state", pCurrentDemo->zName);
                        bool invalid = false;
                        if (wrandomseed != pValidateInfo->wrandomseed)
                        {
                            LOG_F(ERROR, "Random seed desync: expected %08X, got %08X", pValidateInfo->wrandomseed, (uint32_t)wrandomseed);
                            invalid = true;
                        }
                        if (playerSprite->x != pValidateInfo->xyz.x || playerSprite->y != pValidateInfo->xyz.y || playerSprite->z != pValidateInfo->xyz.z)
                        {
                            LOG_F(ERROR, "Player position desync: expected (%d, %d, %d), got (%d, %d, %d)",
                                pValidateInfo->xyz.x, pValidateInfo->xyz.y, pValidateInfo->xyz.z,
                                int(playerSprite->x), int(playerSprite->y), int(playerSprite->z));
                            invalid = true;
                        }
                        uint32_t const health = xsprite[playerSprite->extra].health;
                        if (health != pValidateInfo->health)
                        {
                            LOG_F(ERROR, "Player health desync: expected %u, got %u", pValidateInfo->health, health);
                            invalid = true;
                        }
                        if (invalid)
                            DemoValidationError("Demo desync", pCurrentDemo->zName);
                        ++nValidationPassed;
                        nValidationInputs += pValidateInfo->nInputTicks;
                        LOG_F(INFO, "Demo Synced: %s", pCurrentDemo->zName);
                        if (!pCurrentDemo->pNext)
                        {
                            if (nValidationPassed != nDemosFound || nValidationPassed + nValidationSkipped != ARRAY_SSIZE(gDemoValidate))
                                DemoValidationError("Validation suite did not complete");
                            gDemoValidationPassed = true;
                            LOG_F(INFO, "1.21 Validation Successful! (%d passed, %d skipped)", nValidationPassed, nValidationSkipped);
                            LOG_F(INFO, "Total Demo Hours: %02u:%02u", nValidationInputs / (kTicsPerSec * 60 * 60), nValidationInputs / (kTicsPerSec * 60) % 60);
                            gQuitGame = true;
                            break;
                        }
                    }
                    if (nDemosFound > 1)
                    {
                        v4 = 0;
                        Close();
                        NextDemo();
                        gNetFifoClock = totalclock;
                        goto _DEMOPLAYBACK;
                    }
                    else
                    {
                        int const nOffset = sizeof(DEMOHEADER)+(m_bLegacy ? sizeof(GAMEOPTIONSLEGACY) : sizeof(GAMEOPTIONS));
                        klseek(hPFile, nOffset, SEEK_SET);
                        v4 = 0;
                    }
                }
            }
            gNetFifoClock += kTicsPerFrame;
            if (!gQuitGame)
                ProcessFrame();
            ready2send = 0;
        }
        if (engineFPSLimit())
        {
            if (handleevents() && quitevent)
            {
                if (gDemoRunValidation)
                    DemoValidationError("Validation interrupted", pCurrentDemo ? pCurrentDemo->zName : NULL);
                KB_KeyDown[sc_Escape] = 1;
                quitevent = 0;
            }
            if (!gDemoRunValidation)
                MUSIC_Update();
            viewDrawScreen();
            if (!gDemoRunValidation && gInputMode == INPUT_MODE_1 && CGameMenuMgr::m_bActive)
                gGameMenuMgr.Draw();
            videoNextPage();
        }
        if (TestBitString(gotpic, 2342))
        {
            FireProcess();
            ClearBitString(gotpic, 2342);
        }
    }
    Close();
    if (gDemoRunValidation && !gDemoValidationPassed)
        DemoValidationError("Validation stopped before the suite completed");
}

void CDemo::StopPlayback(void)
{
    at1 = 0;
}

void CDemo::LoadDemoInfo(void)
{
    if (gDemoRunValidation)
    {
        gDemoValidationPassed = false;
        nValidationPassed = nValidationSkipped = 0;
        nValidationInputs = 0;
        nDemosFound = 0;
        auto pDemo = &pFirstDemo;
        int const oldSearchMode = pathsearchmode;
        pathsearchmode = 0;
        auto const pList = klistpath("/validatedemos/", "*.dem", BUILDVFS_FIND_FILE);
        const char *demoNames[ARRAY_SIZE(gDemoValidate)] = {};
        for (auto it = pList; it; it = it->next)
        {
            char name[BMAX_PATH];
            Bsnprintf(name, sizeof(name), "/validatedemos/%s", it->name);
            auto const *info = DemoValidationInfo(name);
            if (!info)
                DemoValidationError("No expected result for demo", name);
            size_t const index = info - gDemoValidate;
            if (demoNames[index])
                DemoValidationError("Duplicate validation demo", name);
            demoNames[index] = it->name;
        }
        // Build the chain in baseline order, and require every non-excluded case.
        // A partial or empty archive must never report a successful validation.
        for (size_t index = 0; index < ARRAY_SIZE(gDemoValidate); ++index)
        {
            auto const &info = gDemoValidate[index];
            if (auto const reason = DemoValidationSkipReason(info.zName))
            {
                LOG_F(INFO, "Demo Skipped: %s (%s)", info.zName, reason);
                ++nValidationSkipped;
                continue;
            }
            if (!demoNames[index])
                DemoValidationError("Required validation demo missing", info.zName);
            char name[BMAX_PATH];
            Bsnprintf(name, sizeof(name), "/validatedemos/%s", demoNames[index]);
            int const file = kopen4loadfrommod(name, 0);
            if (file == -1)
                DemoValidationError("Cannot open validation demo", name);
            DEMOHEADER header;
            if (kread(file, &header, sizeof(header)) != sizeof(header))
                DemoValidationError("Truncated demo header", name);
            header.signature = B_LITTLE32(header.signature);
            header.nVersion = B_LITTLE16(header.nVersion);
            header.nInputCount = B_LITTLE32(header.nInputCount);
            header.nNetPlayers = B_LITTLE32(header.nNetPlayers);
            header.nMyConnectIndex = B_LITTLE16(header.nMyConnectIndex);
            header.nConnectHead = B_LITTLE16(header.nConnectHead);
            header.connectPoints[0] = B_LITTLE16(header.connectPoints[0]);
            DemoValidationCheckHeader(header, kfilelength(file), name);
            kclose(file);
            *pDemo = new DEMOCHAIN;
            (*pDemo)->pNext = NULL;
            Bstrncpy((*pDemo)->zName, name, BMAX_PATH);
            pDemo = &(*pDemo)->pNext;
            ++nDemosFound;
        }
        klistfree(pList);
        pathsearchmode = oldSearchMode;
        pCurrentDemo = pFirstDemo;
        if (!nDemosFound)
            DemoValidationError("No validation demos found");
        LOG_F(INFO, "1.21 Validation Suite: %d demos, %d compiler exclusions", nDemosFound, nValidationSkipped);
        return;
    }
    auto pDemo = &pFirstDemo;
    const int opsm = pathsearchmode;
    nDemosFound = 0;
    pathsearchmode = 0;
    char zFN[BMAX_PATH];
    Bsnprintf(zFN, BMAX_PATH, "%s*.dem", BloodIniPre);
    auto pList = klistpath("/", zFN, BUILDVFS_FIND_FILE);
    auto pIterator = pList;
    while (pIterator != NULL)
    {
        int hFile = kopen4loadfrommod(pIterator->name, 0);
        if (hFile == -1)
            ThrowError("Error loading demo file header.");
        kread(hFile, &atf, sizeof(atf));
        kclose(hFile);
#if B_BIG_ENDIAN == 1
        atf.signature = B_LITTLE32(atf.signature);
        atf.nVersion = B_LITTLE16(atf.nVersion);
#endif
        if ((atf.signature == 0x1a4d4544 /* '\x1aMED' */&& atf.nVersion == BloodVersion)
            || (atf.signature == 0x1a4d4445 /* '\x1aMDE' */ && atf.nVersion == BYTEVERSION))
        {
            *pDemo = new DEMOCHAIN;
            (*pDemo)->pNext = NULL;
            Bstrncpy((*pDemo)->zName, pIterator->name, BMAX_PATH);
            nDemosFound++;
            pDemo = &(*pDemo)->pNext;
        }
        pIterator = pIterator->next;
    }
    klistfree(pList);
    pathsearchmode = opsm;
    pCurrentDemo = pFirstDemo;
}

bool CDemo::VanillaDemo(void)
{
    return at1 && m_bLegacy;
}

void CDemo::NextDemo(void)
{
    pCurrentDemo = pCurrentDemo->pNext ? pCurrentDemo->pNext : pFirstDemo;
    SetupPlayback(NULL);
}

const int nInputSize = 17;
const int nInputSizeLegacy = 22;

void CDemo::FlushInput(int nCount)
{
    char pBuffer[nInputSize*kInputBufferSize];
    BitWriter bitWriter(pBuffer, sizeof(pBuffer));
    for (int i = 0; i < nCount; i++)
    {
        GINPUT *pInput = &at1aa[i];
        bitWriter.writeBit(pInput->syncFlags.buttonChange);
        bitWriter.writeBit(pInput->syncFlags.keyChange);
        bitWriter.writeBit(pInput->syncFlags.useChange);
        bitWriter.writeBit(pInput->syncFlags.weaponChange);
        bitWriter.writeBit(pInput->syncFlags.mlookChange);
        bitWriter.writeBit(pInput->syncFlags.run);
        bitWriter.write(pInput->forward, 16);
        bitWriter.write(pInput->q16turn, 32);
        bitWriter.write(pInput->strafe, 16);
        bitWriter.writeBit(pInput->buttonFlags.jump);
        bitWriter.writeBit(pInput->buttonFlags.crouch);
        bitWriter.writeBit(pInput->buttonFlags.shoot);
        bitWriter.writeBit(pInput->buttonFlags.shoot2);
        bitWriter.writeBit(pInput->buttonFlags.lookUp);
        bitWriter.writeBit(pInput->buttonFlags.lookDown);
        bitWriter.writeBit(pInput->keyFlags.action);
        bitWriter.writeBit(pInput->keyFlags.jab);
        bitWriter.writeBit(pInput->keyFlags.prevItem);
        bitWriter.writeBit(pInput->keyFlags.nextItem);
        bitWriter.writeBit(pInput->keyFlags.useItem);
        bitWriter.writeBit(pInput->keyFlags.prevWeapon);
        bitWriter.writeBit(pInput->keyFlags.nextWeapon);
        bitWriter.writeBit(pInput->keyFlags.holsterWeapon);
        bitWriter.writeBit(pInput->keyFlags.lookCenter);
        bitWriter.writeBit(pInput->keyFlags.lookLeft);
        bitWriter.writeBit(pInput->keyFlags.lookRight);
        bitWriter.writeBit(pInput->keyFlags.spin180);
        bitWriter.writeBit(pInput->keyFlags.pause);
        bitWriter.writeBit(pInput->keyFlags.quit);
        bitWriter.writeBit(pInput->keyFlags.restart);
        bitWriter.writeBit(pInput->keyFlags.lastWeapon);
        bitWriter.writeBit(pInput->useFlags.useBeastVision);
        bitWriter.writeBit(pInput->useFlags.useCrystalBall);
        bitWriter.writeBit(pInput->useFlags.useJumpBoots);
        bitWriter.writeBit(pInput->useFlags.useMedKit);
        bitWriter.write(pInput->newWeapon, 8);
        bitWriter.write(pInput->q16mlook, 32);
        bitWriter.skipBits(1);
    }
    fwrite(pBuffer, 1, nInputSize*nCount, hRFile);
}

void CDemo::ReadInput(int nCount)
{
    if (m_bLegacy)
    {
        char pBuffer[nInputSizeLegacy*kInputBufferSize];
        int const inputBytes = kread(hPFile, pBuffer, nInputSizeLegacy*nCount);
        if (gDemoRunValidation && inputBytes != nInputSizeLegacy*nCount)
            DemoValidationError("Truncated demo inputs", pCurrentDemo ? pCurrentDemo->zName : NULL);
        BitReader bitReader(pBuffer, sizeof(pBuffer));
        memset(at1aa, 0, nCount * sizeof(GINPUT));
        for (int i = 0; i < nCount; i++)
        {
            GINPUT *pInput = &at1aa[i];
            pInput->syncFlags.buttonChange = bitReader.readBit();
            pInput->syncFlags.keyChange = bitReader.readBit();
            pInput->syncFlags.useChange = bitReader.readBit();
            pInput->syncFlags.weaponChange = bitReader.readBit();
            pInput->syncFlags.mlookChange = bitReader.readBit();
            pInput->syncFlags.run = bitReader.readBit();
            bitReader.skipBits(26);
            pInput->forward = bitReader.readSigned(8) << 8;
            pInput->q16turn = fix16_from_int(bitReader.readSigned(16)) >> 2;
            pInput->strafe = bitReader.readSigned(8) << 8;
            pInput->buttonFlags.jump = bitReader.readBit();
            pInput->buttonFlags.crouch = bitReader.readBit();
            pInput->buttonFlags.shoot = bitReader.readBit();
            pInput->buttonFlags.shoot2 = bitReader.readBit();
            pInput->buttonFlags.lookUp = bitReader.readBit();
            pInput->buttonFlags.lookDown = bitReader.readBit();
            bitReader.skipBits(26);
            pInput->keyFlags.action = bitReader.readBit();
            pInput->keyFlags.jab = bitReader.readBit();
            pInput->keyFlags.prevItem = bitReader.readBit();
            pInput->keyFlags.nextItem = bitReader.readBit();
            pInput->keyFlags.useItem = bitReader.readBit();
            pInput->keyFlags.prevWeapon = bitReader.readBit();
            pInput->keyFlags.nextWeapon = bitReader.readBit();
            pInput->keyFlags.holsterWeapon = bitReader.readBit();
            pInput->keyFlags.lookCenter = bitReader.readBit();
            pInput->keyFlags.lookLeft = bitReader.readBit();
            pInput->keyFlags.lookRight = bitReader.readBit();
            pInput->keyFlags.spin180 = bitReader.readBit();
            pInput->keyFlags.pause = bitReader.readBit();
            pInput->keyFlags.quit = bitReader.readBit();
            pInput->keyFlags.restart = bitReader.readBit();
            pInput->keyFlags.lastWeapon = 0;
            bitReader.skipBits(17);
            pInput->useFlags.useBeastVision = bitReader.readBit();
            pInput->useFlags.useCrystalBall = bitReader.readBit();
            pInput->useFlags.useJumpBoots = bitReader.readBit();
            pInput->useFlags.useMedKit = bitReader.readBit();
            bitReader.skipBits(28);
            pInput->newWeapon = bitReader.readUnsigned(8);
            int mlook = bitReader.readSigned(8);
            pInput->q16mlook = fix16_from_int(mlook / 4);
        }
    }
    else
    {
        char pBuffer[nInputSize*kInputBufferSize];
        kread(hPFile, pBuffer, nInputSize*nCount);
        BitReader bitReader(pBuffer, sizeof(pBuffer));
        memset(at1aa, 0, nCount * sizeof(GINPUT));
        for (int i = 0; i < nCount; i++)
        {
            GINPUT *pInput = &at1aa[i];
            pInput->syncFlags.buttonChange = bitReader.readBit();
            pInput->syncFlags.keyChange = bitReader.readBit();
            pInput->syncFlags.useChange = bitReader.readBit();
            pInput->syncFlags.weaponChange = bitReader.readBit();
            pInput->syncFlags.mlookChange = bitReader.readBit();
            pInput->syncFlags.run = bitReader.readBit();
            pInput->forward = bitReader.readSigned(16);
            pInput->q16turn = bitReader.readSigned(32);
            pInput->strafe = bitReader.readSigned(16);
            pInput->buttonFlags.jump = bitReader.readBit();
            pInput->buttonFlags.crouch = bitReader.readBit();
            pInput->buttonFlags.shoot = bitReader.readBit();
            pInput->buttonFlags.shoot2 = bitReader.readBit();
            pInput->buttonFlags.lookUp = bitReader.readBit();
            pInput->buttonFlags.lookDown = bitReader.readBit();
            pInput->keyFlags.action = bitReader.readBit();
            pInput->keyFlags.jab = bitReader.readBit();
            pInput->keyFlags.prevItem = bitReader.readBit();
            pInput->keyFlags.nextItem = bitReader.readBit();
            pInput->keyFlags.useItem = bitReader.readBit();
            pInput->keyFlags.prevWeapon = bitReader.readBit();
            pInput->keyFlags.nextWeapon = bitReader.readBit();
            pInput->keyFlags.holsterWeapon = bitReader.readBit();
            pInput->keyFlags.lookCenter = bitReader.readBit();
            pInput->keyFlags.lookLeft = bitReader.readBit();
            pInput->keyFlags.lookRight = bitReader.readBit();
            pInput->keyFlags.spin180 = bitReader.readBit();
            pInput->keyFlags.pause = bitReader.readBit();
            pInput->keyFlags.quit = bitReader.readBit();
            pInput->keyFlags.restart = bitReader.readBit();
            pInput->keyFlags.lastWeapon = bitReader.readBit();
            pInput->useFlags.useBeastVision = bitReader.readBit();
            pInput->useFlags.useCrystalBall = bitReader.readBit();
            pInput->useFlags.useJumpBoots = bitReader.readBit();
            pInput->useFlags.useMedKit = bitReader.readBit();
            pInput->newWeapon = bitReader.readUnsigned(8);
            pInput->q16mlook = bitReader.readSigned(32);
            bitReader.skipBits(1);
        }
    }
}
