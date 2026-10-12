// Native, bounded-memory Ogg Theora/Vorbis cinematic playback.
#include "cinematic_ogv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "compat.h"
#include "vfs.h"
#include "fx_man.h"
#include "../../libtheora/include/ogg/ogg.h"
#include "../../libtheora/include/theora/theoradec.h"

extern int32_t SoundToggle;
extern int32_t FXVolume;

namespace
{
constexpr long kReadBytes = 16384;
constexpr long kMaxQueuedBytes = 4 * 1024 * 1024;
constexpr unsigned kMaxDimension = 4096;
constexpr uint64_t kMaxPixels = 4096ull * 2160;
constexpr size_t kAudioBlockSamples = 2048;

uint8_t colorByte(int value)
{
    return static_cast<uint8_t>(std::max(0, std::min(value, 255)));
}
}

struct CinematicOgv
{
    buildvfs_kfd file = buildvfs_kfd_invalid;
    ogg_sync_state sync{};
    ogg_stream_state videoStream{}, audioStream{};
    bool haveVideo = false, haveAudio = false, bosDone = false;
    bool eof = false, failed = false, videoEnded = false, started = false;
    int videoHeaders = 0, audioHeaders = 0;
    uint64_t bytesRead = 0, frameNumber = 0;
    uint32_t previousMs = 0;
    th_info videoInfo{};
    th_comment videoComment{};
    th_setup_info* setup = nullptr;
    th_dec_ctx* decoder = nullptr;
    vorbis_info audioInfo{};
    vorbis_comment audioComment{};
    vorbis_dsp_state audioDsp{};
    vorbis_block audioBlock{};
    bool audioInitialized = false;
    int audioChannels = 0, audioVoice = 0;
    std::vector<int16_t> audioRing;
    // Single producer (update) / single consumer (the mixer callback). Mixer
    // copies into its own buffer before releasing ring storage to the producer.
    std::atomic<uint64_t> audioRead{0}, audioWrite{0};
    std::atomic<bool> audioEnded{false};
    int16_t audioOutput[kAudioBlockSamples]{};
    std::vector<uint8_t> pixels;
    CinematicOgvFrame frame;

    CinematicOgv()
    {
        ogg_sync_init(&sync);
        th_info_init(&videoInfo);
        th_comment_init(&videoComment);
        vorbis_info_init(&audioInfo);
        vorbis_comment_init(&audioComment);
    }

    ~CinematicOgv()
    {
        // MV_Kill takes the mixer lock: the callback cannot outlive this object.
        if (audioVoice > 0)
            FX_StopSound(audioVoice, false);
        if (audioInitialized)
        {
            vorbis_block_clear(&audioBlock);
            vorbis_dsp_clear(&audioDsp);
        }
        vorbis_comment_clear(&audioComment);
        vorbis_info_clear(&audioInfo);
        if (decoder) th_decode_free(decoder);
        if (setup) th_setup_free(setup);
        th_comment_clear(&videoComment);
        th_info_clear(&videoInfo);
        if (haveVideo) ogg_stream_clear(&videoStream);
        if (haveAudio) ogg_stream_clear(&audioStream);
        ogg_sync_clear(&sync);
        if (file != buildvfs_kfd_invalid) kclose(file);
    }

    bool readPage()
    {
        ogg_page page{};
        for (;;)
        {
            int const result = ogg_sync_pageout(&sync, &page);
            if (result > 0) break;
            if (result < 0) { failed = true; return false; }
            if (eof) return false;
            char* buffer = ogg_sync_buffer(&sync, kReadBytes);
            if (!buffer) { failed = true; return false; }
            auto const count = kread(file, buffer, kReadBytes);
            if (count < 0) { failed = true; return false; }
            if (!count) eof = true;
            bytesRead += count;
            ogg_sync_wrote(&sync, count);
        }
        int const serial = ogg_page_serialno(&page);
        if (!bosDone && ogg_page_bos(&page))
        {
            ogg_stream_state stream{};
            ogg_packet packet{};
            if (ogg_stream_init(&stream, serial)) { failed = true; return false; }
            ogg_stream_pagein(&stream, &page);
            if (ogg_stream_packetout(&stream, &packet) != 1)
            {
                ogg_stream_clear(&stream);
                failed = true;
                return false;
            }
            if (!haveVideo && packet.bytes >= 7 && packet.packet[0] == 0x80 &&
                !memcmp(packet.packet + 1, "theora", 6))
            {
                haveVideo = true;
                videoStream = stream;
                if (th_decode_headerin(&videoInfo, &videoComment, &setup, &packet) <= 0)
                    failed = true;
                videoHeaders = 1;
            }
            else if (!haveAudio && packet.bytes >= 7 && packet.packet[0] == 1 &&
                     !memcmp(packet.packet + 1, "vorbis", 6))
            {
                haveAudio = true;
                audioStream = stream;
                if (vorbis_synthesis_headerin(&audioInfo, &audioComment, &packet)) failed = true;
                audioHeaders = 1;
            }
            else ogg_stream_clear(&stream); // e.g. an Ogg Skeleton stream
        }
        else
        {
            bosDone = true;
            ogg_stream_state* stream = haveVideo && serial == videoStream.serialno ? &videoStream :
                                      haveAudio && serial == audioStream.serialno ? &audioStream : nullptr;
            if (stream)
            {
                // Pathological interleaving or an unbounded packet must not
                // turn a short cinematic into unbounded memory consumption.
                if (stream->body_fill - stream->body_returned + page.body_len > kMaxQueuedBytes ||
                    stream->lacing_fill - stream->lacing_returned > 65536 ||
                    ogg_stream_pagein(stream, &page))
                    failed = true;
            }
        }
        return !failed;
    }

    bool headers()
    {
        while (!failed)
        {
            ogg_packet packet{};
            while (haveVideo && videoHeaders < 3)
            {
                int const result = ogg_stream_packetout(&videoStream, &packet);
                if (!result) break;
                if (result < 0 || th_decode_headerin(&videoInfo, &videoComment, &setup, &packet) <= 0)
                    return false;
                ++videoHeaders;
            }
            while (haveAudio && audioHeaders < 3)
            {
                int const result = ogg_stream_packetout(&audioStream, &packet);
                if (!result) break;
                if (result < 0 || vorbis_synthesis_headerin(&audioInfo, &audioComment, &packet))
                    return false;
                ++audioHeaders;
            }
            if (bosDone && videoHeaders == 3 && (!haveAudio || audioHeaders == 3)) return true;
            if (bytesRead > static_cast<uint64_t>(kMaxQueuedBytes) || !readPage()) return false;
        }
        return false;
    }

    uint64_t audioBuffered() const
    {
        return audioWrite.load(std::memory_order_acquire) - audioRead.load(std::memory_order_acquire);
    }

    void discardSilentAudio()
    {
        if (!started || audioVoice > 0) return;
        // A muted/unavailable device consumes according to the movie clock,
        // not once per rendered frame. Otherwise priming would read farther
        // ahead on every update and eventually overflow the video packet queue.
        uint64_t const due = uint64_t(previousMs) * audioInfo.rate / 1000 * audioChannels;
        uint64_t const write = audioWrite.load(std::memory_order_acquire);
        uint64_t const read = audioRead.load(std::memory_order_relaxed);
        audioRead.store(std::max(read, std::min(due, write)), std::memory_order_release);
    }

    void decodeAudio()
    {
        if (!audioInitialized) return;
        for (;;)
        {
            discardSilentAudio();
            float** pcm = nullptr;
            int samples = vorbis_synthesis_pcmout(&audioDsp, &pcm);
            if (samples > 0)
            {
                uint64_t const write = audioWrite.load(std::memory_order_relaxed);
                uint64_t const read = audioRead.load(std::memory_order_acquire);
                samples = std::min(samples, static_cast<int>((audioRing.size() - (write - read)) / audioChannels));
                if (!samples) return;
                for (int i = 0; i < samples; ++i)
                    for (int channel = 0; channel < audioChannels; ++channel)
                    {
                        // Cinematics normally carry mono/stereo. For additional
                        // channels include all speakers at a bounded gain.
                        float value = 0;
                        int count = 0;
                        for (int source = channel; source < audioInfo.channels; source += audioChannels)
                        {
                            value += pcm[source][i];
                            ++count;
                        }
                        value = std::max(-1.f, std::min(value / count, 1.f));
                        audioRing[(write + uint64_t(i) * audioChannels + channel) % audioRing.size()] =
                            static_cast<int16_t>(value * 32767.f);
                    }
                audioWrite.store(write + uint64_t(samples) * audioChannels, std::memory_order_release);
                vorbis_synthesis_read(&audioDsp, samples);
            }
            else
            {
                ogg_packet packet{};
                int const result = ogg_stream_packetout(&audioStream, &packet);
                if (!result)
                {
                    if (eof || ogg_stream_eos(&audioStream)) audioEnded.store(true, std::memory_order_release);
                    return;
                }
                if (result < 0 || vorbis_synthesis(&audioBlock, &packet) || vorbis_synthesis_blockin(&audioDsp, &audioBlock))
                {
                    failed = true;
                    audioEnded.store(true, std::memory_order_release);
                    return;
                }
            }
        }
    }

    static void audioFeed(const char** data, uint32_t* length, void* user)
    {
        auto& movie = *static_cast<CinematicOgv*>(user);
        uint64_t const read = movie.audioRead.load(std::memory_order_relaxed);
        uint64_t const write = movie.audioWrite.load(std::memory_order_acquire);
        size_t const count = std::min<uint64_t>(write - read, kAudioBlockSamples);
        *data = reinterpret_cast<const char*>(movie.audioOutput);
        if (!count)
        {
            // A brief main-thread stall must not destroy the mixer voice.
            memset(movie.audioOutput, 0, sizeof(movie.audioOutput));
            *length = movie.audioEnded.load(std::memory_order_acquire) ? 0 : kAudioBlockSamples / movie.audioChannels;
            return;
        }
        for (size_t i = 0; i < count; ++i)
            movie.audioOutput[i] = movie.audioRing[(read + i) % movie.audioRing.size()];
        movie.audioRead.store(read + count, std::memory_order_release);
        // Demand-feed lengths count sample frames, not bytes or the individual
        // interleaved channel samples. Blocks stay below MV's 0x8000 split size.
        *length = static_cast<uint32_t>(count / movie.audioChannels);
    }

    void convertFrame()
    {
        th_ycbcr_buffer planes;
        th_decode_ycbcr_out(decoder, planes);
        unsigned const shiftX = videoInfo.pixel_fmt == TH_PF_444 ? 0 : 1;
        unsigned const shiftY = videoInfo.pixel_fmt == TH_PF_420 ? 1 : 0;
        for (unsigned y = 0; y < frame.height; ++y)
        {
            unsigned const row = videoInfo.pic_y + y;
            auto const* yy = planes[0].data + ptrdiff_t(row) * planes[0].stride + videoInfo.pic_x;
            auto const* cb = planes[1].data + ptrdiff_t(row >> shiftY) * planes[1].stride;
            auto const* cr = planes[2].data + ptrdiff_t(row >> shiftY) * planes[2].stride;
            uint8_t* dest = pixels.data() + size_t(y) * frame.width * 4;
            for (unsigned x = 0; x < frame.width; ++x)
            {
                unsigned const chroma = (videoInfo.pic_x + x) >> shiftX;
                int const c = int(yy[x]) - 16, d = int(cb[chroma]) - 128, e = int(cr[chroma]) - 128;
                dest[4*x + 0] = colorByte((298*c + 409*e + 128) >> 8);
                dest[4*x + 1] = colorByte((298*c - 100*d - 208*e + 128) >> 8);
                dest[4*x + 2] = colorByte((298*c + 516*d + 128) >> 8);
                dest[4*x + 3] = 255;
            }
        }
        ++frame.generation;
    }

    bool decodeFrame()
    {
        while (!failed)
        {
            ogg_packet packet{};
            int const result = ogg_stream_packetout(&videoStream, &packet);
            if (result < 0) { failed = true; return false; }
            if (result > 0)
            {
                int const decoded = th_decode_packetin(decoder, &packet, nullptr);
                if (decoded < 0) { failed = true; return false; }
                if (decoded != TH_DUPFRAME || !frame.generation) convertFrame();
                ++frameNumber;
                return true;
            }
            if (ogg_stream_eos(&videoStream) || !readPage())
            {
                videoEnded = true;
                return false;
            }
            decodeAudio();
        }
        return false;
    }

    void primeAudio()
    {
        if (!audioInitialized) return;
        uint64_t const target = uint64_t(audioInfo.rate) * audioChannels / 2;
        decodeAudio();
        // Keep about half a second ahead of the mixer. Pages for future video
        // frames remain compressed, and are subject to the queue size limit.
        while (!failed && !audioEnded.load(std::memory_order_acquire) && audioBuffered() < target && readPage())
            decodeAudio();
        decodeAudio();
    }
};

CinematicOgv* cinematicOgvOpen(const char* filename)
{
    if (!filename || !*filename) return nullptr;
    auto* movie = new CinematicOgv;
    movie->file = kopen4loadfrommod(filename, 0);
    auto fail = [movie]() -> CinematicOgv* { delete movie; return nullptr; };
    if (movie->file == buildvfs_kfd_invalid || !movie->headers()) return fail();
    auto const& info = movie->videoInfo;
    if (!info.frame_width || !info.frame_height || info.frame_width > kMaxDimension ||
        info.frame_height > kMaxDimension || uint64_t(info.frame_width) * info.frame_height > kMaxPixels ||
        !info.pic_width || !info.pic_height || info.pic_width > info.frame_width || info.pic_height > info.frame_height ||
        info.pic_x > info.frame_width - info.pic_width || info.pic_y > info.frame_height - info.pic_height ||
        !info.fps_numerator || !info.fps_denominator ||
        info.fps_numerator > uint64_t(info.fps_denominator) * 240 ||
        (info.pixel_fmt != TH_PF_420 && info.pixel_fmt != TH_PF_422 && info.pixel_fmt != TH_PF_444)) return fail();
    movie->decoder = th_decode_alloc(&info, movie->setup);
    if (!movie->decoder) return fail();
    th_setup_free(movie->setup);
    movie->setup = nullptr;
    movie->frame.width = info.pic_width;
    movie->frame.height = info.pic_height;
    movie->frame.fps = double(info.fps_numerator) / info.fps_denominator;
    if (info.aspect_numerator && info.aspect_denominator)
    {
        movie->frame.pixelAspectNumerator = info.aspect_numerator;
        movie->frame.pixelAspectDenominator = info.aspect_denominator;
    }
    movie->pixels.resize(size_t(info.pic_width) * info.pic_height * 4);
    movie->frame.rgba = movie->pixels.data();
    if (movie->haveAudio)
    {
        if (movie->audioInfo.channels < 1 || movie->audioInfo.channels > 8 ||
            movie->audioInfo.rate < 8000 || movie->audioInfo.rate > 192000 ||
            vorbis_synthesis_init(&movie->audioDsp, &movie->audioInfo)) return fail();
        movie->audioInitialized = true;
        vorbis_block_init(&movie->audioDsp, &movie->audioBlock);
        movie->audioChannels = std::min(movie->audioInfo.channels, 2);
        movie->audioRing.resize(size_t(movie->audioInfo.rate) * movie->audioChannels * 2);
    }
    if (!movie->decodeFrame()) return fail();
    movie->primeAudio();
    if (movie->failed) return fail();
    return movie;
}

void cinematicOgvClose(CinematicOgv* movie) { delete movie; }

const CinematicOgvFrame& cinematicOgvFrame(const CinematicOgv* movie) { return movie->frame; }

bool cinematicOgvHasAudio(const CinematicOgv* movie) { return movie && movie->haveAudio; }

bool cinematicOgvUpdate(CinematicOgv* movie, uint32_t elapsedMs)
{
    if (!movie || movie->failed) return false;
    // Protect against a caller's clock moving backwards.
    elapsedMs = std::max(elapsedMs, movie->previousMs);
    movie->previousMs = elapsedMs;
    if (!movie->started)
    {
        movie->started = true;
        if (movie->audioInitialized && SoundToggle)
            movie->audioVoice = FX_StartDemandFeedPlayback(CinematicOgv::audioFeed, 16, movie->audioChannels,
                movie->audioInfo.rate, 0, FXVolume, FXVolume, FXVolume, FX_MUSIC_PRIORITY, fix16_one,
                reinterpret_cast<intptr_t>(&movie->audioVoice), movie);
    }
    // The first decoded frame occupies [0, 1/fps); the final frame gets the
    // same display duration instead of disappearing as soon as it is decoded.
    uint64_t const due = uint64_t(elapsedMs) * movie->videoInfo.fps_numerator /
                         (uint64_t(1000) * movie->videoInfo.fps_denominator);
    while (!movie->videoEnded && movie->frameNumber <= due)
        if (!movie->decodeFrame()) break;
    movie->primeAudio();
    return !movie->failed && (!movie->videoEnded || movie->frameNumber > due);
}
