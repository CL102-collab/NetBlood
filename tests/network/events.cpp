// Production event codec/dispatch with an inert audio mixer and tiny world.
// SPDX-License-Identifier: GPL-2.0-only
#include "compat.h"
#include "build.h"
#include "mmulti.h"
#include "netevents.h"
#include "player.h"
#include "resource.h"
#include "sound.h"
#include "messages.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

spritetype sprite[MAXSPRITES];
PLAYER gPlayer[kMaxPlayers];
PLAYER *gMe = &gPlayer[0], *gView = &gPlayer[0];
int myconnectindex = 0;
Resource gSoundRes;
Resource::Resource() {}
Resource::~Resource() {}
static unsigned resourceId;
static DICTNODE resource;
static SFX effect;
DICTNODE *Resource::Lookup(unsigned id, const char *) { resourceId = id; return &resource; }
void *Resource::Load(DICTNODE *) { effect.loopStart = resourceId == 200 ? 0 : -1; return &effect; }

static bool serverTick = false, suppress = false;
static uint32_t generations[MAXSPRITES];
static unsigned positional, attached, samples, kills, texts, tests;
static std::string lastText;
static std::vector<std::vector<uint8_t>> sent;
bool netInServerTick() { return serverTick; }
uint32_t netAppliedCommandSequence(int) { return 55; }
uint32_t netSnapshotSpriteGeneration(int n) { return generations[n]; }
void netBroadcastEvent(const uint8_t *p, size_t n) { sent.emplace_back(p, p + n); }
bool netPredictSuppressSound(int, int, uint32_t, int) { return suppress; }
void sfxPlay3DSound(int, int, int, int, int) { ++positional; }
void sfxPlay3DSound(spritetype *, int, int, int) { ++attached; }
void sfxPlay3DSoundCP(spritetype *, int, int, int, int, int) { ++attached; }
void sfxKill3DSound(spritetype *, int, int) { ++kills; }
void sfxKillSpriteSounds(spritetype *) { ++kills; }
void sfxKillAllSounds() { ++kills; }
void sndStartSample(unsigned, int, int, bool) { ++samples; }
void sndStartSample(const char *, int, int) { ++samples; }
void sndStartWavID(unsigned, int, int) { ++samples; }
void sndKillAllSounds() { ++kills; }
void viewSetMessage(const char *text, int palette, MESSAGE_PRIORITY priority)
{
    netMessageBroadcast(text, palette, int(priority));
    ++texts; lastText = text;
}

static void check(bool condition, const char *message)
{
    ++tests;
    if (!condition) { std::fprintf(stderr, "event test failed: %s\n", message); std::exit(1); }
}
static void put(std::vector<uint8_t> &v, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(value >> (i * 8));
}
static bool play(const std::vector<uint8_t> &v) { return netPlayEvent(v.data(), v.size()); }
static void reset()
{
    netEventsReset(); sent.clear(); positional = attached = samples = kills = texts = 0;
    suppress = false; myconnectindex = 0; serverTick = false;
    for (auto &p : gPlayer) p.nSprite = -1;
    sprite[4].index = 4; sprite[4].type = 201; sprite[4].picnum = 100;
    sprite[4].statnum = 6; sprite[4].extra = 1; sprite[4].sectnum = 0;
    generations[4] = 10; gPlayer[0].nSprite = 4;
}

int main()
{
    reset();
    netSoundPosition(10, 20, 30, 100, 0);
    check(sent.empty(), "menu/offline sound does not emit a server event");
    serverTick = true;
    netSoundPosition(10, 20, 30, 100, 0);
    check(sent.size() == 1, "server sound emits independently of audio mixer");
    auto position = sent.back(); serverTick = false;
    check(play(position) && positional == 1 && sent.size() == 1, "validated positional event plays without echo");
    for (size_t n = 0; n < position.size(); ++n)
        check(!netPlayEvent(position.data(), n), "every truncated payload is rejected");
    check(positional == 1, "truncation never dispatches audio");
    for (auto field : {std::pair<size_t, uint32_t>(4, 0xffffffffu), {8, 256}, {12, 16}, {16, 0x1000001},
                       {20, 256}, {36, uint32_t(kMaxSectors)}, {40, uint32_t(kMaxSprites)},
                       {48, uint32_t(MAXTILES)}, {52, uint32_t(kMaxXSprites)}, {64, 8}})
    {
        auto bad = position; put(bad, field.first, field.second);
        check(!play(bad), "out-of-range field is rejected before dispatch");
    }
    for (auto field : {std::pair<size_t, uint8_t>(0, 2), {1, 0}, {1, 11}, {2, 2}, {3, 1}})
    {
        auto bad = position; bad[field.first] = field.second;
        check(!play(bad), "unknown version/type/flags rejected");
    }
    auto trailing = position; trailing.push_back(0);
    check(!play(trailing), "trailing data rejected");

    serverTick = true; netSoundSprite(NetSoundKind::Sprite, 4, 100, 2);
    auto actor = sent.back(); serverTick = false;
    check(play(actor) && attached == 1, "current generation attaches to actor");
    sprite[4].picnum++;
    check(play(actor) && attached == 2, "ordinary sprite animation does not break sound attachment");
    generations[4]++;
    check(play(actor) && attached == 2 && positional == 2, "reused actor slot receives only positional fallback");
    generations[4] = 10; suppress = true;
    check(play(actor) && attached == 2 && positional == 2, "predicted one-shot echo is suppressed");
    suppress = false;

    reset(); serverTick = true;
    netSoundSprite(NetSoundKind::Sprite, 4, 200, 2);
    auto loop = sent.back();
    std::vector<std::vector<uint8_t>> baseline;
    netEventsBaseline(baseline);
    check(baseline.size() == 1, "dedicated loop state retained without mixer playback");
    netEventsReset(); serverTick = false; generations[4] = 9;
    check(play(loop) && attached == 0 && positional == 0, "loop arriving before actor snapshot is deferred");
    netEventsBaseline(baseline); check(baseline.size() == 1, "mid-record retains loops awaiting a future actor snapshot");
    generations[4] = 10; netEventsOnSnapshot();
    check(attached == 1, "matching snapshot starts deferred loop once");
    netEventsOnSnapshot(); check(attached == 1, "later snapshots do not restart loop");
    netEventsBaseline(baseline); check(baseline.size() == 1, "client loop cache supports mid-session demo recording");
    serverTick = true; netSoundSprite(NetSoundKind::KillSpriteSound, 4, 200, 2);
    auto stop = sent.back(); serverTick = false;
    check(play(stop) && kills == 1, "authoritative channel stop dispatches");
    netEventsBaseline(baseline); check(baseline.empty(), "stopped loop omitted from later hot-join baseline");

    reset(); serverTick = true;
    netSoundForPlayer(1, 3062, 255, 2);
    check(samples == 0 && sent.size() == 1, "listen server does not hear another player's private sound");
    auto personal = sent.back(); serverTick = false;
    check(play(personal) && samples == 0, "private event is ignored by other clients");
    myconnectindex = 1;
    check(play(personal) && samples == 1, "private event plays for intended client");
    myconnectindex = 0; netSoundForPlayer(0, 3063, 255, 2);
    check(samples == 2 && sent.size() == 1, "offline/local targeted sound preserves original playback");
    netEventsReset(); netEventsBaseline(baseline);
    check(baseline.empty(), "map reset clears event baseline");

    reset(); serverTick = true;
    netMessageForPlayer(1, "That requires a key.", 8, MESSAGE_PRIORITY_NORMAL);
    check(texts == 0 && sent.size() == 1, "private HUD message skips listen server display");
    auto message = sent.back(); serverTick = false;
    check(play(message) && texts == 0, "private HUD message skips other recipients");
    myconnectindex = 1;
    check(play(message) && texts == 1 && lastText == "That requires a key.", "intended client receives exact authoritative HUD text");
    check(sent.size() == 1, "replayed message does not echo through view hook");
    auto invalidPalette = message; put(invalidPalette, 16, 256);
    auto invalidPriority = message; put(invalidPriority, 20, 101);
    check(!play(invalidPalette) && !play(invalidPriority), "HUD palette and priority bounded before drawing");
    serverTick = true; viewSetMessage("Blue captured the flag", 0, MESSAGE_PRIORITY_NORMAL);
    check(sent.size() == 2, "global server HUD event is broadcast once");
    std::printf("PASS: authoritative presentation validation, targeting, no echo, sprite reuse, prediction suppression, loop hot-join, HUD messages (%u checks)\n", tests);
}
