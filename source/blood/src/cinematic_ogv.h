// Streaming Ogg Theora/Vorbis cinematics. The caller supplies a monotonic movie
// clock; all file I/O and codec work run on that calling thread.
#ifndef BLOOD_CINEMATIC_OGV_H
#define BLOOD_CINEMATIC_OGV_H

#include <cstdint>

struct CinematicOgv;

struct CinematicOgvFrame
{
    unsigned width = 0, height = 0;
    unsigned pixelAspectNumerator = 1, pixelAspectDenominator = 1;
    double fps = 0;
    uint64_t generation = 0;
    // Top-to-bottom, tightly packed RGBA; valid until the next update/close.
    const uint8_t* rgba = nullptr;
};

// Uses the Build virtual filesystem. Returns null for absent/invalid media.
// A successful open has already decoded the first frame, but starts no sound.
CinematicOgv* cinematicOgvOpen(const char* filename);
void cinematicOgvClose(CinematicOgv* movie);
const CinematicOgvFrame& cinematicOgvFrame(const CinematicOgv* movie);
bool cinematicOgvHasAudio(const CinematicOgv* movie);
// Returns false at the end (or for damaged media); the last frame stays valid.
bool cinematicOgvUpdate(CinematicOgv* movie, uint32_t elapsedMs);

#endif
