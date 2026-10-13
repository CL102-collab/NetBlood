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
#include <ctime>
#include "build.h"
#include "compat.h"
#include "crc32.h"
#include "lz4.h"
#include "mmulti.h"
#include "common_game.h"
#include "config.h"
#include "ai.h"
#include "asound.h"
#include "blood.h"
#include "demo.h"
#include "endgame.h"
#include "globals.h"
#include "db.h"
#include "messages.h"
#include "menu.h"
#include "network.h"
#include "loadsave.h"
#include "resource.h"
#include "screen.h"
#include "sectorfx.h"
#include "seq.h"
#include "sfx.h"
#include "sound.h"
#include "tile.h"
#include "view.h"
#ifdef NOONE_EXTENSIONS
#include "nnexts.h"
#endif

#define LZ4_THRESHOLD_SIZE (0x8000)

GAMEOPTIONS gSaveGameOptions[10];
char *gSaveGamePic[10];
SaveGameStats gSaveGameStats[10];
unsigned int gSavedOffset = 0;

namespace
{
constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 200;
constexpr int kPreviewSize = kPreviewWidth * kPreviewHeight;

// Optional trailer, outside the legacy save body. Older saves have no trailer;
// the game-state loader deliberately does not need to read it.
struct SavePreviewFooter
{
    char magic[8];
    uint32_t version;
    uint16_t width, height;
    uint32_t size, crc;
};
static_assert(sizeof(SavePreviewFooter) == 24, "Save preview footer layout changed");

// Kept separately from the image so a missing or damaged thumbnail does not
// discard the save's statistics. All numeric fields on disk are little endian.
struct SaveStatsFooter
{
    char magic[8];
    uint32_t version, size;
    uint64_t savedAtUtc;
    uint32_t elapsedSeconds;
    int32_t kills, totalKills, secrets, totalSecrets;
    uint32_t crc;
};
static_assert(sizeof(SaveStatsFooter) == 48 && offsetof(SaveStatsFooter, crc) == 44,
    "Save statistics footer layout changed");

SaveGameStats CaptureSaveStats(void)
{
    SaveGameStats stats = {};
    std::time_t const now = std::time(nullptr);
    if (now < 0)
        return stats;
    stats.valid = true;
    stats.savedAtUtc = static_cast<uint64_t>(now);
    stats.elapsedSeconds = ClipLow(gLevelTime, 0) / kTicsPerSec;
    stats.kills = ClipLow(gKillMgr.at4, 0);
    stats.totalKills = ClipLow(gKillMgr.at0, stats.kills);
    stats.secrets = ClipLow(gSecretMgr.nNormalSecretsFound, 0);
    stats.totalSecrets = ClipLow(gSecretMgr.nAllSecrets, stats.secrets);
    return stats;
}

void ReadSaveStats(int hFile, int nSlot)
{
    gSaveGameStats[nSlot] = {};
    // The image can be absent or truncated. Only inspect this bounded trailer
    // area, and accept a complete metadata block with its own version and CRC.
    constexpr int kTailSize = kPreviewSize + sizeof(SavePreviewFooter) + sizeof(SaveStatsFooter);
    int const length = kfilelength(hFile);
    if (length < static_cast<int>(sizeof(SaveStatsFooter)))
        return;
    int const tailSize = ClipHigh(length, kTailSize);
    uint8_t tail[kTailSize];
    if (klseek(hFile, length - tailSize, SEEK_SET) < 0 || kread(hFile, tail, tailSize) != tailSize)
        return;
    for (int offset = tailSize - sizeof(SaveStatsFooter); offset >= 0; --offset)
    {
        if (Bmemcmp(tail + offset, "NBSTAT01", 8))
            continue;
        SaveStatsFooter footer;
        Bmemcpy(&footer, tail + offset, sizeof(footer));
        if (B_LITTLE32(footer.version) != 1 || B_LITTLE32(footer.size) != sizeof(footer)
            || Bcrc32(&footer, offsetof(SaveStatsFooter, crc), 0) != B_LITTLE32(footer.crc))
            continue;
        SaveGameStats stats = {};
        stats.savedAtUtc = B_LITTLE64(footer.savedAtUtc);
        stats.elapsedSeconds = B_LITTLE32(footer.elapsedSeconds);
        stats.kills = B_LITTLE32(footer.kills);
        stats.totalKills = B_LITTLE32(footer.totalKills);
        stats.secrets = B_LITTLE32(footer.secrets);
        stats.totalSecrets = B_LITTLE32(footer.totalSecrets);
        if (stats.savedAtUtc > INT64_MAX || stats.kills < 0 || stats.totalKills < stats.kills
            || stats.secrets < 0 || stats.totalSecrets < stats.secrets)
            continue;
        stats.valid = true;
        gSaveGameStats[nSlot] = stats;
        return;
    }
}

void WriteSaveStats(FILE *file, const SaveGameStats &stats)
{
    if (!stats.valid)
        return;
    SaveStatsFooter footer;
    Bmemcpy(footer.magic, "NBSTAT01", sizeof(footer.magic));
    footer.version = B_LITTLE32(1);
    footer.size = B_LITTLE32(sizeof(footer));
    footer.savedAtUtc = B_LITTLE64(stats.savedAtUtc);
    footer.elapsedSeconds = B_LITTLE32(stats.elapsedSeconds);
    footer.kills = B_LITTLE32(stats.kills);
    footer.totalKills = B_LITTLE32(stats.totalKills);
    footer.secrets = B_LITTLE32(stats.secrets);
    footer.totalSecrets = B_LITTLE32(stats.totalSecrets);
    footer.crc = B_LITTLE32(Bcrc32(&footer, offsetof(SaveStatsFooter, crc), 0));
    if (fwrite(&footer, 1, sizeof(footer), file) != sizeof(footer))
        ThrowError("File error #%d writing save statistics.", errno);
}

uint8_t pendingPreview[kPreviewSize];
bool pendingPreviewValid = false;
int previewTile = -1;
int previewTileSlot = -1;
intptr_t previewTilePixels = 0;

void ClearSavePreview(int nSlot)
{
    if (gSaveGamePic[nSlot])
    {
        Resource::Free(gSaveGamePic[nSlot]);
        gSaveGamePic[nSlot] = nullptr;
    }
    if (previewTileSlot == nSlot)
        previewTileSlot = -1;
}

void CacheSavePreview(int nSlot, const uint8_t *pixels)
{
    ClearSavePreview(nSlot);
    if (pixels)
    {
        gSaveGamePic[nSlot] = static_cast<char *>(Resource::Alloc(kPreviewSize));
        Bmemcpy(gSaveGamePic[nSlot], pixels, kPreviewSize);
    }
}

void ReadSavePreview(int hFile, int nSlot)
{
    ClearSavePreview(nSlot);
    int const length = kfilelength(hFile);
    if (length < kPreviewSize + static_cast<int>(sizeof(SavePreviewFooter)))
        return;
    SavePreviewFooter footer;
    if (klseek(hFile, length - sizeof(footer), SEEK_SET) < 0
        || kread(hFile, &footer, sizeof(footer)) != sizeof(footer)
        || Bmemcmp(footer.magic, "NBPREV01", sizeof(footer.magic))
        || B_LITTLE32(footer.version) != 1
        || B_LITTLE16(footer.width) != kPreviewWidth
        || B_LITTLE16(footer.height) != kPreviewHeight
        || B_LITTLE32(footer.size) != kPreviewSize)
        return;

    uint8_t pixels[kPreviewSize];
    if (klseek(hFile, length - sizeof(footer) - kPreviewSize, SEEK_SET) < 0
        || kread(hFile, pixels, kPreviewSize) != kPreviewSize
        || Bcrc32(pixels, kPreviewSize, 0) != B_LITTLE32(footer.crc))
        return;
    CacheSavePreview(nSlot, pixels);
}

void WriteSavePreview(FILE *file)
{
    if (!pendingPreviewValid)
        return;
    SavePreviewFooter footer;
    Bmemcpy(footer.magic, "NBPREV01", sizeof(footer.magic));
    footer.version = B_LITTLE32(1);
    footer.width = B_LITTLE16(kPreviewWidth);
    footer.height = B_LITTLE16(kPreviewHeight);
    footer.size = B_LITTLE32(kPreviewSize);
    footer.crc = B_LITTLE32(Bcrc32(pendingPreview, kPreviewSize, 0));
    if (fwrite(pendingPreview, 1, kPreviewSize, file) != kPreviewSize
        || fwrite(&footer, 1, sizeof(footer), file) != sizeof(footer))
        ThrowError("File error #%d writing save preview.", errno);
}
}

void CaptureSaveGamePreview(void)
{
    pendingPreviewValid = viewCaptureSavePreview(pendingPreview, kPreviewWidth, kPreviewHeight);
}

int SaveGamePreviewTile(int nSlot)
{
    if ((unsigned)nSlot >= 10 || !gSaveGamePic[nSlot])
        return -1;
    // Extra ART can replace dynamic tiles when a new episode is loaded.
    if (previewTile >= 0 && (waloff[previewTile] != previewTilePixels
        || tilesiz[previewTile].x != kPreviewWidth || tilesiz[previewTile].y != kPreviewHeight))
    {
        previewTile = -1;
        previewTileSlot = -1;
    }
    if (previewTile < 0)
    {
        previewTile = tileSearchFreeRange(1);
        if (previewTile < 0)
            return -1;
        previewTilePixels = tileCreate(previewTile, kPreviewWidth, kPreviewHeight);
        if (!previewTilePixels)
        {
            previewTile = -1;
            return -1;
        }
    }
    if (previewTileSlot != nSlot)
    {
        Bmemcpy(reinterpret_cast<void *>(previewTilePixels), gSaveGamePic[nSlot], kPreviewSize);
        tileInvalidate(previewTile, -1, -1);
        previewTileSlot = nSlot;
    }
    return previewTile;
}

LoadSave LoadSave::head(123);
FILE *LoadSave::hSFile = NULL;
int LoadSave::hLFile = -1;

void LoadSave::Save(void)
{
    ThrowError("Pure virtual function called");
}

void LoadSave::Load(void)
{
    ThrowError("Pure virtual function called");
}

void LoadSave::Read(void *pData, int nSize)
{
    dassert(hLFile != -1);
    while (nSize >= LZ4_THRESHOLD_SIZE)
    {
        int nCompSize;
        if (kread(hLFile, (void *)&nCompSize, sizeof(nCompSize)) != sizeof(nCompSize))
            ThrowError("Error reading save file.");
        nCompSize = B_LITTLE32(nCompSize);
        if (nCompSize >= nSize)
            ThrowError("Error reading size file compress offset.");
        if (nCompSize == 0) // uncompressed data
            break;

        void *pIn = Xaligned_alloc(16, nCompSize);
        if (!pIn)
        {
            ThrowError("Error could not allocate %d bytes for save file.", nCompSize);
            return;
        }
        if (kread(hLFile, pIn, nCompSize) != nCompSize)
            ThrowError("Error reading save file.");
        LZ4_decompress_safe((const char *)pIn, (char *)pData, nCompSize, nSize);
        Xaligned_free(pIn);
        return;
    }
    if (kread(hLFile, pData, nSize) != nSize)
        ThrowError("Error reading save file.");
}

void LoadSave::Write(void const *pData, int nSize)
{
    dassert(hSFile != NULL);
    while (nSize >= LZ4_THRESHOLD_SIZE)
    {
        int nCompSize = LZ4_compressBound(nSize);
        void *pOut = Xaligned_alloc(16, nCompSize);
        if (!pOut)
        {
            nCompSize = 0;
            if (fwrite((void *)&nCompSize, 1, sizeof(nCompSize), hSFile) != sizeof(nCompSize))
                ThrowError("File error #%d writing save file.", errno);
            break;
        }
        nCompSize = LZ4_compress_fast((const char *)pData, (char *)pOut, nSize, nCompSize, lz4CompressionLevel);
        const char bBadRatio = nCompSize >= nSize; // compressed size is bigger than uncompressed, store as uncompressed data
        if (bBadRatio)
        {
            nCompSize = 0;
            if (fwrite((void *)&nCompSize, 1, sizeof(nCompSize), hSFile) != sizeof(nCompSize))
                ThrowError("File error #%d writing save file.", errno);
            Xaligned_free(pOut);
            break;
        }

        int nCompSizeBSwap = B_LITTLE32(nCompSize);
        if (fwrite((void *)&nCompSizeBSwap, 1, sizeof(nCompSizeBSwap), hSFile) != sizeof(nCompSizeBSwap))
            ThrowError("File error #%d writing save file.", errno);
        if (fwrite(pOut, 1, nCompSize, hSFile) != (size_t)nCompSize)
            ThrowError("File error #%d writing save file.", errno);
        Xaligned_free(pOut);
        return;
    }
    if (fwrite(pData, 1, nSize, hSFile) != (size_t)nSize)
        ThrowError("File error #%d writing save file.", errno);
}

void LoadSave::LoadGame(char *pzFile)
{
    const char bDemoWasPlayed = gDemo.at1;
    const char bGameWasStarted = gGameStarted;
    if (gDemo.at1 || gDemo.at0)
        gDemo.Close();

    gViewPos = VIEWPOS_0;
    gViewIndex = myconnectindex;
    sndKillAllSounds();
    sfxKillAllSounds();
    ambKillAll();
    seqKillAll();
    if (!gGameStarted)
    {
        memset(xvel,0,sizeof(xvel[0])*kMaxSprites);
        memset(yvel,0,sizeof(yvel[0])*kMaxSprites);
        memset(zvel,0,sizeof(zvel[0])*kMaxSprites);
        memset(xsprite, 0, sizeof(XSPRITE)*kMaxXSprites);
        memset(sprite, 0, sizeof(spritetype)*kMaxSprites);
        memset(qsprite_filler,0,sizeof(qsprite_filler[0])*kMaxSprites);
        memset(qsector_filler,0,sizeof(qsector_filler[0])*kMaxSectors);
        automapping = 1;
    }
    hLFile = kopen4load(pzFile, 0);
    if (hLFile == -1)
        ThrowError("Error loading save file.");
    LoadSave *rover = head.next;
    while (rover != &head)
    {
        rover->Load();
        rover = rover->next;
    }
    kclose(hLFile);
    hLFile = -1;
    if (!gGameStarted)
        scrLoadPLUs();
    InitSectorFX();
    viewInitializePrediction();
    PreloadCache();
    if (!VanillaMode()) // set reverb sound effect state
        sfxSetReverb(packItemActive(gMe, kPackDivingSuit) || powerupCheck(gMe, kPwUpReflectShots));
    ambInit();
#ifdef YAX_ENABLE
    yax_update(numyaxbunches > 0 ? 2 : 1);
#endif
    calc_sector_reachability();
    memset(myMinLag, 0, sizeof(myMinLag));
    otherMinLag = 0;
    myMaxLag = 0;
    gNetFifoClock = 0;
    gNetFifoTail = 0;
    memset(gNetFifoHead, 0, sizeof(gNetFifoHead));
    gPredictTail = 0;
    gNetFifoMasterTail = 0;
    memset(gFifoInput, 0, sizeof(gFifoInput));
    memset(gChecksum, 0, sizeof(gChecksum));
    memset(gCheckFifo, 0, sizeof(gCheckFifo));
    memset(gCheckHead, 0, sizeof(gCheckHead));
    gSendCheckTail = 0;
    gCheckTail = 0;
    gBufferJitter = 0;
    bOutOfSync = 0;
    for (int i = 0; i < gNetPlayers; i++)
        playerSetRace(&gPlayer[i], gPlayer[i].lifeMode);
    if (VanillaMode())
        viewSetMessage("");
    else
        gGameMessageMgr.Clear();
    viewSetErrorMessage("");
    viewResizeView(gViewSize);
    viewUpdatePages();
    if (!gGameStarted)
    {
        netWaitForEveryone(0);
        memset(gPlayerReady, 0, sizeof(gPlayerReady));
    }
    gFrameTicks = 0;
    gFrame = 0;
    gCacheMiss = 0;
    gFrameRate = 0;
    totalclock = 0;
    gPaused = 0;
    gGameStarted = 1;

#ifdef USE_STRUCT_TRACKERS
    Bmemset(sectorchanged, 0, sizeof(sectorchanged));
    Bmemset(spritechanged, 0, sizeof(spritechanged));
    Bmemset(wallchanged, 0, sizeof(wallchanged));
#endif

#ifdef USE_OPENGL
    Polymost_prepare_loadboard();
#endif

#ifdef POLYMER
    if (videoGetRenderMode() == REND_POLYMER)
        polymer_loadboard();

    // this light pointer nulling needs to be outside the videoGetRenderMode check
    // because we might be loading the savegame using another renderer but
    // change to Polymer later
    for (int i=0; i<kMaxSprites; i++)
    {
        gPolymerLight[i].lightptr = NULL;
        gPolymerLight[i].lightId = -1;
    }
#endif

    if (gGameOptions.nEpisode >= gEpisodeCount || gGameOptions.nLevel >= gEpisodeInfo[gGameOptions.nEpisode].nLevels
        || Bstrcasecmp(gEpisodeInfo[gGameOptions.nEpisode].levelsInfo[gGameOptions.nLevel].Filename, gGameOptions.zLevelName) != 0)
    {
        if (!gSysRes.Lookup(gGameOptions.zLevelName, "MAP"))
        {
            gGameOptions.nEpisode = 0;
            gGameOptions.nLevel = 0;
        }
        else
        {
            levelAddUserMap(gGameOptions.zLevelName);
        }
    }

    if (MusicRestartsOnLoadToggle
        || bDemoWasPlayed
        || !bGameWasStarted
        || (gMusicPrevLoadedEpisode != gGameOptions.nEpisode || gMusicPrevLoadedLevel != gGameOptions.nLevel))
    {
        levelTryPlayMusicOrNothing(gGameOptions.nEpisode, gGameOptions.nLevel);
    }
    gMusicPrevLoadedEpisode = gGameOptions.nEpisode;
    gMusicPrevLoadedLevel = gGameOptions.nLevel;

    netBroadcastPlayerInfo(myconnectindex);
    //sndPlaySong(gGameOptions.zLevelSong, 1);
}

// TODO: when starting to use buildvfs in SaveGame(), remove this function and use buildvfs_exists() instead
static int file_exists(char const* path)
{
#ifdef _WIN32
    return GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct Bstat st;
    return !Bstat(path, &st);
#endif
}

void LoadSave::SaveGame(char *pzFile)
{
    SaveGameStats const stats = CaptureSaveStats();
    // TODO: use buildvfs_open_write() etc (probably in the whole file?)
    char fileNameTmp[BMAX_PATH+4];
    const char* saveFileName = pzFile;
    bool saveFileExists = file_exists(saveFileName);
    if (saveFileExists)
    {
        // write to a different file first, so in case the game crashes while saving
        // (which would result in a corrupted savegame) at least the old savegame is preserved
        strcpy(fileNameTmp, pzFile);
        strcat(fileNameTmp, "_tmp");
        saveFileName = fileNameTmp;
    }

    hSFile = fopen(saveFileName, "wb");
    if (hSFile == NULL)
        ThrowError("File error #%d creating save file.", errno);
    LoadSave *rover = head.next;
    while (rover != &head)
    {
        rover->Save();
        rover = rover->next;
    }
    WriteSaveStats(hSFile, stats);
    WriteSavePreview(hSFile);
    if (fclose(hSFile))
        ThrowError("File error #%d finishing save file.", errno);
    hSFile = NULL;
    if (saveFileExists)
    {
        // the savegame was written successfully, so we can rename the saved file
        // to the requested name (from gameXXX.sav_tmp to gameXXX.sav)
#ifdef _WIN32
        _unlink(pzFile);
#endif
        if (rename(saveFileName, pzFile))
            ThrowError("File error #%d replacing save file.", errno);
    }
    int const nSlot = gGameOptions.nSaveGameSlot;
    if ((unsigned)nSlot < 10)
    {
        CacheSavePreview(nSlot, pendingPreviewValid ? pendingPreview : nullptr);
        gSaveGameStats[nSlot] = stats;
    }
    pendingPreviewValid = false;
}

class MyLoadSave : public LoadSave
{
public:
    virtual void Load(void);
    virtual void Save(void);
};

void MyLoadSave::Load(void)
{
    psky_t *pSky = tileSetupSky(0);
    int id;
    Read(&id, sizeof(id));
    if (id != 0x5653424e/*'VSBN'*/)
        ThrowError("Old saved game found");
    short version;
    Read(&version, sizeof(version));
    if (version != BYTEVERSION)
        ThrowError("Incompatible version of saved game found!");
    uint32_t uINICRC;
    Read(&uINICRC, sizeof(uINICRC));
    if (uINICRC != Bcrc32(pINISelected->zName, (unsigned int)BMAX_PATH, 0))
        ThrowError("Not using the same INI as saved game!");
    Read(&gGameOptions, sizeof(gGameOptions));
    Read(&numsectors, sizeof(numsectors));
    Read(&numwalls, sizeof(numwalls));
    Read(&numsectors, sizeof(numsectors));
    int nNumSprites;
    Read(&nNumSprites, sizeof(nNumSprites));
    memset(sector, 0, sizeof(sector[0])*kMaxSectors);
    memset(wall, 0, sizeof(wall[0])*kMaxWalls);
    memset(sprite, 0, sizeof(sprite[0])*kMaxSprites);
    memset(spriteext, 0, sizeof(spriteext[0])*kMaxSprites);
    memset(qsprite_filler,0,sizeof(qsprite_filler[0])*kMaxSprites);
    memset(qsector_filler,0,sizeof(qsector_filler[0])*kMaxSectors);
    Read(sector, sizeof(sector[0])*numsectors);
    Read(wall, sizeof(wall[0])*numwalls);
    Read(sprite, sizeof(sprite[0])*kMaxSprites);
    Read(spriteext, sizeof(spriteext[0])*kMaxSprites);
    Read(qsector_filler, sizeof(qsector_filler[0])*numsectors);
    Read(qsprite_filler, sizeof(qsprite_filler[0])*kMaxSprites);
    Read(&randomseed, sizeof(randomseed));
    Read(&parallaxtype, sizeof(parallaxtype));
    Read(&showinvisibility, sizeof(showinvisibility));
    Read(&pSky->horizfrac, sizeof(pSky->horizfrac));
    Read(&pSky->yoffs, sizeof(pSky->yoffs));
    Read(&pSky->yscale, sizeof(pSky->yscale));
    Read(&gVisibility, sizeof(gVisibility));
    Read(&g_visibility, sizeof(g_visibility));
    Read(&parallaxvisibility, sizeof(parallaxvisibility));
    Read(pSky->tileofs, sizeof(pSky->tileofs));
    Read(&pSky->lognumtiles, sizeof(pSky->lognumtiles));
    Read(headspritesect, sizeof(headspritesect));
    Read(headspritestat, sizeof(headspritestat));
    Read(prevspritesect, sizeof(prevspritesect));
    Read(prevspritestat, sizeof(prevspritestat));
    Read(nextspritesect, sizeof(nextspritesect));
    Read(nextspritestat, sizeof(nextspritestat));
    Read(show2dsector, sizeof(show2dsector));
    Read(show2dwall, sizeof(show2dwall));
    Read(show2dsprite, sizeof(show2dsprite));
    Read(&automapping, sizeof(automapping));
    Read(gotpic, sizeof(gotpic));
    Read(gotsector, sizeof(gotsector));
    Read(&gFrameClock, sizeof(gFrameClock));
    Read(&gFrameTicks, sizeof(gFrameTicks));
    Read(&gFrame, sizeof(gFrame));
    ClockTicks nGameClock;
    Read(&totalclock, sizeof(totalclock));
    totalclock = nGameClock;
    Read(&gLevelTime, sizeof(gLevelTime));
    Read(&gPaused, sizeof(gPaused));
    Read(&gbAdultContent, sizeof(gbAdultContent));
    Read(baseWall, sizeof(baseWall[0])*numwalls);
    Read(baseSprite, sizeof(baseSprite[0])*nNumSprites);
    Read(baseFloor, sizeof(baseFloor[0])*numsectors);
    Read(baseCeil, sizeof(baseCeil[0])*numsectors);
    Read(velFloor, sizeof(velFloor[0])*numsectors);
    Read(velCeil, sizeof(velCeil[0])*numsectors);
    Read(&gHitInfo, sizeof(gHitInfo));
    Read(&byte_1A76C6, sizeof(byte_1A76C6));
    Read(&byte_1A76C8, sizeof(byte_1A76C8));
    Read(&byte_1A76C7, sizeof(byte_1A76C7));
    Read(&byte_19AE44, sizeof(byte_19AE44));
    Read(gStatCount, sizeof(gStatCount));
    Read(nextXSprite, sizeof(nextXSprite));
    Read(nextXWall, sizeof(nextXWall));
    Read(nextXSector, sizeof(nextXSector));
    memset(xsprite, 0, sizeof(xsprite));
    for (int nSprite = 0; nSprite < kMaxSprites; nSprite++)
    {
        if (sprite[nSprite].statnum < kMaxStatus)
        {
            int nXSprite = sprite[nSprite].extra;
            if (nXSprite > 0)
                Read(&xsprite[nXSprite], sizeof(XSPRITE));
        }
    }
    memset(xwall, 0, sizeof(xwall));
    for (int nWall = 0; nWall < numwalls; nWall++)
    {
        int nXWall = wall[nWall].extra;
        if (nXWall > 0)
            Read(&xwall[nXWall], sizeof(XWALL));
    }
    memset(xsector, 0, sizeof(xsector));
    for (int nSector = 0; nSector < numsectors; nSector++)
    {
        int nXSector = sector[nSector].extra;
        if (nXSector > 0)
            Read(&xsector[nXSector], sizeof(XSECTOR));
    }
    Read(xvel, nNumSprites*sizeof(xvel[0]));
    Read(yvel, nNumSprites*sizeof(yvel[0]));
    Read(zvel, nNumSprites*sizeof(zvel[0]));
    Read(&gMapRev, sizeof(gMapRev));
    Read(&gSongId, sizeof(gSongId));
    Read(&gSkyCount, sizeof(gSkyCount));
    Read(&gFogMode, sizeof(gFogMode));
#ifdef NOONE_EXTENSIONS
    Read(&gModernMap, sizeof(gModernMap));
#endif
#ifdef YAX_ENABLE
    Read(&numyaxbunches, sizeof(numyaxbunches));
#endif
    psky_t skyInfo;
    Read(&skyInfo, sizeof(skyInfo));

    *tileSetupSky(0) = skyInfo;
    gCheatMgr.ResetCheats();

}

void MyLoadSave::Save(void)
{
    psky_t *pSky = tileSetupSky(0);
    int nNumSprites = 0;
    int id = 0x5653424e/*'VSBN'*/;
    Write(&id, sizeof(id));
    short version = BYTEVERSION;
    Write(&version, sizeof(version));
    uint32_t uINICRC = Bcrc32(pINISelected->zName, (unsigned int)BMAX_PATH, 0);
    Write(&uINICRC, sizeof(uINICRC));
    for (int nSprite = 0; nSprite < kMaxSprites; nSprite++)
    {
        if (sprite[nSprite].statnum < kMaxStatus && nSprite > nNumSprites)
            nNumSprites = nSprite;
    }
    nNumSprites++;
    Write(&gGameOptions, sizeof(gGameOptions));
    Write(&numsectors, sizeof(numsectors));
    Write(&numwalls, sizeof(numwalls));
    Write(&numsectors, sizeof(numsectors));
    Write(&nNumSprites, sizeof(nNumSprites));
    Write(sector, sizeof(sector[0])*numsectors);
    Write(wall, sizeof(wall[0])*numwalls);
    Write(sprite, sizeof(sprite[0])*kMaxSprites);
    Write(spriteext, sizeof(spriteext[0])*kMaxSprites);
    Write(qsector_filler, sizeof(qsector_filler[0])*numsectors);
    Write(qsprite_filler, sizeof(qsprite_filler[0])*kMaxSprites);
    Write(&randomseed, sizeof(randomseed));
    Write(&parallaxtype, sizeof(parallaxtype));
    Write(&showinvisibility, sizeof(showinvisibility));
    Write(&pSky->horizfrac, sizeof(pSky->horizfrac));
    Write(&pSky->yoffs, sizeof(pSky->yoffs));
    Write(&pSky->yscale, sizeof(pSky->yscale));
    Write(&gVisibility, sizeof(gVisibility));
    Write(&g_visibility, sizeof(g_visibility));
    Write(&parallaxvisibility, sizeof(parallaxvisibility));
    Write(pSky->tileofs, sizeof(pSky->tileofs));
    Write(&pSky->lognumtiles, sizeof(pSky->lognumtiles));
    Write(headspritesect, sizeof(headspritesect));
    Write(headspritestat, sizeof(headspritestat));
    Write(prevspritesect, sizeof(prevspritesect));
    Write(prevspritestat, sizeof(prevspritestat));
    Write(nextspritesect, sizeof(nextspritesect));
    Write(nextspritestat, sizeof(nextspritestat));
    Write(show2dsector, sizeof(show2dsector));
    Write(show2dwall, sizeof(show2dwall));
    Write(show2dsprite, sizeof(show2dsprite));
    Write(&automapping, sizeof(automapping));
    Write(gotpic, sizeof(gotpic));
    Write(gotsector, sizeof(gotsector));
    Write(&gFrameClock, sizeof(gFrameClock));
    Write(&gFrameTicks, sizeof(gFrameTicks));
    Write(&gFrame, sizeof(gFrame));
    ClockTicks nGameClock = totalclock;
    Write(&nGameClock, sizeof(nGameClock));
    Write(&gLevelTime, sizeof(gLevelTime));
    Write(&gPaused, sizeof(gPaused));
    Write(&gbAdultContent, sizeof(gbAdultContent));
    Write(baseWall, sizeof(baseWall[0])*numwalls);
    Write(baseSprite, sizeof(baseSprite[0])*nNumSprites);
    Write(baseFloor, sizeof(baseFloor[0])*numsectors);
    Write(baseCeil, sizeof(baseCeil[0])*numsectors);
    Write(velFloor, sizeof(velFloor[0])*numsectors);
    Write(velCeil, sizeof(velCeil[0])*numsectors);
    Write(&gHitInfo, sizeof(gHitInfo));
    Write(&byte_1A76C6, sizeof(byte_1A76C6));
    Write(&byte_1A76C8, sizeof(byte_1A76C8));
    Write(&byte_1A76C7, sizeof(byte_1A76C7));
    Write(&byte_19AE44, sizeof(byte_19AE44));
    Write(gStatCount, sizeof(gStatCount));
    Write(nextXSprite, sizeof(nextXSprite));
    Write(nextXWall, sizeof(nextXWall));
    Write(nextXSector, sizeof(nextXSector));
    for (int nSprite = 0; nSprite < kMaxSprites; nSprite++)
    {
        if (sprite[nSprite].statnum < kMaxStatus)
        {
            int nXSprite = sprite[nSprite].extra;
            if (nXSprite > 0)
                Write(&xsprite[nXSprite], sizeof(XSPRITE));
        }
    }
    for (int nWall = 0; nWall < numwalls; nWall++)
    {
        int nXWall = wall[nWall].extra;
        if (nXWall > 0)
            Write(&xwall[nXWall], sizeof(XWALL));
    }
    for (int nSector = 0; nSector < numsectors; nSector++)
    {
        int nXSector = sector[nSector].extra;
        if (nXSector > 0)
            Write(&xsector[nXSector], sizeof(XSECTOR));
    }
    Write(xvel, nNumSprites*sizeof(xvel[0]));
    Write(yvel, nNumSprites*sizeof(yvel[0]));
    Write(zvel, nNumSprites*sizeof(zvel[0]));
    Write(&gMapRev, sizeof(gMapRev));
    Write(&gSongId, sizeof(gSongId));
    Write(&gSkyCount, sizeof(gSkyCount));
    Write(&gFogMode, sizeof(gFogMode));
#ifdef NOONE_EXTENSIONS
    Write(&gModernMap, sizeof(gModernMap));
#endif
#ifdef YAX_ENABLE
    Write(&numyaxbunches, sizeof(numyaxbunches));
#endif
    psky_t skyInfo = *tileSetupSky(0);
    Write(&skyInfo, sizeof(skyInfo));
}

void LoadSavedInfo(void)
{
    for (int nSlot = 0; nSlot < 10; nSlot++)
    {
        ClearSavePreview(nSlot);
        gSaveGameStats[nSlot] = {};
    }
    int const bakpathsearchmode = pathsearchmode;
    pathsearchmode = 1;
    auto pList = klistpath((g_modDir[0] != '/') ? g_modDir : "./", "game*.sav", BUILDVFS_FIND_FILE);
    for (auto pIterator = pList; pIterator != NULL; pIterator = pIterator->next)
    {
        uint32_t uINICRC = 0;
        int hFile = kopen4loadfrommod(pIterator->name, 0);
        if (hFile == -1)
            continue;
        int id = 0;
        short version = 0;
        if ((uint32_t)kread(hFile, &id, sizeof(id)) != sizeof(id))
        {
            kclose(hFile);
            continue;
        }
        if (id != 0x5653424e/*'VSBN'*/)
        {
            kclose(hFile);
            continue;
        }
        if (kread(hFile, &version, sizeof(version)) != sizeof(version) || version != BYTEVERSION)
        {
            kclose(hFile);
            continue;
        }
        if (kread(hFile, &uINICRC, sizeof(uINICRC)) != sizeof(uINICRC)
            || uINICRC != Bcrc32(pINISelected->zName, (unsigned int)BMAX_PATH, 0))
        {
            kclose(hFile);
            continue;
        }
        GAMEOPTIONS options;
        if (kread(hFile, &options, sizeof(options)) != sizeof(options) || (unsigned)options.nSaveGameSlot >= 10)
        {
            kclose(hFile);
            continue;
        }
        int const nSlot = options.nSaveGameSlot;
        char slotFileName[16];
        Bsnprintf(slotFileName, sizeof(slotFileName), "game00%02d.sav", nSlot);
        // Match the file LoadGame opens for this slot; backups and files with
        // a mismatched header must not replace its title or preview.
        if (Bstrcmp(pIterator->name, slotFileName))
        {
            kclose(hFile);
            continue;
        }
        options.szUserGameName[sizeof(options.szUserGameName) - 1] = '\0';
        options.zLevelName[sizeof(options.zLevelName) - 1] = '\0';
        gSaveGameOptions[nSlot] = options;
        UpdateSavedInfo(nSlot);
        ReadSavePreview(hFile, nSlot);
        ReadSaveStats(hFile, nSlot);
        kclose(hFile);
    }
    klistfree(pList);
    pathsearchmode = bakpathsearchmode;
}

void UpdateSavedInfo(int nSlot)
{
    if ((unsigned)nSlot >= 10 || (unsigned)gSaveGameOptions[nSlot].nSaveGameSlot >= 10)
        return;
    int const targetSlot = gSaveGameOptions[nSlot].nSaveGameSlot;
    Bstrncpyz(strRestoreGameStrings[targetSlot], gSaveGameOptions[nSlot].szUserGameName, sizeof(strRestoreGameStrings[0]));
    restoreGameDifficulty[targetSlot] = ClipRange(gSaveGameOptions[nSlot].nDifficulty, 0, 4);
}

static MyLoadSave *myLoadSave;


void LoadSaveSetup(void)
{
    void ActorLoadSaveConstruct(void);
    void AILoadSaveConstruct(void);
    void EndGameLoadSaveConstruct(void);
    void EventQLoadSaveConstruct(void);
    void LevelsLoadSaveConstruct(void);
    void MessagesLoadSaveConstruct(void);
    void MirrorLoadSaveConstruct(void);
    void PlayerLoadSaveConstruct(void);
    void SeqLoadSaveConstruct(void);
    void TriggersLoadSaveConstruct(void);
    void ViewLoadSaveConstruct(void);
    void WarpLoadSaveConstruct(void);
    void WeaponLoadSaveConstruct(void);
#ifdef NOONE_EXTENSIONS
    void nnExtLoadSaveConstruct(void);
#endif
    myLoadSave = new MyLoadSave();
    
    // NoOne: Seq must be in top of AI because of AISTATE callbacks (and seq callbacks in general).
    // Ex: cultist throwing TNT - if you saved the game while it plays animation
    // before fire trigger seq flag, you will get assertion fail on load since
    // target == -1 after aiInitSprite() call.

    // Another reason is that it just spawns the wrong animation.
    // Ex: save the game when some dude moves, load it and quckly
    // use ONERING cheat. You will see that dude plays moving
    // animation, but have idle AISTATE after aiInitSprite()
    // call. In vanilla they just stand still.

    SeqLoadSaveConstruct();
    ActorLoadSaveConstruct();
    AILoadSaveConstruct();
    EndGameLoadSaveConstruct();
    EventQLoadSaveConstruct();
    LevelsLoadSaveConstruct();
    MessagesLoadSaveConstruct();
    MirrorLoadSaveConstruct();
    PlayerLoadSaveConstruct();
    TriggersLoadSaveConstruct();
    ViewLoadSaveConstruct();
    WarpLoadSaveConstruct();
    WeaponLoadSaveConstruct();
#ifdef NOONE_EXTENSIONS
    nnExtLoadSaveConstruct();
#endif
}
