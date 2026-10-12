// Exercise the production decoder with real Ogg fixtures and a fake mixer/VFS.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "compat.h"
#include "vfs.h"
#include "fx_man.h"
#include "cinematic_ogv.h"

int32_t SoundToggle = 1, FXVolume = 200;
int FX_ErrorCode = 0;
static std::vector<FILE*> files;
static uint64_t bytesRead;
static void (*feed)(const char**, uint32_t*, void*);
static void* feedUser;
static int channels, sampleRate, audioStarts, audioStops;
static intptr_t callbackToken;
static bool mixerAvailable = true, unrelatedVoice = false;

void handle_memerr(int32_t) { abort(); }

extern "C" {
void* _xmalloc(bsize_t n) { void* p = malloc(n); assert(p || !n); return p; }
void* _xcalloc(bsize_t n, bsize_t s) { void* p = calloc(n, s); assert(p || !n || !s); return p; }
void* _xrealloc(void* p, bsize_t n) { p = realloc(p, n); assert(p || !n); return p; }
void _xfree(void* p) { free(p); }
int FX_StartDemandFeedPlayback(void (*fn)(const char**, uint32_t*, void*), int bits, int count, int rate,
                              int, int, int, int, int, fix16_t, intptr_t token, void* user)
{
    assert(bits == 16 && (count == 1 || count == 2) && !feed);
    if (!mixerAvailable) return FX_Error;
    feed = fn; feedUser = user; channels = count; sampleRate = rate; callbackToken = token;
    ++audioStarts;
    return 1;
}
int MV_Kill(int handle, bool callback)
{
    assert(handle == 1 && !callback && !unrelatedVoice);
    feed = nullptr; feedUser = nullptr; callbackToken = 0;
    ++audioStops;
    return MV_Ok;
}
}

buildvfs_kfd kopen4loadfrommod(const char* path, char)
{
    FILE* file = fopen(path, "rb");
    if (!file) return -1;
    files.push_back(file);
    return static_cast<int>(files.size() - 1);
}
int32_t kread(buildvfs_kfd fd, void* data, int32_t size)
{
    auto count = fread(data, 1, size, files[fd]);
    bytesRead += count;
    return static_cast<int32_t>(count);
}
void kclose(buildvfs_kfd fd) { assert(files[fd]); fclose(files[fd]); files[fd] = nullptr; }

static std::string directory;
static std::string path(const char* file) { return directory + "/" + file; }

static void testVideo(const char* file, bool audio)
{
    auto* movie = cinematicOgvOpen(path(file).c_str());
    assert(movie && !feed);
    auto const& first = cinematicOgvFrame(movie);
    assert(first.width == 62 && first.height == 46 && first.generation == 1);
    assert(std::abs(first.fps - 10) < .001 && cinematicOgvHasAudio(movie) == audio);
    FILE* reference = fopen((path(file) + ".rgba").c_str(), "rb");
    assert(reference);
    std::vector<uint8_t> expected(first.width * first.height * 4);
    assert(fread(expected.data(), 1, expected.size(), reference) == expected.size());
    fclose(reference);
    uint64_t colorError = 0;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        colorError += std::abs(int(first.rgba[i]) - expected[i]);
        if (i % 4 == 3) assert(first.rgba[i] == 255);
    }
    // The decoder's integer BT.601 matrix differs slightly from swscale's
    // rounding/chroma interpolation, especially at sharp color boundaries.
    assert(double(colorError) / expected.size() < 12);
    assert(cinematicOgvUpdate(movie, 0));
    assert(cinematicOgvUpdate(movie, 99) && cinematicOgvFrame(movie).generation == 1);
    assert(cinematicOgvUpdate(movie, 100) && cinematicOgvFrame(movie).generation == 2);
    assert(cinematicOgvUpdate(movie, 0) && cinematicOgvFrame(movie).generation == 2);
    uint64_t audioFrames = 0, nonzero = 0;
    double audioMs = 0;
    for (unsigned ms = 110; ms < 2000; ms += 10)
    {
        assert(cinematicOgvUpdate(movie, ms));
        while (feed && audioMs <= ms)
        {
            const char* data = nullptr;
            uint32_t frames = 0;
            feed(&data, &frames, feedUser);
            if (!frames) break;
            assert(data && frames * channels <= 2048);
            auto* pcm = reinterpret_cast<const int16_t*>(data);
            for (uint32_t i = 0; i < frames * channels; ++i) nonzero += pcm[i] != 0;
            audioFrames += frames;
            audioMs += double(frames) * 1000 / sampleRate;
        }
    }
    assert(!cinematicOgvUpdate(movie, 2000));
    assert(cinematicOgvFrame(movie).generation == 20);
    if (audio)
    {
        assert(feed && audioFrames > uint64_t(sampleRate) * 19 / 10 && nonzero > audioFrames);
        assert(callbackToken);
    }
    cinematicOgvClose(movie);
    assert(!feed);
}

static void testSilentStreaming()
{
    bytesRead = 0;
    auto* movie = cinematicOgvOpen(path("long.ogv").c_str());
    assert(movie && bytesRead < 256 * 1024);
    FILE* source = fopen(path("long.ogv").c_str(), "rb");
    assert(source && !fseek(source, 0, SEEK_END));
    uint64_t const length = ftell(source);
    fclose(source);
    // At 60 render updates/sec a silent movie may not consume a new half
    // second of audio per update, exhausting the file in about one second.
    for (unsigned ms = 0; ms < 1000; ms += 16) assert(cinematicOgvUpdate(movie, ms));
    assert(bytesRead < length / 2 && !feed);
    for (unsigned ms = 1000; ms < 30000; ms += 100) assert(cinematicOgvUpdate(movie, ms));
    assert(!cinematicOgvUpdate(movie, 30000));
    cinematicOgvClose(movie);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    directory = argv[1];
    assert(!cinematicOgvOpen(nullptr));
    assert(!cinematicOgvOpen(path("missing.ogv").c_str()));
    assert(!cinematicOgvOpen(path("invalid.ogv").c_str()));
    assert(!cinematicOgvOpen(path("truncated-header.ogv").c_str()));
    testVideo("420.ogv", true);
    testVideo("422.ogv", false);
    testVideo("444.ogv", false);
    auto* ended = cinematicOgvOpen(path("420.ogv").c_str());
    assert(ended && cinematicOgvUpdate(ended, 0) && feed && callbackToken);
    // Model natural EOF/FX_StopAllSounds: Blood writes zero through the
    // callback pointer. The mixer can immediately recycle this numeric handle.
    *reinterpret_cast<int*>(callbackToken) = 0;
    feed = nullptr; feedUser = nullptr; callbackToken = 0;
    unrelatedVoice = true;
    cinematicOgvClose(ended); // Must not kill the recycled voice.
    unrelatedVoice = false;
    SoundToggle = 0;
    auto* silent = cinematicOgvOpen(path("420.ogv").c_str());
    assert(silent);
    for (unsigned ms = 0; ms < 2000; ms += 10) assert(cinematicOgvUpdate(silent, ms));
    assert(!cinematicOgvUpdate(silent, 2000) && !feed);
    cinematicOgvClose(silent);
    testSilentStreaming();
    SoundToggle = 1;
    mixerAvailable = false;
    testSilentStreaming();
    // Truncated video data can fail open or finish early, but may never loop.
    auto* truncated = cinematicOgvOpen(path("truncated-data.ogv").c_str());
    if (truncated)
    {
        bool playing = true;
        for (unsigned ms = 0; playing && ms <= 3000; ms += 10) playing = cinematicOgvUpdate(truncated, ms);
        assert(!playing);
        cinematicOgvClose(truncated);
    }
    for (FILE* file : files) assert(!file);
    assert(audioStarts == 2 && audioStops == 1);
    puts("OGV decoder: chroma 420/422/444, PCM audio, timing, EOF, truncation, and bounded streaming passed");
}
