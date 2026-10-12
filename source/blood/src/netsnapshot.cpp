// SPDX-License-Identifier: GPL-2.0-or-later
#include "netsnapshot.h"
#include "common_game.h"
#include "actor.h"
#include "ai.h"
#include "aihand.h"
#include "blood.h"
#include "db.h"
#include "dude.h"
#include "endgame.h"
#include "gameutil.h"
#include "globals.h"
#include "levels.h"
#include "player.h"
#include "mmulti.h"
#include "screen.h"
#include "sectorfx.h"
#include "tile.h"
#include "view.h"
#include "warp.h"
#ifdef NOONE_EXTENSIONS
#include "nnexts.h"
#include "nnextslaser.h"
// Existing extension-owned registry. The wire schema below includes only its
// rendering state; scans, callbacks, damage and triggers remain server-owned.
extern LASER* gLaser[2048];
extern uint16_t gLasersCount;
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

extern QAV* weaponQAV[126];

namespace
{
constexpr uint32_t kStateMagic = 0x3153434e; // NCS1
uint32_t spriteGeneration[kMaxSprites] = {};
AISTATE visualAiState[kMaxXSprites] = {};

// Every numeric field has an explicit signedness/range and is encoded as one
// little-endian 32-bit value. Strings are length-prefixed UTF-8/legacy bytes.
// Native struct padding, pointers, and bitfield layout never enter the stream.
class Archive
{
public:
    bool reading, ok = true;
    size_t position = 0;
    const std::vector<uint8_t>* input;
    std::vector<uint8_t>* output;
    std::string& error;

    Archive(const std::vector<uint8_t>& bytes, std::string& err)
        : reading(true), input(&bytes), output(nullptr), error(err) {}
    Archive(std::vector<uint8_t>& bytes, std::string& err)
        : reading(false), input(nullptr), output(&bytes), error(err) {}

    void fail(const char* message)
    {
        if (ok) error = message;
        ok = false;
    }

    int64_t number(int64_t value, int64_t low, int64_t high)
    {
        if (!ok) return 0;
        if (reading)
        {
            if (input->size() - position < 4) { fail("truncated snapshot field"); return 0; }
            uint32_t bits = 0;
            for (unsigned i = 0; i != 4; ++i) bits |= uint32_t((*input)[position++]) << (8*i);
            value = low < 0 ? int64_t(int32_t(bits)) : int64_t(bits);
        }
        if (value < low || value > high) { fail("snapshot field outside its defined range"); return 0; }
        if (!reading)
        {
            if (output->size() > kNetSnapshotMaxBytes - 4) { fail("snapshot exceeds maximum state size"); return 0; }
            uint32_t const bits = uint32_t(value);
            for (unsigned i = 0; i != 4; ++i) output->push_back(uint8_t(bits >> (8*i)));
        }
        return value;
    }

    template<size_t N> void string(char (&value)[N])
    {
        size_t count = 0;
        if (!reading)
        {
            while (count < N && value[count]) ++count;
            if (count == N) { fail("unterminated snapshot string"); return; }
        }
        count = size_t(number(count, 0, N-1));
        if (!ok) return;
        if (reading)
        {
            if (input->size() - position < count) { fail("truncated snapshot string"); return; }
            memset(value, 0, N);
            for (size_t i = 0; i != count; ++i)
            {
                value[i] = char((*input)[position++]);
                if (!value[i]) fail("embedded zero in snapshot string");
            }
        }
        else
        {
            if (output->size() > kNetSnapshotMaxBytes-count) { fail("snapshot exceeds maximum state size"); return; }
            output->insert(output->end(), value, value+count);
        }
    }

    float real(float value)
    {
        uint32_t bits = 0;
        static_assert(sizeof(float) == sizeof(bits) && std::numeric_limits<float>::is_iec559,
                      "network float fields require IEEE-754 binary32");
        memcpy(&bits, &value, sizeof(bits));
        bits = uint32_t(number(bits, 0, UINT32_MAX));
        memcpy(&value, &bits, sizeof(bits));
        if (!std::isfinite(value) || value < -255.f || value > 1.f) fail("invalid snapshot sprite alpha");
        return value;
    }
};

#define F(name, low, high) do { bool const wasOK = io.ok; \
    v.name = static_cast<std::remove_reference<decltype(v.name)>::type>(io.number(v.name, low, high)); \
    if (wasOK && !io.ok) io.error += " (" #name ")"; } while (0);
#define I(name) F(name, INT32_MIN, INT32_MAX)
#define U(name) F(name, 0, UINT32_MAX)
#define B(name) F(name, 0, 1)

void sectorFields(Archive& io, usectortype& v)
{
    F(wallptr, 0, kMaxWalls-1)
    F(wallnum, 0, kMaxWalls)
    F(ceilingz, INT32_MIN, INT32_MAX)
    F(floorz, INT32_MIN, INT32_MAX)
    F(ceilingstat, 0, 65535)
    F(floorstat, 0, 65535)
    F(ceilingpicnum, 0, kMaxTiles-1)
    F(ceilingheinum, -32768, 32767)
    F(ceilingshade, -128, 127)
    F(ceilingpal, 0, 255)
    F(ceilingxpanning, 0, 255)
    F(ceilingypanning, 0, 255)
    F(floorpicnum, 0, kMaxTiles-1)
    F(floorheinum, -32768, 32767)
    F(floorshade, -128, 127)
    F(floorpal, 0, 255)
    F(floorxpanning, 0, 255)
    F(floorypanning, 0, 255)
    F(visibility, 0, 255)
    F(fogpal, 0, 255)
    F(lotag, -32768, 32767)
    F(hitag, -32768, 32767)
    F(extra, -1, kMaxXSectors-1)
#ifdef NEW_MAP_FORMAT
    F(ceilingbunch, -1, YAX_MAXBUNCHES-1) F(floorbunch, -1, YAX_MAXBUNCHES-1)
#endif
}

void wallFields(Archive& io, uwalltype& v)
{
    F(x, INT32_MIN, INT32_MAX)
    F(y, INT32_MIN, INT32_MAX)
    F(point2, 0, kMaxWalls-1)
    F(nextwall, -1, kMaxWalls-1)
    F(nextsector, -1, kMaxSectors-1)
    F(cstat, 0, 65535)
    F(picnum, 0, kMaxTiles-1)
    F(overpicnum, -1, kMaxTiles-1)
    F(shade, -128, 127)
    F(pal, 0, 255)
    F(xrepeat, 0, 255)
    F(yrepeat, 0, 255)
    F(xpanning, 0, 255)
    F(ypanning, 0, 255)
    F(lotag, -32768, 32767)
    F(hitag, -32768, 32767)
    F(extra, -1, kMaxXWalls-1)
#ifdef NEW_MAP_FORMAT
    F(upwall, -1, kMaxWalls-1) F(dnwall, -1, kMaxWalls-1)
#endif
}

void spriteFields(Archive& io, uspritetype& v)
{
    F(x, INT32_MIN, INT32_MAX)
    F(y, INT32_MIN, INT32_MAX)
    F(z, INT32_MIN, INT32_MAX)
    F(cstat, 0, 65535)
    F(picnum, 0, kMaxTiles-1)
    F(shade, -128, 127)
    F(pal, 0, 255)
    F(clipdist, 0, 255)
    F(blend, 0, 255)
    F(xrepeat, 0, 255)
    F(yrepeat, 0, 255)
    F(xoffset, -128, 127)
    F(yoffset, -128, 127)
    F(sectnum, 0, kMaxSectors-1)
    F(statnum, 0, kMaxStatus-1)
    F(ang, -32768, 32767)
    F(owner, -32768, 32767)
    F(index, 0, kMaxSprites-1)
    F(yvel, -32768, 32767)
    F(inittype, -32768, 32767)
    F(type, -32768, 32767)
    F(flags, -32768, 32767)
    F(extra, -1, kMaxXSprites-1)
}

void xspriteFields(Archive& io, XSPRITE& v)
{
    F(unused1, 0, 3LL)
    F(unused2, 0, 1LL)
    F(unused3, 0, 3LL)
    F(unused4, 0, 63LL)
    F(reference, -16384, 16383LL)
    F(state, 0, 1LL)
    F(busy, 0, 131071LL)
    F(txID, 0, 1023LL)
    F(rxID, 0, 1023LL)
    F(command, 0, 255LL)
    F(triggerOn, 0, 1LL)
    F(triggerOff, 0, 1LL)
    F(busyTime, 0, 4095LL)
    F(waitTime, 0, 4095LL)
    F(restState, 0, 1LL)
    F(Interrutable, 0, 1LL)
    F(respawnPending, 0, 3LL)
    F(dropMsg, 0, 255LL)
    F(Decoupled, 0, 1LL)
    F(triggerOnce, 0, 1LL)
    F(isTriggered, 0, 1LL)
    F(key, 0, 7LL)
    F(wave, 0, 3LL)
    F(Push, 0, 1LL)
    F(Vector, 0, 1LL)
    F(Impact, 0, 1LL)
    F(Pickup, 0, 1LL)
    F(Touch, 0, 1LL)
    F(Sight, 0, 1LL)
    F(Proximity, 0, 1LL)
    F(lSkill, 0, 31LL)
    F(lS, 0, 1LL)
    F(lB, 0, 1LL)
    F(lT, 0, 1LL)
    F(lC, 0, 1LL)
    F(DudeLockout, 0, 1LL)
    F(data1, -32768, 32767LL)
    F(data2, -32768, 32767LL)
    F(data3, -32768, 32767LL)
    F(data4, 0, 65535LL)
    F(locked, 0, 1LL)
    F(medium, 0, 3LL)
    F(respawn, 0, 3LL)
    F(lockMsg, 0, 255LL)
    F(health, 0, 1048575LL)
    F(dudeDeaf, 0, 1LL)
    F(dudeAmbush, 0, 1LL)
    F(dudeGuard, 0, 1LL)
    F(dudeFlag4, 0, 1LL)
    F(target, -32768, 32767LL)
    F(targetX, -2147483648, 2147483647LL)
    F(targetY, -2147483648, 2147483647LL)
    F(targetZ, -2147483648, 2147483647LL)
    F(goalAng, 0, 2047LL)
    F(dodgeDir, -2, 1LL)
    F(burnTime, 0, 65535LL)
    F(burnSource, -32768, 32767LL)
    F(height, 0, 65535LL)
    F(stateTimer, 0, 65535LL)
#ifdef NOONE_EXTENSIONS
    F(sysData1, -2147483648, 2147483647LL)
    F(sysData2, -2147483648, 2147483647LL)
    F(sysData3, -2147483648, 2147483647LL)
    F(sysData4, -2147483648, 2147483647LL)
    F(physAttr, 0, 4294967295LL)
#endif
    I(scale)
}

void xsectorFields(Archive& io, XSECTOR& v)
{
    F(reference, -8192, 8191LL)
    F(state, 0, 1LL)
    F(busy, 0, 131071LL)
    F(data, 0, 65535LL)
    F(txID, 0, 1023LL)
    F(rxID, 0, 1023LL)
    F(busyWaveA, 0, 7LL)
    F(busyWaveB, 0, 7LL)
    F(command, 0, 255LL)
    F(triggerOn, 0, 1LL)
    F(triggerOff, 0, 1LL)
    F(busyTimeA, 0, 4095LL)
    F(waitTimeA, 0, 4095LL)
    F(restState, 0, 1LL)
    F(interruptable, 0, 1LL)
    F(reTriggerA, 0, 1LL)
    F(reTriggerB, 0, 1LL)
    F(amplitude, -128, 127LL)
    F(freq, 0, 255LL)
    F(phase, 0, 255LL)
    F(wave, 0, 15LL)
    F(shadeAlways, 0, 1LL)
    F(shadeFloor, 0, 1LL)
    F(shadeCeiling, 0, 1LL)
    F(shadeWalls, 0, 1LL)
    F(shade, -128, 127LL)
    F(panAlways, 0, 1LL)
    F(panFloor, 0, 1LL)
    F(panCeiling, 0, 1LL)
    F(Drag, 0, 1LL)
    F(panVel, 0, 255LL)
    F(panAngle, 0, 2047LL)
    F(Underwater, 0, 1LL)
    F(Depth, 0, 7LL)
    F(unused1, 0, 1LL)
    F(decoupled, 0, 1LL)
    F(triggerOnce, 0, 1LL)
    F(isTriggered, 0, 1LL)
    F(Key, 0, 7LL)
    F(Push, 0, 1LL)
    F(Vector, 0, 1LL)
    F(Reserved, 0, 1LL)
    F(Enter, 0, 1LL)
    F(Exit, 0, 1LL)
    F(Wallpush, 0, 1LL)
    F(color, 0, 1LL)
    F(unused2, 0, 1LL)
    F(busyTimeB, 0, 4095LL)
    F(waitTimeB, 0, 4095LL)
    F(stopOn, 0, 1LL)
    F(stopOff, 0, 1LL)
    F(ceilpal, 0, 15LL)
    F(offCeilZ, -2147483648, 2147483647LL)
    F(onCeilZ, -2147483648, 2147483647LL)
    F(offFloorZ, -2147483648, 2147483647LL)
    F(onFloorZ, -2147483648, 2147483647LL)
    F(marker0, 0, 65535LL)
    F(marker1, 0, 65535LL)
    F(Crush, 0, 1LL)
    F(ceilXPanFrac, 0, 255LL)
    F(ceilYPanFrac, 0, 255LL)
    F(floorXPanFrac, 0, 255LL)
    F(damageType, 0, 7LL)
    F(floorpal, 0, 15LL)
    F(floorYPanFrac, 0, 255LL)
    F(locked, 0, 1LL)
#ifdef NOONE_EXTENSIONS
    F(windVel, 0, 4294967295LL)
#else
    F(windVel, 0, 1023LL)
#endif
    F(windAng, 0, 2047LL)
    F(windAlways, 0, 1LL)
    F(dudeLockout, 0, 1LL)
    F(bobTheta, 0, 2047LL)
    F(bobZRange, 0, 31LL)
    F(bobSpeed, -2048, 2047LL)
    F(bobAlways, 0, 1LL)
    F(bobFloor, 0, 1LL)
    F(bobCeiling, 0, 1LL)
    F(bobRotate, 0, 1LL)
}

void xwallFields(Archive& io, XWALL& v)
{
    F(reference, -16384, 16383LL)
    F(state, 0, 1LL)
    F(busy, 0, 131071LL)
    F(data, -32768, 32767LL)
    F(txID, 0, 1023LL)
    F(unused1, 0, 63LL)
    F(rxID, 0, 1023LL)
    F(command, 0, 255LL)
    F(triggerOn, 0, 1LL)
    F(triggerOff, 0, 1LL)
    F(busyTime, 0, 4095LL)
    F(waitTime, 0, 4095LL)
    F(restState, 0, 1LL)
    F(interruptable, 0, 1LL)
    F(panAlways, 0, 1LL)
    F(panXVel, -128, 127LL)
    F(panYVel, -128, 127LL)
    F(decoupled, 0, 1LL)
    F(triggerOnce, 0, 1LL)
    F(isTriggered, 0, 1LL)
    F(key, 0, 7LL)
    F(triggerPush, 0, 1LL)
    F(triggerVector, 0, 1LL)
    F(triggerTouch, 0, 1LL)
    F(unused2, 0, 3LL)
    F(xpanFrac, 0, 255LL)
    F(ypanFrac, 0, 255LL)
    F(locked, 0, 1LL)
    F(dudeLockout, 0, 1LL)
    F(unused3, 0, 15LL)
    F(unused4, 0, 4294967295LL)
}

void inputFields(Archive& io, GINPUT& v)
{
    B(syncFlags.buttonChange) B(syncFlags.keyChange) B(syncFlags.useChange)
    B(syncFlags.weaponChange) B(syncFlags.mlookChange) B(syncFlags.run)
    F(forward, -32768, 32767) I(q16turn) F(strafe, -32768, 32767)
    B(buttonFlags.jump) B(buttonFlags.crouch) B(buttonFlags.shoot) B(buttonFlags.shoot2)
    B(buttonFlags.lookUp) B(buttonFlags.lookDown)
    B(keyFlags.action) B(keyFlags.jab) B(keyFlags.prevItem) B(keyFlags.nextItem)
    B(keyFlags.useItem) B(keyFlags.prevWeapon) B(keyFlags.nextWeapon) B(keyFlags.holsterWeapon)
    B(keyFlags.lookCenter) B(keyFlags.lookLeft) B(keyFlags.lookRight) B(keyFlags.spin180)
    B(keyFlags.pause) B(keyFlags.quit) B(keyFlags.restart) B(keyFlags.lastWeapon)
    B(useFlags.useBeastVision) B(useFlags.useCrystalBall) B(useFlags.useJumpBoots) B(useFlags.useMedKit)
    F(newWeapon, 0, kWeaponMax-1) I(q16mlook)
}

void postureFields(Archive& io, POSTURE& v)
{
    I(frontAccel) I(sideAccel) I(backAccel) I(pace[0]) I(pace[1])
    I(bobV) I(bobH) I(swayV) I(swayH) I(eyeAboveZ) I(weaponAboveZ)
    I(xOffset) I(zOffset) I(normalJumpZ) I(pwupJumpZ)
}

void playerFields(Archive& io, PLAYER& v)
{
    inputFields(io, v.input);
    I(used1)
    I(weaponQav)
    I(qavCallback)
    B(isRunning)
    I(posture)
    I(sceneQav)
    I(bobPhase)
    I(bobAmp)
    I(bobHeight)
    I(bobWidth)
    I(swayPhase)
    I(swayAmp)
    I(swayHeight)
    I(swayWidth)
    I(nPlayer)
    I(nSprite)
    I(lifeMode)
    I(bloodlust)
    I(zView)
    I(zViewVel)
    I(zWeapon)
    I(zWeaponVel)
    I(q16look)
    I(q16horiz)
    I(q16slopehoriz)
    I(slope)
    B(isUnderwater)
    for (int i0 = 0; i0 < 8; ++i0) B(hasKey[i0])
    F(hasFlag, -128, 127)
    for (int i0 = 0; i0 < 8; ++i0) F(used2[i0], -32768, 32767)
    for (int i0 = 0; i0 < kDamageMax; ++i0) I(damageControl[i0])
    F(curWeapon, -128, 127)
    F(nextWeapon, -128, 127)
    I(weaponTimer)
    I(weaponState)
    I(weaponAmmo)
    for (int i0 = 0; i0 < kWeaponMax; ++i0) B(hasWeapon[i0])
    for (int i0 = 0; i0 < kWeaponMax; ++i0) I(weaponMode[i0])
    for (int i0 = 0; i0 < 2; ++i0) for (int i1 = 0; i1 < kWeaponMax; ++i1) I(weaponOrder[i0][i1])
    for (int i0 = 0; i0 < 12; ++i0) I(ammoCount[i0])
    B(qavLoop)
    I(fuseTime)
    I(throwTime)
    I(throwPower)
    I(aimTarget)
    I(aimTargetsCount)
    for (int i0 = 0; i0 < 16; ++i0) F(aimTargets[i0], -32768, 32767)
    I(deathTime)
    for (int i0 = 0; i0 < kMaxPowerUps; ++i0) I(pwUpTime[i0])
    I(fragCount)
    for (int i0 = 0; i0 < kMaxPlayers; ++i0) I(fragInfo[i0])
    I(teamId)
    I(fraggerId)
    I(underwaterTime)
    I(bloodTime)
    I(gooTime)
    I(wetTime)
    I(bubbleTime)
    I(lastWeapon)
    I(restTime)
    I(kickPower)
    I(laughCount)
    I(spin)
    B(godMode)
    B(fallScream)
    B(cantJump)
    I(packItemTime)
    I(packItemId)
    for (int i0 = 0; i0 < 3; ++i0) I(armor[i0])
    I(voodooTarget)
    I(voodooTargets)
    I(voodooVar1)
    I(vodooVar2)
    I(flickerEffect)
    I(tiltEffect)
    I(visibility)
    I(painEffect)
    I(blindEffect)
    I(chokeEffect)
    I(handTime)
    B(hand)
    I(pickupEffect)
    B(flashEffect)
    I(quakeEffect)
    I(q16ang)
    I(angold)
    I(player_par)
    I(nWaterPal)
    I(aim.dx) I(aim.dy) I(aim.dz) I(relAim.dx) I(relAim.dy) I(relAim.dz)
    for (int i = 0; i < kPackMax; ++i) { B(packSlots[i].isActive) I(packSlots[i].curAmount) }
    for (int i = 0; i < kModeMax; ++i)
        for (int j = 0; j < kPostureMax; ++j) postureFields(io, v.pPosture[i][j]);
}

void profileFields(Archive& io, PROFILE& v)
{
    F(nAutoAim, 0, 3) F(nWeaponSwitch, 0, 3) F(skill, 0, 4)
    io.string(v.name);
}

void optionsFields(Archive& io, GAMEOPTIONS& v)
{
    F(nGameType, 0, kGameTypeTeams)
    F(nDifficulty, 0, 4) F(nDifficultyQuantity, 0, 4) F(nDifficultyHealth, 0, 4)
    F(nEpisode, 0, kMaxEpisodes-1) F(nLevel, 0, kMaxLevels-1)
    io.string(v.zLevelName); io.string(v.zLevelSong);
    I(nTrackNumber)
    io.string(v.szSaveGameName); io.string(v.szUserGameName);
    F(nSaveGameSlot, -32768, 32767) I(picEntry) U(uMapCRC)
    F(nMonsterSettings, 0, 2) I(uGameFlags) I(uNetGameFlags)
    F(nWeaponSettings, 0, 3) F(nItemSettings, 0, 3) F(nRespawnSettings, 0, 3) F(nTeamSettings, 0, 3)
    I(nMonsterRespawnTime) I(nWeaponRespawnTime) I(nItemRespawnTime) I(nSpecialRespawnTime)
    B(weaponsV10x) B(bFriendlyFire) F(bPlayerKeys, LOSTONDEATH, SHARED)
    io.string(v.szUserMap);
}

void extensionFields(Archive& io, spriteext_t& v)
{
    v.alpha = io.real(v.alpha);
    // mdanimcur/mdanimtims and tspr are renderer-local caches. A dedicated
    // server never advances them; copying them would restart model animations.
    F(flags, 0, 65535)
    F(mdangoff, -32768, 32767) F(xpanning, 0, 255) F(ypanning, 0, 255)
    F(mdpitch, -32768, 32767) F(mdroll, -32768, 32767)
    I(mdpivot_offset.x) I(mdpivot_offset.y) I(mdpivot_offset.z)
    I(mdposition_offset.x) I(mdposition_offset.y) I(mdposition_offset.z)
}

struct SectorState
{
    usectortype value = {};
    XSECTOR extension = {};
    int32_t ceilingVelocity = 0, floorVelocity = 0;
    int16_t upperLink = -1, lowerLink = -1, head = -1;
    int8_t filler = 0;
};
struct WallState
{
    uwalltype value = {};
    XWALL extension = {};
    uint8_t blend = 0;
};
struct SpriteState
{
    uint32_t generation = 0;
    uspritetype value = {};
    spriteext_t extension = {};
    XSPRITE extra = {};
    SPRITEHIT hit = {};
    int32_t velocity[3] = {};
    int16_t nextSector = -1, previousSector = -1, nextStatus = -1, previousStatus = -1;
    int32_t visualAiType = 0;
    bool attachedHand = false;
    int8_t filler = 0;
};
#ifdef NOONE_EXTENSIONS
struct LaserState
{
    int32_t owner = -1;
    uint8_t color = 0, vertical = 0, direction = 0;
    std::vector<LASER::RAYPIECE> pieces;
};
#endif
struct ClientWorld
{
    GAMEOPTIONS options = {};
    uint8_t mask = 0;
    int32_t frame = 0, clock = 0, levelTime = 0, visibility = 0, engineVisibility = 0;
    int32_t fog = 0, paused = 0, modern = 0;
    int32_t killsTotal = 0, killsDone = 0, secretsTotal = 0, secretsDone = 0, superSecrets = 0;
    bool blueFlag = false, redFlag = false;
    std::vector<SectorState> sectors;
    std::vector<WallState> walls;
    std::vector<SpriteState> sprites;
    int16_t statusHeads[kMaxStatus] = {};
    PLAYER players[kMaxPlayers] = {};
    PROFILE profiles[kMaxPlayers] = {};
    int32_t scores[kMaxPlayers] = {}, scoreTicks[kMaxPlayers] = {};
#ifdef NOONE_EXTENSIONS
    int16_t qavControllers[kMaxPlayers] = {};
    std::vector<LaserState> lasers;
#endif
};

bool stateFields(Archive& io, ClientWorld& v)
{
    uint32_t magic = kStateMagic, version = kNetSnapshotVersion;
    magic = uint32_t(io.number(magic, 0, UINT32_MAX));
    version = uint32_t(io.number(version, 0, UINT32_MAX));
    if (magic != kStateMagic || version != kNetSnapshotVersion) io.fail("unsupported client snapshot version");
    uint32_t features = 0;
#ifdef NOONE_EXTENSIONS
    features |= 1;
#endif
#ifdef NEW_MAP_FORMAT
    features |= 2;
#endif
    if (io.number(features, 0, 3) != features) io.fail("snapshot build feature mismatch");
    F(mask, 0, 255)
    optionsFields(io, v.options);
    I(frame) I(clock) I(levelTime) I(visibility) I(engineVisibility)
    F(fog, 0, 1) B(paused) F(modern, 0, 2) B(blueFlag) B(redFlag)
    I(killsTotal) I(killsDone) I(secretsTotal) I(secretsDone) I(superSecrets)
#ifndef NOONE_EXTENSIONS
    if (v.modern) io.fail("modern map snapshot requires an extension-enabled client");
#endif
    size_t sectorCount = size_t(io.number(v.sectors.size(), 1, kMaxSectors));
    size_t wallCount = size_t(io.number(v.walls.size(), 1, kMaxWalls));
    size_t spriteCount = size_t(io.number(v.sprites.size(), 0, kMaxSprites));
    if (!io.ok) return false;
    if (io.reading) { v.sectors.resize(sectorCount); v.walls.resize(wallCount); v.sprites.resize(spriteCount); }
    for (auto& s : v.sectors)
    {
        sectorFields(io, s.value);
        s.ceilingVelocity = int32_t(io.number(s.ceilingVelocity, INT32_MIN, INT32_MAX));
        s.floorVelocity = int32_t(io.number(s.floorVelocity, INT32_MIN, INT32_MAX));
        s.upperLink = int16_t(io.number(s.upperLink, -1, kMaxSprites-1));
        s.lowerLink = int16_t(io.number(s.lowerLink, -1, kMaxSprites-1));
        s.head = int16_t(io.number(s.head, -1, kMaxSprites-1));
        s.filler = int8_t(io.number(s.filler, -128, 127));
        if (s.value.extra > 0) xsectorFields(io, s.extension);
    }
    for (auto& s : v.walls)
    {
        wallFields(io, s.value);
        s.blend = uint8_t(io.number(s.blend, 0, 255));
        if (s.value.extra > 0) xwallFields(io, s.extension);
    }
    for (auto& s : v.sprites)
    {
        spriteFields(io, s.value);
        s.generation = uint32_t(io.number(s.generation, 1, UINT32_MAX));
        extensionFields(io, s.extension);
        for (int i = 0; i < 3; ++i) s.velocity[i] = int32_t(io.number(s.velocity[i], INT32_MIN, INT32_MAX));
        s.nextSector = int16_t(io.number(s.nextSector, -1, kMaxSprites-1));
        s.previousSector = int16_t(io.number(s.previousSector, -1, kMaxSprites-1));
        s.nextStatus = int16_t(io.number(s.nextStatus, -1, kMaxSprites-1));
        s.previousStatus = int16_t(io.number(s.previousStatus, -1, kMaxSprites-1));
        s.filler = int8_t(io.number(s.filler, -128, 127));
        if (s.value.extra > 0)
        {
            xspriteFields(io, s.extra);
            s.hit.hit = int32_t(io.number(s.hit.hit, INT32_MIN, INT32_MAX));
            s.hit.ceilhit = int32_t(io.number(s.hit.ceilhit, INT32_MIN, INT32_MAX));
            s.hit.florhit = int32_t(io.number(s.hit.florhit, INT32_MIN, INT32_MAX));
            // Preserve kAiStateOther (-1) and custom-monster death posture (-2).
            s.visualAiType = int32_t(io.number(s.visualAiType, -2, 65535));
            s.attachedHand = io.number(s.attachedHand, 0, 1) != 0;
        }
    }
    for (int i = 0; i < kMaxStatus; ++i) F(statusHeads[i], -1, kMaxSprites-1)
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        profileFields(io, v.profiles[i]);
        I(scores[i]) I(scoreTicks[i])
        if (v.mask & (1u << i))
        {
            playerFields(io, v.players[i]);
#ifdef NOONE_EXTENSIONS
            F(qavControllers[i], -1, kMaxSprites-1)
#endif
        }
    }
#ifdef NOONE_EXTENSIONS
    size_t laserCount = size_t(io.number(v.lasers.size(), 0, 2048));
    if (!io.ok) return false;
    if (io.reading) v.lasers.resize(laserCount);
    size_t allPieces = 0;
    for (auto& laser : v.lasers)
    {
        laser.owner = int32_t(io.number(laser.owner, 0, kMaxSprites-1));
        laser.color = uint8_t(io.number(laser.color, 0, kLaserMaxColors-1));
        laser.vertical = uint8_t(io.number(laser.vertical, 0, 1));
        laser.direction = uint8_t(io.number(laser.direction, 0, 1));
        size_t pieceCount = size_t(io.number(laser.pieces.size(), 0, 131072));
        allPieces += pieceCount;
        if (allPieces > 131072) io.fail("snapshot laser piece budget exceeded");
        if (!io.ok) return false;
        if (io.reading) laser.pieces.resize(pieceCount);
        for (auto& p : laser.pieces)
        {
            p.x = int32_t(io.number(p.x, INT32_MIN, INT32_MAX));
            p.y = int32_t(io.number(p.y, INT32_MIN, INT32_MAX));
            p.z = int32_t(io.number(p.z, INT32_MIN, INT32_MAX));
            p.s = int16_t(io.number(p.s, 0, kMaxSectors-1));
            p.w = uint8_t(io.number(p.w, 0, 255));
            p.h = uint8_t(io.number(p.h, 0, 255));
        }
    }
#endif
    return io.ok;
}

#undef F
#undef I
#undef U
#undef B

bool validateWorld(const ClientWorld& world, std::string& error)
{
    auto fail = [&](const char* why) { error = why; return false; };
    size_t const ns = world.sectors.size(), nw = world.walls.size();
    std::vector<int> spriteIndex(kMaxSprites, -1);
    std::vector<bool> xSectorSeen(kMaxXSectors), xWallSeen(kMaxXWalls), xSpriteSeen(kMaxXSprites);
    std::vector<int> wallSector(nw, -1);
    std::vector<unsigned> wallIncoming(nw, 0);
    for (size_t i = 0; i < ns; ++i)
    {
        auto const& s = world.sectors[i];
        // E6M7 contains a two-wall sector that the original engine accepts.
        if (size_t(s.value.wallptr) >= nw || size_t(s.value.wallptr)+s.value.wallnum > nw || s.value.wallnum < 2)
            return fail("invalid snapshot sector wall range");
        for (int j = s.value.wallptr; j < s.value.wallptr+s.value.wallnum; ++j)
        {
            if (wallSector[j] != -1) return fail("overlapping snapshot sector wall ranges");
            wallSector[j] = int(i);
        }
        if (s.value.extra > 0)
        {
            if (xSectorSeen[s.value.extra] || s.extension.reference != int(i)) return fail("invalid snapshot sector extension reference");
            xSectorSeen[s.value.extra] = true;
        }
    }
    for (size_t i = 0; i < nw; ++i)
    {
        auto const& s = world.walls[i];
        if (wallSector[i] < 0 || size_t(s.value.point2) >= nw || wallSector[i] != wallSector[s.value.point2])
            return fail("invalid snapshot wall loop");
        if (++wallIncoming[s.value.point2] != 1) return fail("snapshot wall loops merge instead of closing");
        if ((s.value.nextwall < 0) != (s.value.nextsector < 0) ||
            s.value.nextwall >= int(nw) || s.value.nextsector >= int(ns)) return fail("invalid snapshot wall portal");
        // Shipped CP02 has stale nextsector ownership and E3M5 contains an
        // asymmetric portal. Preserve their bounded legacy topology instead
        // of requiring reciprocal links that the original maps do not have.
        if (s.value.overpicnum < 0 && (s.value.cstat & (16 | 32)))
            return fail("invalid snapshot visible wall overlay");
        if (s.value.extra > 0)
        {
            if (xWallSeen[s.value.extra] || s.extension.reference != int(i)) return fail("invalid snapshot wall extension reference");
            xWallSeen[s.value.extra] = true;
        }
    }
    for (size_t i = 0; i < world.sprites.size(); ++i)
    {
        auto const& s = world.sprites[i];
        int const id = s.value.index;
        if (spriteIndex[id] != -1 || s.value.sectnum >= int(ns)) return fail("duplicate sprite ID or invalid sprite sector");
        spriteIndex[id] = int(i);
        if (s.value.extra > 0)
        {
            if (xSpriteSeen[s.value.extra] || s.extra.reference != id) return fail("invalid snapshot sprite extension reference");
            xSpriteSeen[s.value.extra] = true;
        }
        if ((s.value.statnum == kStatDude || s.value.statnum == kStatThing) && s.value.extra <= 0)
            return fail("actor snapshot lacks its extended state");
        if (s.value.extra <= 0 && (s.value.statnum == kStatExplosion ||
            (s.value.statnum == kStatTraps && s.value.type == kTrapSawCircular) ||
            ((s.value.flags & kHitagRespawn) && s.value.owner == 3)))
            return fail("snapshot visual effect lacks its extended sprite state");
        if (s.attachedHand && (s.extra.target < 0 || s.extra.target >= kMaxSprites)) return fail("invalid attached hand target");
    }
    // Check every chain once; preserving list order preserves collision tie
    // breaking and sprite draw order without trusting arbitrary packet links.
    for (int kind = 0; kind < 2; ++kind)
    {
        std::vector<bool> seen(kMaxSprites);
        size_t count = 0;
        int const chains = kind ? kMaxStatus : int(ns);
        for (int chain = 0; chain < chains; ++chain)
        {
            int previous = -1;
            int current = kind ? world.statusHeads[chain] : world.sectors[chain].head;
            int const first = current;
            while (current >= 0)
            {
                if (seen[current] || spriteIndex[current] < 0) return fail("cyclic or missing sprite in snapshot list");
                seen[current] = true;
                ++count;
                auto const& s = world.sprites[spriteIndex[current]];
                if ((kind ? s.value.statnum : s.value.sectnum) != chain ||
                    (previous >= 0 && (kind ? s.previousStatus : s.previousSector) != previous))
                    return fail("inconsistent snapshot sprite list");
                previous = current;
                current = kind ? s.nextStatus : s.nextSector;
            }
            if (first >= 0)
            {
                auto const& firstSprite = world.sprites[spriteIndex[first]];
                if ((kind ? firstSprite.previousStatus : firstSprite.previousSector) != previous)
                    return fail("invalid snapshot sprite-list tail reference");
            }
        }
        if (count != world.sprites.size()) return fail("snapshot sprite is absent from its list");
    }
    for (auto const& s : world.sectors)
    {
        for (int link : {int(s.upperLink), int(s.lowerLink)})
            if (link >= 0 && spriteIndex[link] < 0) return fail("snapshot sector link references absent sprite");
    }
    for (auto const& s : world.sprites)
        if (s.attachedHand && spriteIndex[s.extra.target] < 0) return fail("snapshot hand target is absent");
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        if (!(world.mask & (1u << i))) continue;
        auto const& p = world.players[i];
        if (p.nPlayer != i || p.nSprite < 0 || p.nSprite >= kMaxSprites || spriteIndex[p.nSprite] < 0)
            return fail("invalid snapshot player sprite");
        auto const& s = world.sprites[spriteIndex[p.nSprite]];
        if (s.value.type != kDudePlayer1+i || s.value.extra <= 0)
            return fail("snapshot player identity disagrees with sprite");
        if (unsigned(p.posture) >= kPostureMax || unsigned(p.lifeMode) >= kModeMax ||
            p.weaponQav < -1 || p.weaponQav >= 126 || p.sceneQav < -1 ||
            unsigned(p.curWeapon) >= kWeaponMax || unsigned(p.nextWeapon) >= kWeaponMax ||
            p.weaponAmmo < -1 || p.weaponAmmo >= 12 ||
            p.packItemId < -1 || p.packItemId >= kPackMax || p.aimTargetsCount < 0 || p.aimTargetsCount > 16 ||
            unsigned(p.nWaterPal) >= MAXPALOOKUPS) return fail("invalid snapshot player HUD or movement state");
#ifdef NOONE_EXTENSIONS
        if (p.sceneQav >= 0 && (world.qavControllers[i] < 0 || spriteIndex[world.qavControllers[i]] < 0))
            return fail("snapshot QAV scene controller is absent");
#else
        if (p.sceneQav >= 0) return fail("snapshot QAV scene requires an extension-enabled client");
#endif
    }
#ifdef NOONE_EXTENSIONS
    if (!world.modern && !world.lasers.empty()) return fail("snapshot lasers require a modern map");
    std::vector<bool> laserOwner(kMaxSprites);
    for (size_t i = 0; i < world.lasers.size(); ++i)
    {
        auto const& laser = world.lasers[i];
        if (spriteIndex[laser.owner] < 0 || laserOwner[laser.owner]) return fail("invalid snapshot laser owner");
        laserOwner[laser.owner] = true;
        auto const& owner = world.sprites[spriteIndex[laser.owner]];
        if (owner.value.type != kModernLaserGen || owner.value.extra <= 0 || owner.extra.sysData1 != int(i))
            return fail("snapshot laser registry disagrees with sprite state");
        for (auto const& piece : laser.pieces)
            if (piece.s >= int(ns)) return fail("snapshot laser piece has invalid sector");
    }
#endif
    return true;
}

bool decode(const std::vector<uint8_t>& state, ClientWorld& world, std::string& error)
{
    if (state.empty() || state.size() > kNetSnapshotMaxBytes) { error = "invalid snapshot state length"; return false; }
    Archive reader(state, error);
    if (!stateFields(reader, world)) return false;
    if (reader.position != state.size()) { error = "trailing bytes in snapshot state"; return false; }
    if (!validateWorld(world, error)) return false;
    error.clear();
    return true;
}
}

void netSnapshotSpriteCreated(int index)
{
    if (unsigned(index) < kMaxSprites && !++spriteGeneration[index]) ++spriteGeneration[index];
}

void netSnapshotResetGenerations()
{
    std::fill(std::begin(spriteGeneration), std::end(spriteGeneration), 0);
}

uint32_t netSnapshotSpriteGeneration(int index)
{
    return unsigned(index) < kMaxSprites ? spriteGeneration[index] : 0;
}

bool netSnapshotCapture(std::vector<uint8_t>& state, uint8_t playerMask, std::string& error)
{
    auto world = std::unique_ptr<ClientWorld>(new ClientWorld());
    world->mask = playerMask;
    world->options = gGameOptions;
    world->frame = gFrame; world->clock = int(gFrameClock); world->levelTime = gLevelTime;
    world->visibility = gVisibility; world->engineVisibility = g_visibility;
    world->fog = gFogMode; world->paused = gPaused;
    world->blueFlag = gBlueFlagDropped; world->redFlag = gRedFlagDropped;
    world->killsTotal = gKillMgr.at0; world->killsDone = gKillMgr.at4;
    world->secretsTotal = gSecretMgr.nAllSecrets; world->secretsDone = gSecretMgr.nNormalSecretsFound;
    world->superSecrets = gSecretMgr.nSuperSecretsFound;
#ifdef NOONE_EXTENSIONS
    world->modern = gModernMap;
#endif
    if (numsectors <= 0 || numsectors > kMaxSectors || numwalls <= 0 || numwalls > kMaxWalls)
    { error = "no valid loaded world to capture"; return false; }
    world->sectors.resize(numsectors);
    world->walls.resize(numwalls);
    for (int i = 0; i < numsectors; ++i)
    {
        auto& s = world->sectors[i];
        memcpy(&s.value, &sector[i], sizeof(s.value));
        if (s.value.extra > 0 && s.value.extra < kMaxXSectors) s.extension = xsector[s.value.extra];
        s.ceilingVelocity = velCeil[i]; s.floorVelocity = velFloor[i];
        s.upperLink = gUpperLink[i]; s.lowerLink = gLowerLink[i];
        s.head = headspritesect[i]; s.filler = qsector_filler[i];
    }
    for (int i = 0; i < numwalls; ++i)
    {
        auto& s = world->walls[i];
        memcpy(&s.value, &wall[i], sizeof(s.value));
        if (s.value.extra > 0 && s.value.extra < kMaxXWalls) s.extension = xwall[s.value.extra];
#ifndef NEW_MAP_FORMAT
        s.blend = wallext[i].blend;
#endif
    }
    for (int i = 0; i < kMaxSprites; ++i)
    {
        if (sprite[i].statnum >= kMaxStatus) continue;
        SpriteState s;
        memcpy(&s.value, &sprite[i], sizeof(s.value));
        if (!spriteGeneration[i]) netSnapshotSpriteCreated(i); // original map entries do not call InsertSprite
        s.generation = spriteGeneration[i];
        s.extension = spriteext[i];
        if (s.value.extra > 0 && s.value.extra < kMaxXSprites)
        {
            s.extra = xsprite[s.value.extra]; s.hit = gSpriteHit[s.value.extra];
            s.visualAiType = s.extra.aiState ? s.extra.aiState->stateType : 0;
            s.attachedHand = s.value.type == kDudeHand && s.extra.aiState == &hand13A3B4;
        }
        s.velocity[0] = xvel[i]; s.velocity[1] = yvel[i]; s.velocity[2] = zvel[i];
        s.nextSector = nextspritesect[i]; s.previousSector = prevspritesect[i];
        s.nextStatus = nextspritestat[i]; s.previousStatus = prevspritestat[i];
        s.filler = qsprite_filler[i];
        world->sprites.push_back(s);
    }
    for (int i = 0; i < kMaxStatus; ++i) world->statusHeads[i] = headspritestat[i];
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        world->players[i] = gPlayer[i]; world->profiles[i] = gProfile[i];
        world->scores[i] = gPlayerScores[i]; world->scoreTicks[i] = int(gPlayerScoreTicks[i]);
#ifdef NOONE_EXTENSIONS
        world->qavControllers[i] = gPlayerCtrl[i].qavScene.index;
#endif
    }
#ifdef NOONE_EXTENSIONS
    if (gLasersCount > 2048) { error = "laser registry exceeds protocol limit"; return false; }
    for (unsigned i = 0; i < gLasersCount; ++i)
    {
        auto const* source = gLaser[i];
        if (!source || !source->pOwn || source->numpieces > 131072 ||
            (source->numpieces && !source->pieces)) { error = "invalid server laser state"; return false; }
        LaserState laser;
        laser.owner = source->pOwn->index; laser.color = source->color;
        laser.vertical = source->vertical; laser.direction = source->direction;
        if (source->numpieces) laser.pieces.assign(source->pieces, source->pieces+source->numpieces);
        world->lasers.push_back(std::move(laser));
    }
#endif
    std::vector<uint8_t> result;
    Archive writer(result, error);
    if (!stateFields(writer, *world) || !validateWorld(*world, error)) return false;
    state.swap(result);
    error.clear();
    return true;
}

bool netSnapshotValidate(const std::vector<uint8_t>& state, std::string& error)
{
    auto world = std::unique_ptr<ClientWorld>(new ClientWorld());
    return decode(state, *world, error);
}

bool netSnapshotGetOptions(const std::vector<uint8_t>& state, GAMEOPTIONS& options,
                           uint8_t& playerMask, std::string& error)
{
    auto world = std::unique_ptr<ClientWorld>(new ClientWorld());
    if (!decode(state, *world, error)) return false;
    options = world->options;
    playerMask = world->mask;
    return true;
}

bool netSnapshotApply(const std::vector<uint8_t>& state, int localSlot, std::string& error)
{
    int const previousLocal = myconnectindex;
    int const previousView = gViewIndex;
    auto world = std::unique_ptr<ClientWorld>(new ClientWorld());
    if (!decode(state, *world, error)) return false;
    if (unsigned(localSlot) >= kMaxPlayers || !(world->mask & (1u << localSlot)))
    { error = "local player is absent from snapshot roster"; return false; }
    // StartLevel loads resources/mirror structures first. Snapshots install only
    // dynamic state; neither startup nor simulation callbacks run here.
    if (numsectors != int(world->sectors.size()) || numwalls != int(world->walls.size()) ||
        gGameOptions.uMapCRC != world->options.uMapCRC)
    { error = "snapshot map does not match the loaded client map"; return false; }
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        if (!(world->mask & (1u << i))) continue;
        int const qav = world->players[i].weaponQav;
        if (qav >= 0 && (!weaponQAV[qav] || weaponQAV[qav]->at10 <= 0))
        { error = "snapshot weapon animation resource is missing or invalid"; return false; }
    }
    std::vector<uint8_t> sameIdentity(kMaxSprites, 0);
    for (auto& s : world->sprites)
    {
        int const id = s.value.index;
        if (spriteGeneration[id] != s.generation || sprite[id].statnum != s.value.statnum ||
            sprite[id].type != s.value.type) continue;
        sameIdentity[id] = 1;
        s.extension.mdanimcur = spriteext[id].mdanimcur;
        s.extension.mdanimtims = spriteext[id].mdanimtims;
    }

#ifdef NOONE_EXTENSIONS
    if (gModernMap != world->modern) { error = "snapshot modern-map revision mismatch"; return false; }
    // Resource and allocation preflight precedes any authoritative world write.
    // A rejected update leaves the previous world available for presentation.
    QAV* sceneResources[kMaxPlayers] = {};
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        if (!(world->mask & (1u << i)) || world->players[i].sceneQav < 0) continue;
        sceneResources[i] = playerQavSceneLoad(world->players[i].sceneQav);
        if (!sceneResources[i] || sceneResources[i]->at10 <= 0)
        { error = "snapshot QAV scene resource is missing or invalid"; return false; }
    }
    std::vector<std::unique_ptr<LASER>> lasers;
    lasers.reserve(world->lasers.size());
    for (auto const& source : world->lasers)
    {
        std::unique_ptr<LASER> laser(new LASER());
        laser->color = source.color; laser->vertical = source.vertical; laser->direction = source.direction;
        if (!source.pieces.empty())
        {
            laser->pieces = static_cast<LASER::RAYPIECE*>(malloc(source.pieces.size()*sizeof(LASER::RAYPIECE)));
            if (!laser->pieces) { error = "unable to allocate snapshot laser pieces"; return false; }
            laser->numpieces = laser->piecescapacity = source.pieces.size();
            std::copy(source.pieces.begin(), source.pieces.end(), laser->pieces);
            for (auto const& piece : source.pieces) SetBitString(laser->piecesectmap, piece.s);
        }
        lasers.push_back(std::move(laser));
    }
#endif

    // Everything is decoded and graph-validated before the first world write.
    // Server snapshots bypass DeleteSprite. Retire renderer-owned resources
    // when a slot disappears or becomes another entity, without actor callbacks.
    for (int i = 0; i < kMaxSprites; ++i)
    {
        if (sameIdentity[i]) continue;
        memset(&spritesmooth[i], 0, sizeof(spritesmooth[i]));
#ifdef POLYMER
        if (gPolymerLight[i].lightptr) DeleteLight(i);
        gPolymerLight[i].lightcount = 0;
        gPolymerLight[i].lightmaxrange = 0;
#endif
    }
    gGameOptions = world->options;
    gFrame = world->frame; gFrameClock = world->clock; gLevelTime = world->levelTime;
    gVisibility = world->visibility; g_visibility = world->engineVisibility;
    gFogMode = world->fog; gPaused = world->paused;
    gBlueFlagDropped = world->blueFlag; gRedFlagDropped = world->redFlag;
    gKillMgr.at0 = world->killsTotal; gKillMgr.at4 = world->killsDone;
    gSecretMgr.nAllSecrets = world->secretsTotal; gSecretMgr.nNormalSecretsFound = world->secretsDone;
    gSecretMgr.nSuperSecretsFound = world->superSecrets;
    memset(xsector, 0, sizeof(xsector)); memset(xwall, 0, sizeof(xwall));
    memset(xsprite, 0, sizeof(xsprite)); memset(gSpriteHit, 0, sizeof(gSpriteHit));
    for (int i = 0; i < numsectors; ++i)
    {
        auto const& s = world->sectors[i];
        memcpy(&sector[i], &s.value, sizeof(s.value));
        if (s.value.extra > 0) xsector[s.value.extra] = s.extension;
        velCeil[i] = s.ceilingVelocity; velFloor[i] = s.floorVelocity;
        gUpperLink[i] = s.upperLink; gLowerLink[i] = s.lowerLink;
        headspritesect[i] = s.head; qsector_filler[i] = s.filler;
#ifdef USE_STRUCT_TRACKERS
        ++sectorchanged[i];
#endif
    }
    for (int i = 0; i < numwalls; ++i)
    {
        auto const& s = world->walls[i];
        memcpy(&wall[i], &s.value, sizeof(s.value));
        if (s.value.extra > 0) xwall[s.value.extra] = s.extension;
#ifndef NEW_MAP_FORMAT
        wallext[i].blend = s.blend;
#endif
#ifdef USE_STRUCT_TRACKERS
        ++wallchanged[i];
#endif
    }
    memset(sprite, 0, sizeof(sprite[0])*kMaxSprites);
    memset(spriteext, 0, sizeof(spriteext[0])*kMaxSprites);
    memset(gStatCount, 0, sizeof(gStatCount));
    for (int i = 0; i < kMaxSprites; ++i)
    {
        sprite[i].statnum = kMaxStatus; sprite[i].sectnum = -1; sprite[i].index = -1;
        sprite[i].extra = -1;
        xvel[i] = yvel[i] = zvel[i] = 0; qsprite_filler[i] = 0;
        nextspritesect[i] = prevspritesect[i] = nextspritestat[i] = prevspritestat[i] = -1;
    }
    for (auto const& s : world->sprites)
    {
        int const id = s.value.index;
        memcpy(&sprite[id], &s.value, sizeof(s.value));
        spriteGeneration[id] = s.generation;
        spriteext[id] = s.extension;
        spriteext[id].tspr = nullptr;
        if (s.value.extra > 0)
        {
            int const x = s.value.extra;
            xsprite[x] = s.extra;
            visualAiState[x] = {};
            visualAiState[x].stateType = s.visualAiType;
            xsprite[x].aiState = s.attachedHand ? &hand13A3B4 : &visualAiState[x];
            gSpriteHit[x] = s.hit;
        }
        xvel[id] = s.velocity[0]; yvel[id] = s.velocity[1]; zvel[id] = s.velocity[2];
        nextspritesect[id] = s.nextSector; prevspritesect[id] = s.previousSector;
        nextspritestat[id] = s.nextStatus; prevspritestat[id] = s.previousStatus;
        qsprite_filler[id] = s.filler;
        ++gStatCount[s.value.statnum];
#ifdef USE_STRUCT_TRACKERS
        ++spritechanged[id];
#endif
    }
    for (int i = 0; i < kMaxStatus; ++i) headspritestat[i] = world->statusHeads[i];
    for (int i = numsectors; i <= kMaxSectors; ++i) headspritesect[i] = -1;
    headspritestat[kMaxStatus] = -1;
    int lastFree = -1;
    for (int i = 0; i < kMaxSprites; ++i)
    {
        if (sprite[i].statnum < kMaxStatus) continue;
        if (lastFree < 0) headspritestat[kMaxStatus] = i;
        else nextspritestat[lastFree] = i;
        prevspritestat[i] = lastFree;
        lastFree = i;
    }
    if (headspritestat[kMaxStatus] >= 0) prevspritestat[headspritestat[kMaxStatus]] = lastFree;
    Numsprites = int(world->sprites.size());
    gStatCount[kMaxStatus] = kMaxSprites-Numsprites;
    connecthead = -1;
    int previousPlayer = -1;
    numplayers = gNetPlayers = 0;
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        gProfile[i] = world->profiles[i]; gPlayerScores[i] = world->scores[i];
        gPlayerScoreTicks[i] = world->scoreTicks[i];
        connectpoint2[i] = -1;
        gPlayer[i] = world->players[i];
        gPlayer[i].pSprite = nullptr; gPlayer[i].pXSprite = nullptr; gPlayer[i].pDudeInfo = nullptr;
        if (!(world->mask & (1u << i))) { gPlayer[i].nSprite = -1; continue; }
        if (connecthead < 0) connecthead = i;
        if (previousPlayer >= 0) connectpoint2[previousPlayer] = i;
        previousPlayer = i;
        ++numplayers; ++gNetPlayers;
        auto& p = gPlayer[i];
        p.pSprite = &sprite[p.nSprite]; p.pXSprite = &xsprite[p.pSprite->extra];
        p.pDudeInfo = &dudeInfo[p.pSprite->type-kDudeBase];
    }
    myconnectindex = localSlot;
    gMe = &gPlayer[localSlot];
    gViewIndex = previousLocal == localSlot && unsigned(previousView) < kMaxPlayers &&
                 (world->mask & (1u << previousView)) ? previousView : localSlot;
    gView = &gPlayer[gViewIndex];
#ifdef NOONE_EXTENSIONS
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        gPlayerCtrl[i].qavScene.index = world->qavControllers[i];
        gPlayerCtrl[i].qavScene.qavResrc = sceneResources[i];
    }
    lasersClear();
    for (size_t i = 0; i < lasers.size(); ++i)
    {
        auto& laser = lasers[i];
        laser->pOwn = &sprite[world->lasers[i].owner];
        laser->pXOwn = &xsprite[laser->pOwn->extra];
        LASER::GenerateTiles(laser->color);
        gLaser[i] = laser.release();
    }
    gLasersCount = world->lasers.size();
#endif
    InitSectorFX();
    error.clear();
    return true;
}
