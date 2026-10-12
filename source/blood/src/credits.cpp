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
#include "credits.h"
#include "build.h"
#include "compat.h"
#include "SmackerDecoder.h"
#include "fx_man.h"
#include "keyboard.h"
#include "common_game.h"
#include "blood.h"
#include "config.h"
#include "controls.h"
#include "globals.h"
#include "resource.h"
#include "screen.h"
#include "sound.h"
#include "view.h"
#include "cinematic_ogv.h"
#include "polymost.h"

#include <algorithm>
#include <string>
#include <vector>

extern char textfont[2048];

inline char keyJoyGetScan(void)
{
    char ch = keyGetScan();
    if (CONTROL_JoystickEnabled && !ch)
    {
        int32_t joy = JOYSTICK_GetControllerButtons();
        JOYSTICK_ClearAllButtons();
        if ((joy == (1 << CONTROLLER_BUTTON_A)) || (joy == (1 << CONTROLLER_BUTTON_B)) || (joy == (1 << CONTROLLER_BUTTON_START)))
            ch = 1;
    }
    return ch;
}

char Wait(int nTicks)
{
    totalclock = 0;
    ctrlClearAllInput();
    while (totalclock < nTicks)
    {
        gameHandleEvents();
        if (keyJoyGetScan())
            return FALSE;
    }
    return TRUE;
}

char DoFade(char r, char g, char b, int nTicks)
{
    dassert(nTicks > 0);
    scrSetupFade(r, g, b);
    totalclock = gFrameClock = 0;
    do
    {
        while (totalclock < gFrameClock) { gameHandleEvents();};
        gFrameClock += 2;
        scrNextPage();
        scrFadeAmount(divscale16(ClipHigh((int)totalclock, nTicks), nTicks));
        if (keyJoyGetScan())
            return FALSE;
    } while (totalclock <= nTicks);
    return TRUE;
}

char DoUnFade(int nTicks)
{
    dassert(nTicks > 0);
    scrSetupUnfade();
    totalclock = gFrameClock = 0;
    do
    {
        while (totalclock < gFrameClock) { gameHandleEvents(); };
        gFrameClock += 2;
        scrNextPage();
        scrFadeAmount(0x10000-divscale16(ClipHigh((int)totalclock, nTicks), nTicks));
        if (keyJoyGetScan())
            return FALSE;
    } while (totalclock <= nTicks);
    return TRUE;
}

void credLogosDos(void)
{
    char bShift = keystatus[sc_LeftShift] | keystatus[sc_RightShift];
    if (bShift)
        return;

    videoClearScreen(0);
    DoUnFade(1);

    if (!credPlaySmk("LOGO.SMK", "logo811m.wav", 300) && !credPlaySmk("movie/LOGO.SMK", "movie/logo811m.wav", 300))
    {
        rotatesprite(160<<16, 100<<16, 65536, 0, 2050, 0, 0, 0x4a, 0, 0, xdim-1, ydim-1);
        scrNextPage();
        sndStartSample("THUNDER2", 128, -1);
        if (Wait(360))
        {
            if (videoGetRenderMode() == REND_CLASSIC)
                DoFade(0, 0, 0, 60);
        }
    }

    if (videoGetRenderMode() == REND_CLASSIC)
        credReset();

    if (!credPlaySmk("GTI.SMK", "gti.wav", 301) && !credPlaySmk("movie/GTI.SMK", "movie/gti.wav", 301))
    {
        rotatesprite(160<<16, 100<<16, 65536, 0, 2052, 0, 0, 0x0a, 0, 0, xdim-1, ydim-1);
        scrNextPage();
        sndStartSample("THUNDER2", 128, -1);
        if (Wait(360))
        {
            if (videoGetRenderMode() == REND_CLASSIC)
                DoFade(0, 0, 0, 60);
        }
    }

    credReset();

    videoClearScreen(gMenuColor);
    rotatesprite(160<<16, 100<<16, 65536, 0, gMenuPicnum, 0, 0, 0xa, 0, 0, xdim-1, ydim-1);
    scrNextPage();
    sndStartSample("THUNDER2", 128, -1);
    sndPlaySpecialMusicOrNothing(MUS_INTRO);
    Wait(360);
    sndFadeSong(4000);
}

void credReset(void)
{
    videoClearScreen(0);
    scrNextPage();
    DoFade(0,0,0,1);
    scrSetupUnfade();
    DoUnFade(1);
}

int credKOpen4Load(char *&pzFile)
{
    int nLen = strlen(pzFile);
    for (int i = 0; i < nLen; i++)
    {
        if (pzFile[i] == '\\')
            pzFile[i] = '/';
    }
    int nHandle = kopen4loadfrommod(pzFile, 0);
    if (nHandle == -1)
    {
        // Hack
        if (nLen >= 3 && isalpha(pzFile[0]) && pzFile[1] == ':' && pzFile[2] == '/')
        {
            pzFile += 3;
            nHandle = kopen4loadfrommod(pzFile, 0);
        }
    }
    return nHandle;
}

#define kSMKPal 5
#define kSMKTile (MAXTILES-1)

char credPlaySmk(const char *_pzSMK, const char *_pzWAV, int nWav)
{
#if 0
    CSMKPlayer smkPlayer;
    if (dword_148E14 >= 0)
    {
        if (toupper(*pzSMK) == 'A'+dword_148E14)
        {
            if (Redbook.sub_82258() == 0 || Redbook.sub_82258() > 20)
                return FALSE;
        }
        Redbook.sub_82554();
    }
    smkPlayer.sub_82E6C(pzSMK, pzWAV);
#endif
    if (Bstrlen(_pzSMK) == 0)
        return false;
    char *pzSMK = Xstrdup(_pzSMK);
    char *pzWAV = Xstrdup(_pzWAV);
    char *pzSMK_ = pzSMK;
    char *pzWAV_ = pzWAV;
    int nHandleSMK = credKOpen4Load(pzSMK);
    if (nHandleSMK == -1)
    {
        Xfree(pzSMK_);
        Xfree(pzWAV_);
        return FALSE;
    }
    kclose(nHandleSMK);
    SmackerHandle hSMK = Smacker_Open(pzSMK);
    if (!hSMK.isValid)
    {
        Xfree(pzSMK_);
        Xfree(pzWAV_);
        return FALSE;
    }
    uint32_t nWidth, nHeight;
    Smacker_GetFrameSize(hSMK, nWidth, nHeight);
    int nFrameRate = Smacker_GetFrameRate(hSMK);
    int nFrames = Smacker_GetNumFrames(hSMK);
    if (!nWidth || !nHeight || !nFrames || !nFrameRate)
    {
        Smacker_Close(hSMK);
        Xfree(pzSMK_);
        Xfree(pzWAV_);
        return FALSE;
    }

    uint8_t *pFrame = (uint8_t*)Xcalloc(1,nWidth*nHeight);
    if (!pFrame)
    {
        Smacker_Close(hSMK);
        Xfree(pzSMK_);
        Xfree(pzWAV_);
        return FALSE;
    }

    walock[kSMKTile] = CACHE1D_PERMANENT;
    waloff[kSMKTile] = (intptr_t)pFrame;
    tileSetSize(kSMKTile, nHeight, nWidth);

    uint8_t palette[768];
    Smacker_GetPalette(hSMK, palette);
    paletteSetColorTable(kSMKPal, palette);
    videoSetPalette(gBrightness>>2, kSMKPal, 8+2);
#ifdef USE_OPENGL
    const int bakUseindexedcolortextures = r_useindexedcolortextures;
    r_useindexedcolortextures = 1;
    osdcmd_restartvid(NULL);
#endif

    auto const oyxaspect = yxaspect;

    int nScale;
    int32_t nStat;

    if (nWidth <= 320 && nHeight <= 200)
    {
        if ((nWidth / (nHeight * 1.2f)) > (1.f * xdim / ydim))
            nScale = divscale16(320 * xdim * 3, nWidth * ydim * 4);
        else
            nScale = divscale16(200, nHeight);

        nStat = 2|4|8|64;
    }
    else
    {
        // DOS Blood v1.11: 320x240, 320x320, 640x400, and 640x480 SMKs all display 1:1 and centered in a 640x480 viewport
        nScale = tabledivide32(scale(65536, ydim << 2, xdim * 3), ((max(nHeight, 240+1u) + 239) / 240));
        nStat = 2|4|8|64|1024;
        renderSetAspect(viewingrange, 65536);
    }

    if (nWav)
        sndStartWavID(nWav, FXVolume);
    else
    {
        int nHandleWAV = credKOpen4Load(pzWAV);
        if (nHandleWAV != -1)
        {
            kclose(nHandleWAV);
            sndStartWavDisk(pzWAV, FXVolume);
        }
    }

    UpdateDacs(0, true);

    gameHandleEvents();
    ClockTicks nStartTime = totalclock;

    ctrlClearAllInput();

    int nFrame = 0;
    do
    {
        gameHandleEvents();
        if (scale((int)(totalclock-nStartTime), nFrameRate, kTicRate) < nFrame)
            continue;

        if (KB_KeyPressed(sc_Escape) || (nFrame > nFrameRate && ctrlCheckAllInput()))
            break;

        videoClearScreen(0);
        Smacker_GetPalette(hSMK, palette);
        paletteSetColorTable(kSMKPal, palette);
        videoSetPalette(gBrightness >> 2, kSMKPal, 0);
        tileInvalidate(kSMKTile, 0, 1 << 4);  // JBF 20031228
        Smacker_GetFrame(hSMK, pFrame);

        rotatesprite_fs(160<<16, 100<<16, nScale, 512, kSMKTile, 0, 0, nStat);

        videoNextPage();

        ctrlClearAllInput();
        nFrame++;
        Smacker_GetNextFrame(hSMK);
    } while(nFrame < nFrames);

    Smacker_Close(hSMK);
    ctrlClearAllInput();
    FX_StopAllSounds();
    renderSetAspect(viewingrange, oyxaspect);
    videoSetPalette(gBrightness >> 2, 0, 8+2);
#ifdef USE_OPENGL
    r_useindexedcolortextures = bakUseindexedcolortextures;
    osdcmd_restartvid(NULL);
#endif
    walock[kSMKTile] = 0;
    waloff[kSMKTile] = 0;
    tileSetSize(kSMKTile, 0, 0);
    Xfree(pFrame);
    Xfree(pzSMK_);
    Xfree(pzWAV_);

    return TRUE;
}

namespace
{
// Build's built-in font remains available before/after a level and does not
// depend on a movie palette or on mod-supplied HUD tiles.
struct CinematicPlayer
{
    SmackerHandle smk{false, -1};
    CinematicOgv *ogv = nullptr;
    unsigned width = 0, height = 0, frames = 0, decoded = 0;
    double fps = 0, aspect = 1;
    std::vector<uint8_t> pixels, rgba;
    uint8_t palette[768]{};
    bool changed = false, audioStarted = false, paletteChanged = false, cubePaletteActive = false;
    int savedPalette = 0, voice = 0;
    DICTNODE *waveResource = nullptr;
    std::vector<char> wave;
    const char *waveData = nullptr;
    unsigned waveSize = 0;
    uint64_t ogvGeneration = 0;
#ifdef USE_OPENGL
    GLuint texture = 0;
#endif
} cinematic;
#ifdef USE_OPENGL
bool cinematicCanvasQuads = false;
#endif

std::string cinematicNormalize(const char *name)
{
    std::string path = name ? name : "";
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.size() >= 3 && isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && path[2] == '/')
        path.erase(0, 3);
    return path;
}

std::vector<std::string> cinematicPaths(const char *name, bool video)
{
    std::vector<std::string> result;
    auto add = [&](std::string path)
    {
        if (std::find(result.begin(), result.end(), path) == result.end()) result.push_back(path);
    };
    std::string path = cinematicNormalize(name);
    if (path.empty()) return result;
    std::string base = path.substr(path.find_last_of('/') + 1);
    std::vector<std::string> paths{path, base};
    for (auto directory : {"movie/", "movies/", "video/", "videos/", "cutscenes/"}) paths.emplace_back(directory + base);
    // Re-encoded cinematics can supply their own soundtrack. Prefer them to
    // the original Smacker copy, including when the INI still names .SMK.
    for (int format = 0; format < (video ? 3 : 1); ++format)
        for (auto candidate : paths)
        {
            if (video && format < 2)
            {
                auto dot = candidate.find_last_of('.');
                if (dot != std::string::npos && dot > candidate.find_last_of('/') + 1)
                    candidate.resize(dot);
                candidate += format == 0 ? ".ogv" : ".smk";
            }
            add(candidate);
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return tolower(c); });
            add(candidate);
            auto slash = candidate.find_last_of('/');
            std::transform(candidate.begin() + (slash == std::string::npos ? 0 : slash + 1), candidate.end(),
                           candidate.begin() + (slash == std::string::npos ? 0 : slash + 1), [](unsigned char c) { return toupper(c); });
            add(candidate);
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return toupper(c); });
            add(candidate);
        }
    return result;
}

bool cinematicAccelerated()
{
#ifdef USE_OPENGL
    return videoGetRenderMode() >= REND_POLYMOST;
#else
    return false;
#endif
}

bool cinematicSmackerHeader(const uint8_t *header, int bytes, int fileSize)
{
    if (bytes < 104 || fileSize <= 104) return false;
    auto read32 = [](uint8_t const *p) { return unsigned(p[0]) | unsigned(p[1]) << 8 | unsigned(p[2]) << 16 | unsigned(p[3]) << 24; };
    unsigned const width = read32(header + 4), height = read32(header + 8);
    unsigned const frames = read32(header + 12), timing = read32(header + 16);
    uint64_t const count = uint64_t(frames) + (read32(header + 20) & 1);
    unsigned const trees = read32(header + 52);
    // The original decoder assumes complete tables, at least one frame and
    // four-pixel blocks. Check these before it indexes/allocates from a header.
    if (!width || !height || width > 4096 || height > 4096 || (width & 3) || (height & 3)
        || !frames || count > 0xffffff || timing == 0x80000000u || !trees
        || uint64_t(104) + count * 5 + trees >= uint64_t(fileSize)) return false;
    // Bound the expanded Huffman trees independently of compressed file size.
    for (int offset = 56; offset <= 68; offset += 4)
        if (read32(header + offset) > 4 * 1024 * 1024) return false;
    return true;
}

void cinematicSetPalette(const uint8_t *palette)
{
    if (cinematicAccelerated()) return;
    if (!cinematic.paletteChanged)
    {
        cinematic.savedPalette = curbasepal;
        cinematic.paletteChanged = true;
    }
    paletteSetColorTable(kSMKPal, palette);
    videoSetPalette(gBrightness >> 2, kSMKPal, 8 + 2);
}

void cinematicCubePalette()
{
    if (cinematic.cubePaletteActive) return;
    uint8_t palette[768];
    for (unsigned i = 0; i < 256; ++i)
    {
        palette[i * 3 + 0] = ((i >> 5) & 7) * 255 / 7;
        palette[i * 3 + 1] = ((i >> 2) & 7) * 255 / 7;
        palette[i * 3 + 2] = (i & 3) * 255 / 3;
    }
    cinematicSetPalette(palette);
    cinematic.cubePaletteActive = true;
}

void cinematicCanvasBegin(bool textured = false)
{
#ifdef USE_OPENGL
    if (cinematicAccelerated())
    {
        buildgl_useShaderProgram(0);
        for (unsigned unit = 0; unit < 5; ++unit)
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glDisable(GL_TEXTURE_2D);
        }
        glActiveTexture(GL_TEXTURE0);
        if (textured) glEnable(GL_TEXTURE_2D);
        glViewport(0, 0, xdim, ydim);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_FOG);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glColor4f(1.f, 1.f, 1.f, 1.f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        if (textured) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, xdim, ydim, 0, -1, 1);
        cinematicCanvasQuads = !textured;
        if (cinematicCanvasQuads) glBegin(GL_QUADS);
        return;
    }
#else
    UNREFERENCED_PARAMETER(textured);
#endif
    videoBeginDrawing();
}

void cinematicCanvasEnd()
{
#ifdef USE_OPENGL
    if (cinematicAccelerated())
    {
        if (cinematicCanvasQuads) glEnd();
        cinematicCanvasQuads = false;
        glDepthMask(GL_TRUE);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        polymost2d = 0;
        // Cinematics use the fixed-function frontend (also native Vulkan).
        // Invalidate the game's GL state cache before OSD or gameplay draws.
        polymost_resetState();
        return;
    }
#endif
    videoEndDrawing();
}

void cinematicRect(int x, int y, int w, int h, bool white)
{
    if (w <= 0 || h <= 0) return;
#ifdef USE_OPENGL
    if (cinematicAccelerated())
    {
        glColor3f(white ? 1.f : 0.f, white ? 1.f : 0.f, white ? 1.f : 0.f);
        glVertex2i(x, y); glVertex2i(x + w, y);
        glVertex2i(x + w, y + h); glVertex2i(x, y + h);
        return;
    }
#endif
    int const right = std::min(x + w, xdim), bottom = std::min(y + h, ydim);
    x = std::max(0, x); y = std::max(0, y);
    if (right <= x) return;
    for (; y < bottom; ++y)
        Bmemset(reinterpret_cast<uint8_t *>(frameplace) + y * bytesperline + x, white ? 255 : 0, right - x);
}

void cinematicText(const char *text, int x, int y, int scale)
{
    for (auto p = reinterpret_cast<const unsigned char *>(text); *p; ++p, x += 8 * scale)
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (static_cast<unsigned char>(textfont[*p * 8 + row]) & (128 >> col))
                    cinematicRect(x + col * scale, y + row * scale, scale, scale, true);
}

void cinematicSmackerPixels()
{
    Smacker_GetFrame(cinematic.smk, cinematic.pixels.data());
    Smacker_GetPalette(cinematic.smk, cinematic.palette);
    if (cinematicAccelerated())
    {
        for (size_t i = 0; i < cinematic.pixels.size(); ++i)
        {
            auto p = cinematic.palette + cinematic.pixels[i] * 3;
            cinematic.rgba[i * 4 + 0] = p[0];
            cinematic.rgba[i * 4 + 1] = p[1];
            cinematic.rgba[i * 4 + 2] = p[2];
            cinematic.rgba[i * 4 + 3] = 255;
        }
    }
    else
    {
        // Reserve exact black/white for letterboxing and the skip/wait UI.
        // Remap only the two displaced movie colors to their nearest match.
        unsigned remap[2]{0, 255};
        for (int entry = 0; entry < 2; ++entry)
        {
            int distance = INT_MAX;
            unsigned const source = entry ? 255 : 0;
            if (cinematic.palette[source * 3] == source && cinematic.palette[source * 3 + 1] == source
                && cinematic.palette[source * 3 + 2] == source) continue;
            for (unsigned i = 1; i < 255; ++i)
            {
                int d = 0;
                for (unsigned c = 0; c < 3; ++c)
                {
                    int v = cinematic.palette[source * 3 + c] - cinematic.palette[i * 3 + c];
                    d += v * v;
                }
                if (d < distance) { distance = d; remap[entry] = i; }
            }
        }
        for (auto &pixel : cinematic.pixels)
            if (pixel == 0 || pixel == 255) pixel = remap[pixel == 255];
        Bmemset(cinematic.palette, 0, 3);
        Bmemset(cinematic.palette + 255 * 3, 255, 3);
        cinematicSetPalette(cinematic.palette);
        cinematic.cubePaletteActive = false;
    }
    cinematic.changed = true;
}

void cinematicLoadWave(const char *wave, int resource)
{
    if (cinematic.ogv && cinematicOgvHasAudio(cinematic.ogv)) return;
    if (resource)
    {
        cinematic.waveResource = gSoundRes.Lookup(resource, "WAV");
        if (cinematic.waveResource)
        {
            cinematic.waveData = static_cast<const char *>(gSoundRes.Lock(cinematic.waveResource));
            cinematic.waveSize = cinematic.waveResource->size;
            return;
        }
    }
    for (auto const &path : cinematicPaths(wave, false))
    {
        int const handle = kopen4loadfrommod(path.c_str(), 0);
        if (handle < 0) continue;
        int const size = kfilelength(handle);
        if (size <= 0 || size > 256 * 1024 * 1024) { kclose(handle); continue; }
        cinematic.wave.resize(size);
        bool const complete = kread(handle, cinematic.wave.data(), size) == size;
        kclose(handle);
        if (!complete) { cinematic.wave.clear(); continue; }
        cinematic.waveData = cinematic.wave.data();
        cinematic.waveSize = size;
        break;
    }
}

void cinematicDrawFrame(const uint8_t *rgba)
{
    double aspect = cinematic.aspect;
    int w = xdim, h = ydim;
    if (aspect >= double(xdim) / ydim) h = static_cast<int>(xdim / aspect + .5);
    else w = static_cast<int>(ydim * aspect + .5);
    w = std::max(1, w); h = std::max(1, h);
    int const x = (xdim - w) / 2, y = (ydim - h) / 2;
    // The ordinary view may update the player's palette before this overlay.
    if (!cinematicAccelerated()) videoSetPalette(gBrightness >> 2, kSMKPal, 8 + 2);
    cinematicCanvasBegin(cinematicAccelerated());
#ifdef USE_OPENGL
    if (cinematicAccelerated())
    {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, cinematic.texture);
        buildgl_bindSamplerObject(0, 0);
        if (cinematic.changed)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cinematic.width, cinematic.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f); glVertex2i(x, y);
        glTexCoord2f(1.f, 0.f); glVertex2i(x + w, y);
        glTexCoord2f(1.f, 1.f); glVertex2i(x + w, y + h);
        glTexCoord2f(0.f, 1.f); glVertex2i(x, y + h);
        glEnd();
    }
    else
#endif
    {
        if (cinematic.ogv && cinematic.changed)
            for (size_t i = 0; i < cinematic.pixels.size(); ++i)
                cinematic.pixels[i] = (rgba[i * 4] & 0xe0) | ((rgba[i * 4 + 1] >> 3) & 0x1c) | (rgba[i * 4 + 2] >> 6);
        cinematicRect(0, 0, xdim, ydim, false);
        for (int row = 0; row < h; ++row)
        {
            auto out = reinterpret_cast<uint8_t *>(frameplace) + (y + row) * bytesperline + x;
            auto in = cinematic.pixels.data() + (uint64_t(row) * cinematic.height / h) * cinematic.width;
            for (int col = 0; col < w; ++col) out[col] = in[uint64_t(col) * cinematic.width / w];
        }
    }
    cinematicCanvasEnd();
    cinematic.changed = false;
}
} // namespace

bool credCinematicBegin(const char *video, const char *wave, int waveResource)
{
    credCinematicEnd();
    for (auto const &path : cinematicPaths(video, true))
    {
        int const handle = kopen4loadfrommod(path.c_str(), 0);
        if (handle < 0) continue;
        uint8_t header[104]{};
        int const fileSize = kfilelength(handle);
        int const bytes = kread(handle, header, sizeof(header));
        kclose(handle);
        if (bytes < 4) continue;
        if (!Bmemcmp(header, "OggS", 4))
        {
            cinematic.ogv = cinematicOgvOpen(path.c_str());
            if (!cinematic.ogv) continue;
            auto const &frame = cinematicOgvFrame(cinematic.ogv);
            cinematic.width = frame.width;
            cinematic.height = frame.height;
            cinematic.aspect = double(frame.width) / frame.height;
            if (frame.pixelAspectNumerator && frame.pixelAspectDenominator)
                cinematic.aspect *= double(frame.pixelAspectNumerator) / frame.pixelAspectDenominator;
            if (!cinematicAccelerated()) cinematicCubePalette();
        }
        else if (!Bmemcmp(header, "SMK2", 4) || !Bmemcmp(header, "SMK4", 4))
        {
            if (!cinematicSmackerHeader(header, bytes, fileSize)) continue;
            cinematic.smk = Smacker_Open(path.c_str());
            if (!cinematic.smk.isValid) continue;
            Smacker_GetFrameSize(cinematic.smk, cinematic.width, cinematic.height);
            cinematic.fps = Smacker_GetFrameRate(cinematic.smk);
            cinematic.frames = Smacker_GetNumFrames(cinematic.smk);
            if (!cinematic.frames || cinematic.fps <= 0) { credCinematicEnd(); continue; }
            cinematic.aspect = double(cinematic.width) / cinematic.height;
            // Original 320x200 cinematics target a 4:3 display with tall DOS
            // pixels; higher-resolution movies and OGV use square pixels.
            if (cinematic.width <= 320 && cinematic.height <= 200) cinematic.aspect /= 1.2;
            cinematic.rgba.resize(size_t(cinematic.width) * cinematic.height * 4);
        }
        else continue;
        cinematic.pixels.resize(size_t(cinematic.width) * cinematic.height);
        if (cinematic.smk.isValid)
        {
            Smacker_GetNextFrame(cinematic.smk);
            cinematicSmackerPixels();
        }
#ifdef USE_OPENGL
        if (cinematicAccelerated())
        {
            cinematicCanvasBegin(true);
            glGenTextures(1, &cinematic.texture);
            glBindTexture(GL_TEXTURE_2D, cinematic.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cinematic.width, cinematic.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            cinematicCanvasEnd();
        }
#endif
        cinematicLoadWave(wave, waveResource);
        LOG_F(INFO, "CINEMATIC opened path=%s format=%s width=%u height=%u", path.c_str(), cinematic.ogv ? "OGV" : "SMK", cinematic.width, cinematic.height);
        return true;
    }
    LOG_F(INFO, "CINEMATIC unavailable video=%s", video ? video : "");
    return false;
}

bool credCinematicFrame(unsigned elapsedMs)
{
    if (!cinematic.ogv && !cinematic.smk.isValid) return false;
    if (!cinematic.audioStarted)
    {
        cinematic.audioStarted = true;
        if (SoundToggle && cinematic.waveData)
            cinematic.voice = FX_Play(const_cast<char *>(cinematic.waveData), cinematic.waveSize, 0, -1, 0,
                                      FXVolume, FXVolume, FXVolume, FX_MUSIC_PRIORITY, fix16_one, intptr_t(&cinematic.voice));
    }
    const uint8_t *rgba = cinematic.rgba.data();
    if (cinematic.ogv)
    {
        if (!cinematicOgvUpdate(cinematic.ogv, elapsedMs)) return false;
        auto const &frame = cinematicOgvFrame(cinematic.ogv);
        cinematic.changed = cinematic.ogvGeneration != frame.generation;
        cinematic.ogvGeneration = frame.generation;
        rgba = frame.rgba;
    }
    else
    {
        unsigned const target = static_cast<unsigned>(elapsedMs * cinematic.fps / 1000.);
        if (target >= cinematic.frames) return false;
        bool advanced = false;
        while (cinematic.decoded < target)
        {
            Smacker_GetNextFrame(cinematic.smk);
            ++cinematic.decoded;
            advanced = true;
        }
        // Palettes may animate even when the pixel indices remain identical.
        if (advanced) cinematicSmackerPixels();
    }
    cinematicDrawFrame(rgba);
    return true;
}

void credCinematicEnd()
{
    if (cinematic.voice > 0) FX_StopSound(cinematic.voice);
    if (cinematic.waveResource) gSoundRes.Unlock(cinematic.waveResource);
    if (cinematic.ogv) cinematicOgvClose(cinematic.ogv);
    if (cinematic.smk.isValid) Smacker_Close(cinematic.smk);
#ifdef USE_OPENGL
    if (cinematic.texture) glDeleteTextures(1, &cinematic.texture);
#endif
    if (cinematic.paletteChanged) videoSetPalette(gBrightness >> 2, cinematic.savedPalette, 8 + 2);
    cinematic = CinematicPlayer{};
}

void credCinematicDrawWaiting()
{
    if (!cinematicAccelerated())
    {
        cinematicCubePalette();
        videoSetPalette(gBrightness >> 2, kSMKPal, 8 + 2);
    }
    int const scale = std::max(1, std::min(xdim / 640, ydim / 480));
    cinematicCanvasBegin();
    cinematicRect(0, 0, xdim, ydim, false);
    const char *message = "Other players are still watching the cinematic";
    int const width = int(Bstrlen(message)) * 8 * scale;
    if (width + 24 * scale <= xdim)
        cinematicText(message, (xdim - width) / 2, ydim / 2 - 4 * scale, scale);
    else
    {
        const char *first = "Other players are still watching";
        const char *second = "the cinematic";
        cinematicText(first, (xdim - int(Bstrlen(first)) * 8 * scale) / 2, ydim / 2 - 12 * scale, scale);
        cinematicText(second, (xdim - int(Bstrlen(second)) * 8 * scale) / 2, ydim / 2 + 4 * scale, scale);
    }
    cinematicCanvasEnd();
}

void credCinematicDrawSkip(float progress)
{
    int const scale = std::max(1, std::min(xdim / 640, ydim / 480));
    int const width = 196 * scale, height = 38 * scale;
    int const x = xdim - width - 12 * scale, y = ydim - height - 12 * scale;
    cinematicCanvasBegin();
    cinematicRect(x, y, width, height, false);
    cinematicRect(x + 4 * scale, y + 4 * scale, 56 * scale, 20 * scale, true);
    cinematicRect(x + 5 * scale, y + 5 * scale, 54 * scale, 18 * scale, false);
    cinematicText("SPACE", x + 12 * scale, y + 10 * scale, scale);
    cinematicText("Hold 3s to skip", x + 68 * scale, y + 10 * scale, scale);
    cinematicRect(x + 4 * scale, y + 30 * scale, 188 * scale, 3 * scale, true);
    cinematicRect(x + 5 * scale, y + 31 * scale, 186 * scale, scale, false);
    int const fill = static_cast<int>(186 * scale * std::max(0.f, std::min(1.f, progress)));
    cinematicRect(x + 5 * scale, y + 31 * scale, fill, scale, true);
    cinematicCanvasEnd();
}
