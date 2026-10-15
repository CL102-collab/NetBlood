// SPDX-License-Identifier: GPL-2.0-or-later
#include "duke3d.h"
#include "rnet.h"
#include "rnet_snapshot.h"
#include "rnet_rules.h"
#include "premap.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

namespace rnet
{
namespace
{
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "snapshot alpha requires IEEE binary32");
constexpr uint32_t Magic = 0x31534e52; // "RNS1", little endian
constexpr uint16_t Version = 1;
constexpr uint32_t NoMultiVar = GAMEVAR_PTR_MASK | GAMEVAR_SPECIAL | GAMEVAR_READONLY | GAMEVAR_NOMULTI;

// All values have explicit field order and fixed-width little-endian encoding.
// Native padding, pointers, renderer caches and server credentials never travel.
struct Writer
{
    std::vector<uint8_t>& bytes;
    bool ok = true;
    template<class T> void operator()(T& value)
    {
        static_assert(std::is_integral<T>::value && sizeof(T) <= 4, "wire integer must be fixed-width");
        if (!ok || bytes.size() + sizeof(T) > MaxMessage) { ok = false; return; }
        uint32_t bits = static_cast<uint32_t>(value);
        for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(uint8_t(bits >> (8*i)));
    }
    template<class T, size_t N> void operator()(T (&values)[N]) { for (auto& v : values) (*this)(v); }
    void floating(float& value) { uint32_t bits; Bmemcpy(&bits, &value, 4); (*this)(bits); }
};
struct Reader
{
    const std::vector<uint8_t>& bytes;
    size_t pos = 0;
    bool ok = true;
    template<class T> void operator()(T& value)
    {
        static_assert(std::is_integral<T>::value && sizeof(T) <= 4, "wire integer must be fixed-width");
        if (!ok || sizeof(T) > bytes.size()-pos) { ok = false; return; }
        uint32_t bits = 0;
        for (size_t i = 0; i < sizeof(T); ++i) bits |= uint32_t(bytes[pos++]) << (8*i);
        // Conversion via unsigned representation preserves all signed bit patterns.
        typename std::make_unsigned<T>::type u = static_cast<typename std::make_unsigned<T>::type>(bits);
        Bmemcpy(&value, &u, sizeof(T));
    }
    template<class T, size_t N> void operator()(T (&values)[N]) { for (auto& v : values) (*this)(v); }
    void floating(float& value) { uint32_t bits = 0; (*this)(bits); Bmemcpy(&value, &bits, 4); }
};

// Packed game structures contain unaligned numeric members. Work through a
// properly aligned local value rather than binding an integer reference to one.
template<class A, class T> void field(A& ar, T* address)
{
    T value;
    Bmemcpy(&value,address,sizeof(T));
    ar(value);
    Bmemcpy(address,&value,sizeof(T));
}

template<class A> void playerFields(A& ar, DukePlayer_t& p)
{
    field(ar,&p.pos.x);
    field(ar,&p.pos.y);
    field(ar,&p.pos.z);
    field(ar,&p.opos.x);
    field(ar,&p.opos.y);
    field(ar,&p.opos.z);
    field(ar,&p.vel.x);
    field(ar,&p.vel.y);
    field(ar,&p.vel.z);
    field(ar,&p.npos.x);
    field(ar,&p.npos.y);
    field(ar,&p.npos.z);
    field(ar,&p.bobpos.x);
    field(ar,&p.bobpos.y);
    field(ar,&p.fric.x);
    field(ar,&p.fric.y);
    field(ar,&p.q16horiz);
    field(ar,&p.q16horizoff);
    field(ar,&p.oq16horiz);
    field(ar,&p.oq16horizoff);
    field(ar,&p.q16ang);
    field(ar,&p.oq16ang);
    field(ar,&p.q16angvel);
    field(ar,&p.truefz);
    field(ar,&p.truecz);
    field(ar,&p.player_par);
    field(ar,&p.randomflamex);
    field(ar,&p.exitx);
    field(ar,&p.exity);
    field(ar,&p.runspeed);
    field(ar,&p.max_player_health);
    field(ar,&p.max_shield_amount);
    field(ar,&p.autostep);
    field(ar,&p.autostep_sbw);
    field(ar,&p.interface_toggle_flag);
    field(ar,&p.max_actors_killed);
    field(ar,&p.actors_killed);
    field(ar,&p.gotweapon);
    field(ar,&p.zoom);
    field(ar,&p.loogiex);
    field(ar,&p.loogiey);
    field(ar,&p.sbs);
    field(ar,&p.sound_pitch);
    field(ar,&p.cursectnum);
    field(ar,&p.look_ang);
    field(ar,&p.last_extra);
    field(ar,&p.subweapon);
    field(ar,&p.max_ammo_amount);
    field(ar,&p.ammo_amount);
    field(ar,&p.inv_amount);
    field(ar,&p.wackedbyactor);
    field(ar,&p.pyoff);
    field(ar,&p.opyoff);
    field(ar,&p.newowner);
    field(ar,&p.jumping_counter);
    field(ar,&p.airleft);
    field(ar,&p.fta);
    field(ar,&p.ftq);
    field(ar,&p.access_wallnum);
    field(ar,&p.access_spritenum);
    field(ar,&p.kickback_pic);
    field(ar,&p.got_access);
    field(ar,&p.weapon_ang);
    field(ar,&p.visibility);
    field(ar,&p.somethingonplayer);
    field(ar,&p.on_crane);
    field(ar,&p.i);
    field(ar,&p.one_parallax_sectnum);
    field(ar,&p.random_club_frame);
    field(ar,&p.one_eighty_count);
    field(ar,&p.dummyplayersprite);
    field(ar,&p.extra_extra8);
    field(ar,&p.actorsqu);
    field(ar,&p.timebeforeexit);
    field(ar,&p.customexitsound);
    field(ar,&p.last_pissed_time);
    field(ar,&p.weaprecs);
    field(ar,&p.weapon_sway);
    field(ar,&p.crack_time);
    field(ar,&p.bobcounter);
    field(ar,&p.orotscrnang);
    field(ar,&p.rotscrnang);
    field(ar,&p.dead_flag);
    field(ar,&p.holoduke_on);
    field(ar,&p.pycount);
    field(ar,&p.transporter_hold);
    field(ar,&p.max_secret_rooms);
    field(ar,&p.secret_rooms);
    field(ar,&p.frag);
    field(ar,&p.fraggedself);
    field(ar,&p.quick_kick);
    field(ar,&p.last_quick_kick);
    field(ar,&p.return_to_center);
    field(ar,&p.reloading);
    field(ar,&p.weapreccnt);
    field(ar,&p.aim_mode);
    field(ar,&p.auto_aim);
    field(ar,&p.weaponswitch);
    field(ar,&p.movement_lock);
    field(ar,&p.team);
    field(ar,&p.tipincs);
    field(ar,&p.hbomb_hold_delay);
    field(ar,&p.frag_ps);
    field(ar,&p.gm);
    field(ar,&p.on_warping_sector);
    field(ar,&p.footprintcount);
    field(ar,&p.hurt_delay);
    field(ar,&p.hbomb_on);
    field(ar,&p.jumping_toggle);
    field(ar,&p.rapid_fire_hold);
    field(ar,&p.on_ground);
    field(ar,&p.inven_icon);
    field(ar,&p.buttonpalette);
    field(ar,&p.over_shoulder_on);
    field(ar,&p.show_empty_weapon);
    field(ar,&p.jetpack_on);
    field(ar,&p.spritebridge);
    field(ar,&p.lastrandomspot);
    field(ar,&p.scuba_on);
    field(ar,&p.footprintpal);
    field(ar,&p.heat_on);
    field(ar,&p.invdisptime);
    field(ar,&p.holster_weapon);
    field(ar,&p.falling_counter);
    field(ar,&p.footprintshade);
    field(ar,&p.refresh_inventory);
    field(ar,&p.last_full_weapon);
    field(ar,&p.toggle_key_flag);
    field(ar,&p.knuckle_incs);
    field(ar,&p.knee_incs);
    field(ar,&p.access_incs);
    field(ar,&p.walking_snd_toggle);
    field(ar,&p.palookup);
    field(ar,&p.hard_landing);
    field(ar,&p.fist_incs);
    field(ar,&p.numloogs);
    field(ar,&p.loogcnt);
    field(ar,&p.scream_voice);
    field(ar,&p.last_weapon);
    field(ar,&p.cheat_phase);
    field(ar,&p.weapon_pos);
    field(ar,&p.wantweaponfire);
    field(ar,&p.curr_weapon);
    field(ar,&p.palette);
    field(ar,&p.pals.r);
    field(ar,&p.pals.g);
    field(ar,&p.pals.b);
    field(ar,&p.pals.f);
    field(ar,&p.last_used_weapon);
    field(ar,&p.recoil);
    field(ar,&p.stairs);
    field(ar,&p.hbomb_offset);
    field(ar,&p.hbomb_time);
    field(ar,&p.shotgun_state);
    field(ar,&p.make_noise);
    field(ar,&p.noise_x);
    field(ar,&p.noise_y);
    field(ar,&p.noise_radius);
    field(ar,&p.keys);
    field(ar,&p.yehaa_timer);
    field(ar,&p.drink_amt);
    field(ar,&p.eat_amt);
    field(ar,&p.drink_ang);
    field(ar,&p.eat_ang);
    field(ar,&p.drink_timer);
    field(ar,&p.eat_timer);
    field(ar,&p.level_end_timer);
    field(ar,&p.moto_speed);
    field(ar,&p.tilt_status);
    field(ar,&p.moto_drink);
    field(ar,&p.on_motorcycle);
    field(ar,&p.on_boat);
    field(ar,&p.moto_underwater);
    field(ar,&p.not_on_water);
    field(ar,&p.moto_on_ground);
    field(ar,&p.moto_do_bump);
    field(ar,&p.moto_bump_fast);
    field(ar,&p.moto_on_oil);
    field(ar,&p.moto_on_mud);
    field(ar,&p.moto_bump);
    field(ar,&p.moto_bump_target);
    field(ar,&p.moto_turb);
    field(ar,&p.drug_stat);
    field(ar,&p.drug_aspect);
    field(ar,&p.drug_mode);
    field(ar,&p.lotag800kill);
    field(ar,&p.sea_sick_stat);
    field(ar,&p.drug_timer);
    field(ar,&p.sea_sick);
    field(ar,&p.hurt_delay2);
    field(ar,&p.nocheat);
    field(ar,&p.dhat60f);
    field(ar,&p.dhat613);
    field(ar,&p.dhat617);
    field(ar,&p.dhat61b);
    field(ar,&p.dhat61f);
    field(ar,&p.dn64_36d);
    field(ar,&p.dn64_36e);
    field(ar,&p.dn64_370);
    field(ar,&p.dn64_372);
    field(ar,&p.dn64_374);
    field(ar,&p.dn64_376);
    field(ar,&p.dn64_378);
    field(ar,&p.dn64_385);
    field(ar,&p.dn_388);
    field(ar,&p.crouch_toggle);
}

template<class A> void sectorFields(A& ar, usectortype& p)
{
#define F(x) field(ar,&p.x)
    F(wallptr); F(wallnum); F(ceilingz); F(floorz); F(ceilingstat); F(floorstat);
    F(ceilingpicnum); F(ceilingheinum); F(ceilingshade); F(ceilingpal); F(ceilingxpanning); F(ceilingypanning);
    F(floorpicnum); F(floorheinum); F(floorshade); F(floorpal); F(floorxpanning); F(floorypanning);
    F(visibility); F(fogpal); F(lotag); F(hitag); F(extra);
#undef F
}
template<class A> void wallFields(A& ar, uwalltype& p)
{
#define F(x) field(ar,&p.x)
    F(x); F(y); F(point2); F(nextwall); F(nextsector); F(cstat); F(picnum); F(overpicnum);
    F(shade); F(pal); F(xrepeat); F(yrepeat); F(xpanning); F(ypanning); F(lotag); F(hitag); F(extra);
#undef F
}
template<class A> void spriteFields(A& ar, uspritetype& p)
{
#define F(x) field(ar,&p.x)
    F(x); F(y); F(z); F(cstat); F(picnum); F(shade); F(pal); F(clipdist); F(blend);
    F(xrepeat); F(yrepeat); F(xoffset); F(yoffset); F(sectnum); F(statnum); F(ang); F(owner);
    F(xvel); F(yvel); F(zvel); F(lotag); F(hitag); F(extra);
#undef F
}
template<class A> void actorFields(A& ar, actor_t& p)
{
#define F(x) field(ar,&p.x)
    F(t_data); F(flags); F(bpos.x); F(bpos.y); F(bpos.z); F(floorz); F(ceilingz);
    F(lastv.x); F(lastv.y); F(picnum); F(ang); F(extra); F(owner); F(movflag); F(tempang);
    F(timetosleep); F(actorstayput); F(cgg); F(lasttransport); F(dispicnum);
#undef F
}
template<class A> void extFields(A& ar, spriteext_t& p)
{
#define F(x) field(ar,&p.x)
    float alpha; Bmemcpy(&alpha,&p.alpha,4); ar.floating(alpha); Bmemcpy(&p.alpha,&alpha,4); F(flags); F(mdanimcur); F(mdanimtims); F(mdangoff); F(xpanning); F(ypanning);
    F(mdpitch); F(mdroll); F(mdpivot_offset.x); F(mdpivot_offset.y); F(mdpivot_offset.z);
    F(mdposition_offset.x); F(mdposition_offset.y); F(mdposition_offset.z);
#undef F
}

// Gameplay options only. Local video, audio, input, menus and passwords are not
// members of the protocol, even if user_defs grows new fields in the future.
#define OPTION_FIELDS(F) \
    F(multimode) F(volume_number) F(level_number) F(user_map) F(player_skill) \
    F(music_episode) F(music_level) F(from_bonus) F(secretlevel) F(respawn_monsters) \
    F(respawn_items) F(respawn_inventory) F(monsters_off) F(last_level) F(eog) \
    F(coop) F(marker) F(ffire) F(noexits) F(playerai) F(pause_on)

// Only visual/game state read by client rendering. Simulation-only queues and
// animation pointers remain server-local; sectors/walls already contain their
// authoritative result. All array element types here are fixed width.
#define MISC_FIELDS(F) \
    F(g_globalRandom) F(g_gametypeFlags) F(everyothertime) F(g_curViewscreen) F(g_cloudCnt) F(g_cloudSect) \
    F(g_cloudX) F(g_cloudY) F(g_pskyidx) F(g_earthquakeTime) F(g_mirrorCount) F(g_mirrorWall) F(g_mirrorSector) \
    F(g_spriteExtra) F(g_sectorExtra) F(g_shadedSector) F(g_geoSectorCnt) F(g_geoSector) \
    F(g_geoSectorWarp) F(g_geoSectorWarp2) F(g_geoSectorX) F(g_geoSectorY) F(g_geoSectorX2) F(g_geoSectorY2) \
    F(g_fogType) F(g_windTime) F(g_windDir) F(g_thunderFlash) F(g_thunderTime) F(g_winderFlash) F(g_winderTime) \
    F(g_bellTime) F(g_bellSprite) F(g_banjoSong) F(g_chickenWeaponTimer) F(g_RAendLevel) F(g_RAendEpisode)
struct Player
{
    uint8_t playing = 0;
    int32_t color = 0, team = 0;
    uint8_t frags[MAXPLAYERS] = {}, choices[MAX_WEAPONS] = {};
    char name[32] = {};
    DukePlayer_t state = {};
    input_t input = {};
};
struct GameVar
{
    uint16_t id = 0;
    uint8_t kind = 0;
    std::vector<int32_t> values;
};
struct Quote
{
    uint16_t id = 0;
    char text[MAXQUOTELEN] = {};
};
struct World
{
    uint32_t magic = Magic, scriptCRC = 0, seed = 0;
    uint16_t version = Version, sectorCount = 0, wallCount = 0;
    uint16_t spriteLimit = MAXSPRITES, sectorLimit = MAXSECTORS, statusLimit = MAXSTATUS;
    uint16_t varCount = 0, quoteCount = 0;
    uint8_t playerLimit = MAXPLAYERS;
    int32_t gameClock = 0, scriptSize = 0, bunches = 0;
    std::vector<usectortype> sectors;
    std::vector<uwalltype> walls;
    std::vector<uspritetype> sprites;
    std::vector<actor_t> actors;
    std::vector<spriteext_t> extensions;
    std::vector<uint8_t> wallBlend;
    std::vector<int16_t> sectorBunch, wallLink;
    Player players[MAXPLAYERS];
#define DECL_OPTION(x) decltype(ud.x) x = {};
    OPTION_FIELDS(DECL_OPTION)
#undef DECL_OPTION
#define DECL_MISC(x) decltype(::x) x = {};
    MISC_FIELDS(DECL_MISC)
#undef DECL_MISC
    std::vector<GameVar> vars;
    std::vector<Quote> quotes;
};

template<class A> bool archive(A& ar, World& w)
{
    ar(w.magic); ar(w.version); ar(w.spriteLimit); ar(w.sectorLimit); ar(w.statusLimit); ar(w.playerLimit);
    ar(w.scriptCRC); ar(w.scriptSize); ar(w.sectorCount); ar(w.wallCount); ar(w.gameClock); ar(w.seed); ar(w.bunches);
    if (!ar.ok || w.magic != Magic || w.version != Version || w.spriteLimit != MAXSPRITES ||
        w.sectorLimit != MAXSECTORS || w.statusLimit != MAXSTATUS || w.playerLimit != MAXPLAYERS ||
        w.sectorCount == 0 || w.sectorCount > MAXSECTORS || w.wallCount == 0 || w.wallCount > MAXWALLS ||
        w.bunches < 0 || w.bunches > YAX_MAXBUNCHES || w.scriptSize < 0 || w.scriptSize > (1<<24)) return false;
    w.sectors.resize(w.sectorCount); w.walls.resize(w.wallCount); w.sprites.resize(MAXSPRITES);
    w.actors.resize(MAXSPRITES); w.extensions.resize(MAXSPRITES); w.wallBlend.resize(w.wallCount);
    w.sectorBunch.resize(size_t(w.sectorCount)*2); w.wallLink.resize(size_t(w.wallCount)*2);
    for (auto& s : w.sectors) sectorFields(ar, s);
    for (auto& s : w.walls) wallFields(ar, s);
    for (auto& s : w.sprites) spriteFields(ar, s);
    for (auto& s : w.actors) actorFields(ar, s);
    for (auto& s : w.extensions) extFields(ar, s);
    for (auto& s : w.wallBlend) ar(s);
    for (auto& s : w.sectorBunch) ar(s);
    for (auto& s : w.wallLink) ar(s);
    for (auto& p : w.players)
    {
        field(ar,&p.playing); field(ar,&p.color); field(ar,&p.team); field(ar,&p.frags); field(ar,&p.choices); field(ar,&p.name);
        playerFields(ar, p.state);
        field(ar,&p.input.bits); field(ar,&p.input.fvel); field(ar,&p.input.svel); field(ar,&p.input.q16avel); field(ar,&p.input.q16horz); field(ar,&p.input.extbits);
    }
#define FIELD(x) ar(w.x);
    OPTION_FIELDS(FIELD)
    MISC_FIELDS(FIELD)
#undef FIELD
    ar(w.varCount);
    if (!ar.ok || w.varCount > MAXGAMEVARS) return false;
    w.vars.resize(w.varCount);
    for (auto& v : w.vars)
    {
        ar(v.id); ar(v.kind);
        if (!ar.ok || v.id >= MAXGAMEVARS || v.kind > 2) return false;
        size_t count = v.kind == 2 ? MAXSPRITES : v.kind == 1 ? MAXPLAYERS : 1;
        v.values.resize(count);
        for (auto& value : v.values) ar(value);
        if (!ar.ok) return false;
    }
    ar(w.quoteCount);
    if (!ar.ok || w.quoteCount > MAXQUOTES) return false;
    w.quotes.resize(w.quoteCount);
    for (auto& q : w.quotes) { ar(q.id); ar(q.text); }
    return ar.ok;
}

bool index(int value, int limit) { return value >= 0 && value < limit; }
bool optionalIndex(int value, int limit) { return value == -1 || index(value, limit); }
bool tile(int value) { return index(value, MAXTILES); }

bool check(const World& w, std::string& error)
{
#define REQUIRE(test, why) do { if (!(test)) { error = why; return false; } } while (0)
    bool referencedPlayers[MAXPLAYERS] = {};
    std::vector<int16_t> wallSector(w.wallCount, -1);
    for (int i = 0; i < w.sectorCount; ++i)
    {
        const auto& s = w.sectors[i];
        REQUIRE(s.wallptr >= 0 && s.wallnum >= 3 && int(s.wallptr)+s.wallnum <= w.wallCount, "Invalid sector wall range");
        REQUIRE(tile(s.ceilingpicnum) && tile(s.floorpicnum), "Invalid sector tile");
        for (int j = s.wallptr; j < int(s.wallptr)+s.wallnum; ++j)
        {
            REQUIRE(wallSector[j] == -1, "Overlapping sector wall ranges");
            wallSector[j] = int16_t(i);
            REQUIRE(w.walls[j].point2 >= s.wallptr && w.walls[j].point2 < int(s.wallptr)+s.wallnum, "Wall leaves sector loop");
        }
    }
    std::vector<uint8_t> incoming(w.wallCount, 0);
    for (int i = 0; i < w.wallCount; ++i)
    {
        const auto& s = w.walls[i];
        REQUIRE(wallSector[i] != -1 && index(s.point2,w.wallCount), "Unowned wall or invalid wall point");
        REQUIRE(++incoming[s.point2] == 1, "Wall loop is not a cycle");
        REQUIRE(optionalIndex(s.nextwall,w.wallCount) && optionalIndex(s.nextsector,w.sectorCount), "Invalid wall portal");
        REQUIRE((s.nextwall == -1) == (s.nextsector == -1), "Incomplete wall portal");
        if (s.nextwall >= 0)
            REQUIRE(w.walls[s.nextwall].nextwall == i && wallSector[s.nextwall] == s.nextsector &&
                    w.walls[s.nextwall].nextsector == wallSector[i], "Nonreciprocal wall portal");
        REQUIRE(tile(s.picnum) && (s.overpicnum == -1 || tile(s.overpicnum)), "Invalid wall tile");
    }
    for (int i = 0; i < MAXSPRITES; ++i)
    {
        const auto& s = w.sprites[i];
        const auto& a = w.actors[i];
        REQUIRE((s.statnum == MAXSTATUS && s.sectnum == MAXSECTORS) ||
                (index(s.statnum,MAXSTATUS) && index(s.sectnum,w.sectorCount)), "Invalid sprite sector/status");
        if (s.statnum == MAXSTATUS) continue;
        REQUIRE(tile(s.picnum) && (s.statnum == STAT_EFFECTOR || s.owner == -2 || optionalIndex(s.owner,MAXSPRITES)), "Invalid sprite tile/owner");
        REQUIRE(optionalIndex(a.owner,MAXSPRITES) && optionalIndex(a.actorstayput,w.sectorCount), "Invalid actor reference");
        REQUIRE(std::isfinite(w.extensions[i].alpha) && w.extensions[i].alpha >= 0 && w.extensions[i].alpha <= 1,
                "Invalid sprite transparency");
        // t_data are numeric CON offsets, never pointers. Only an actor's action
        // is dereferenced by rendering; special-effect sprites reuse these slots.
        if (s.statnum == STAT_ACTOR || s.statnum == STAT_ZOMBIEACTOR || s.statnum == STAT_PLAYER)
            REQUIRE(a.t_data[4] == 0 || (a.t_data[4] > 0 && a.t_data[4] <= w.scriptSize-ACTION_PARAM_COUNT),
                    "Invalid actor CON action offset");
        if (s.picnum == APLAYER || s.statnum == STAT_PLAYER)
        {
            REQUIRE(index(s.yvel,MAXPLAYERS), "Invalid player sprite slot");
            // Rendering resolves player state through an APLAYER sprite even
            // when that slot is absent from the current connection list.
            referencedPlayers[s.yvel] = true;
        }
    }
    for (int i = 0; i < MAXPLAYERS; ++i)
    {
        const auto& entry = w.players[i];
        const auto& p = entry.state;
        REQUIRE(entry.playing <= 1 && entry.name[31] == '\0', "Invalid player metadata");
        if (!entry.playing && !referencedPlayers[i]) continue;
        REQUIRE(index(p.i,MAXSPRITES) && w.sprites[p.i].statnum < MAXSTATUS &&
                w.sprites[p.i].yvel == i, "Invalid player sprite");
        REQUIRE(index(p.cursectnum,w.sectorCount), "Invalid player sector");
        REQUIRE(index(p.curr_weapon,MAX_WEAPONS) && optionalIndex(p.last_weapon,MAX_WEAPONS) &&
                index(p.inven_icon,ICON_MAX) && index(p.ftq,MAXQUOTES) && p.numloogs >= 0 && p.numloogs <= 64 &&
                p.frag_ps < MAXPLAYERS && p.kickback_pic >= 0,
                "Invalid player HUD state");
        REQUIRE(optionalIndex(p.newowner,MAXSPRITES) && optionalIndex(p.holoduke_on,MAXSPRITES) &&
                optionalIndex(p.dummyplayersprite,MAXSPRITES) && optionalIndex(p.access_spritenum,MAXSPRITES) &&
                optionalIndex(p.access_wallnum,w.wallCount) && optionalIndex(p.on_crane,MAXSPRITES) &&
                optionalIndex(p.wackedbyactor,MAXSPRITES) && optionalIndex(p.somethingonplayer,MAXSPRITES) &&
                optionalIndex(p.actorsqu,MAXSPRITES) && optionalIndex(p.one_parallax_sectnum,w.sectorCount), "Invalid player world reference");
        REQUIRE(entry.color >= 0 && entry.color < MAXPALOOKUPS && entry.team >= 0 && entry.team < MAXPLAYERS,
                "Invalid player color/team");
    }
    REQUIRE(w.multimode >= 1 && w.multimode <= MAXPLAYERS && index(w.coop,MAXGAMETYPES) &&
            index(w.volume_number,MAXVOLUMES) && index(w.level_number,MAXLEVELS), "Invalid game options");
    REQUIRE(w.g_mirrorCount >= 0 && w.g_mirrorCount <= 64 && w.g_cloudCnt >= 0 && w.g_cloudCnt <= 256 &&
            w.g_geoSectorCnt <= MAXGEOSECTORS, "Invalid render collection length");
    REQUIRE(optionalIndex(w.g_curViewscreen,MAXSPRITES), "Invalid viewscreen sprite");
    for (int i = 0; i < w.g_mirrorCount; ++i)
        REQUIRE(index(w.g_mirrorWall[i],w.wallCount) && index(w.g_mirrorSector[i],w.sectorCount), "Invalid mirror");
    for (int i = 0; i < w.g_cloudCnt; ++i) REQUIRE(index(w.g_cloudSect[i],w.sectorCount), "Invalid cloud sector");
    for (uint32_t i = 0; i < w.g_geoSectorCnt; ++i)
        REQUIRE(index(w.g_geoSector[i],w.sectorCount) && optionalIndex(w.g_geoSectorWarp[i],w.sectorCount) &&
                optionalIndex(w.g_geoSectorWarp2[i],w.sectorCount), "Invalid geometry warp");
    for (auto b : w.sectorBunch) REQUIRE(optionalIndex(b,w.bunches), "Invalid TROR bunch");
    for (auto link : w.wallLink) REQUIRE(optionalIndex(link,w.wallCount), "Invalid TROR wall link");
    int lastQuote = -1;
    for (const auto& q : w.quotes)
    {
        REQUIRE(q.id > lastQuote && q.id < MAXQUOTES && q.text[MAXQUOTELEN-1] == '\0', "Invalid CON quote");
        lastQuote = q.id;
    }
    int lastVar = -1;
    for (const auto& v : w.vars)
    {
        REQUIRE(v.id > lastVar, "Duplicate or unordered CON variable");
        lastVar = v.id;
    }
#undef REQUIRE
    return true;
}

bool checkViewpoint(const World& w, int localSlot, std::string& error)
{
    // Live snapshots are sent only to playing clients. Packet recordings also
    // select a playing viewpoint before writing each snapshot. Inactive slots
    // can contain unused state, so they must never become the local camera.
    if (!index(localSlot,MAXPLAYERS) || !w.players[localSlot].playing)
    { error = "Snapshot has no active local player"; return false; }
    return true;
}

bool checkProgram(const World& w, std::string& error)
{
    for (int i = 0; i < MAXSPRITES; ++i)
    {
        const auto& s = w.sprites[i];
        if (s.statnum == MAXSTATUS || !g_tile[s.picnum].execPtr) continue;
        int32_t const action = w.actors[i].t_data[4];
        if (action && (action < 0 || action > g_scriptSize-ACTION_PARAM_COUNT))
        { error = "Snapshot actor action is outside the loaded CON program"; return false; }
    }
    return true;
}

bool decode(const std::vector<uint8_t>& bytes, World& world, std::string& error)
{
    error.clear();
    if (bytes.size() > MaxMessage || bytes.size() < 39) { error = "Invalid snapshot size"; return false; }
    Reader reader{bytes};
    if (!archive(reader,world) || reader.pos != bytes.size())
    { error = "Malformed, truncated, or incompatible snapshot"; return false; }
    return check(world,error);
}

// Build maintains two intrusive lists; the wire has no list pointers. Rebuilding
// each list once from validated membership makes cycles/duplicates impossible.
void rebuildLists(const World& w)
{
    std::fill_n(headspritesect,MAXSECTORS+1,int16_t(-1));
    std::fill_n(headspritestat,MAXSTATUS+1,int16_t(-1));
    Numsprites = 0;
    tailspritefree = -1;
    for (int i = MAXSPRITES-1; i >= 0; --i)
    {
        const auto& s = w.sprites[i];
        int sect = s.sectnum, stat = s.statnum;
        prevspritesect[i] = prevspritestat[i] = -1;
        nextspritesect[i] = headspritesect[sect];
        nextspritestat[i] = headspritestat[stat];
        if (headspritesect[sect] >= 0) prevspritesect[headspritesect[sect]] = int16_t(i);
        if (headspritestat[stat] >= 0) prevspritestat[headspritestat[stat]] = int16_t(i);
        headspritesect[sect] = headspritestat[stat] = int16_t(i);
        if (stat < MAXSTATUS) ++Numsprites;
        else if (tailspritefree == -1) tailspritefree = int16_t(i);
    }
}
} // namespace

bool capture(std::vector<uint8_t>& bytes, std::string& error)
{
    error.clear(); bytes.clear();
    if (REALITY) { error = "Duke64 snapshots are not supported"; return false; }
    if (numsectors <= 0 || numsectors > MAXSECTORS || numwalls <= 0 || numwalls > MAXWALLS)
    { error = "No valid world to capture"; return false; }
    std::unique_ptr<World> ptr(new World);
    World& w = *ptr;
    w.scriptCRC = g_scriptcrc; w.scriptSize = g_scriptSize;
    w.sectorCount = uint16_t(numsectors); w.wallCount = uint16_t(numwalls);
    w.gameClock = int32_t(lockclock); w.seed = randomseed; w.bunches = numyaxbunches;
    w.sectors.resize(numsectors); w.walls.resize(numwalls); w.sprites.resize(MAXSPRITES);
    w.actors.resize(MAXSPRITES); w.extensions.resize(MAXSPRITES); w.wallBlend.resize(numwalls);
    w.sectorBunch.resize(size_t(numsectors)*2); w.wallLink.resize(size_t(numwalls)*2);
    // memcpy here only copies local engine representations into untracked local
    // structures. The encoder above serializes every wire field individually.
    Bmemcpy(w.sectors.data(),sector,sizeof(usectortype)*numsectors);
    Bmemcpy(w.walls.data(),wall,sizeof(uwalltype)*numwalls);
    Bmemcpy(w.sprites.data(),sprite,sizeof(uspritetype)*MAXSPRITES);
    Bmemcpy(w.actors.data(),actor,sizeof(actor_t)*MAXSPRITES);
    Bmemcpy(w.extensions.data(),spriteext,sizeof(spriteext_t)*MAXSPRITES);
    for (auto& ext : w.extensions)
    {
        // Model animation clocks and current interpolation frames are local
        // renderer caches, not the server's canonical actor animation state.
        ext.mdanimcur = 0; ext.mdanimtims = 0; ext.tspr = nullptr;
    }
    for (int i = 0; i < numwalls; ++i)
    {
#ifndef NEW_MAP_FORMAT
        w.wallBlend[i] = wallext[i].blend;
#else
        w.wallBlend[i] = wall[i].blend;
#endif
        for (int cf = 0; cf < 2; ++cf) w.wallLink[i*2+cf] = yax_getnextwall(i,cf);
    }
    for (int i = 0; i < numsectors; ++i)
        for (int cf = 0; cf < 2; ++cf) w.sectorBunch[i*2+cf] = yax_getbunch(i,cf);
    for (int i = 0; i < MAXPLAYERS; ++i)
    {
        Player& p = w.players[i];
        p.playing = playing(i) ? 1 : 0;
        p.color = g_player[i].pcolor; p.team = g_player[i].pteam;
        Bmemcpy(p.frags,g_player[i].frags,sizeof(p.frags));
        Bmemcpy(p.choices,g_player[i].wchoice,sizeof(p.choices));
        Bmemcpy(p.name,g_player[i].user_name,sizeof(p.name)); p.name[31] = '\0';
        if (g_player[i].ps) p.state = *g_player[i].ps;
        if (g_player[i].inputBits) p.input = *g_player[i].inputBits;
    }
#define COPY_OPTION(x) w.x = ud.x;
    OPTION_FIELDS(COPY_OPTION)
#undef COPY_OPTION
#define COPY_MISC(x) Bmemcpy(&w.x,&::x,sizeof(w.x));
    MISC_FIELDS(COPY_MISC)
#undef COPY_MISC
    for (int i = 0; i < g_gameVarCount; ++i)
    {
        const auto& var = aGameVars[i];
        if (var.flags & NoMultiVar) continue;
        GameVar v; v.id = uint16_t(i);
        v.kind = (var.flags & GAMEVAR_PERACTOR) ? 2 : (var.flags & GAMEVAR_PERPLAYER) ? 1 : 0;
        size_t count = v.kind == 2 ? MAXSPRITES : v.kind == 1 ? MAXPLAYERS : 1;
        if (v.kind && !var.pValues) { error = "Unallocated CON variable"; return false; }
        v.values.resize(count);
        for (size_t j = 0; j < count; ++j) v.values[j] = int32_t(v.kind ? var.pValues[j] : var.global);
        w.vars.push_back(std::move(v));
    }
    w.varCount = uint16_t(w.vars.size());
    for (int i = 0; i < MAXQUOTES; ++i)
    {
        if (!apStrings[i]) continue;
        Quote q; q.id = uint16_t(i);
        Bstrncpy(q.text,apStrings[i],MAXQUOTELEN-1);
        w.quotes.push_back(q);
    }
    w.quoteCount = uint16_t(w.quotes.size());
    if (!check(w,error) || !checkProgram(w,error)) return false;
    Writer writer{bytes};
    if (!archive(writer,w)) { bytes.clear(); error = "Snapshot exceeds network limit"; return false; }
    return true;
}

bool validate(const std::vector<uint8_t>& bytes, std::string& error)
{
    std::unique_ptr<World> world(new World);
    return decode(bytes,*world,error);
}

bool apply(const std::vector<uint8_t>& bytes, int localSlot, std::string& error)
{
    std::unique_ptr<World> ptr(new World);
    World& w = *ptr;
    if (!decode(bytes,w,error)) return false;
    if (!checkViewpoint(w,localSlot,error)) return false;
    if (REALITY) { error = "Unsupported snapshot game"; return false; }
    if (w.scriptCRC != g_scriptcrc || w.scriptSize != g_scriptSize)
    { error = "Snapshot CON program differs from loaded content"; return false; }
    if (!checkProgram(w,error)) return false;
    for (const auto& v : w.vars)
    {
        if (v.id >= g_gameVarCount) { error = "Snapshot CON variable not present locally"; return false; }
        const auto& var = aGameVars[v.id];
        unsigned kind = (var.flags & GAMEVAR_PERACTOR) ? 2 : (var.flags & GAMEVAR_PERPLAYER) ? 1 : 0;
        if ((var.flags & NoMultiVar) || kind != v.kind || (kind && !var.pValues))
        { error = "Snapshot CON variable layout differs from loaded content"; return false; }
    }
    // No operation above modifies live state. All references have now passed
    // validation; only trusted allocations and copies occur below this point.
    G_MaybeAllocPlayer(localSlot);
    DukePlayer_t local = *g_player[localSlot].ps;
    uint8_t const previousFog = g_fogType;
    numsectors = int16_t(w.sectorCount); numwalls = int16_t(w.wallCount);
    for (int i = 0; i < numsectors; ++i)
    {
#ifdef USE_STRUCT_TRACKERS
        if (Bmemcmp(&sector[i],&w.sectors[i],sizeof(usectortype))) ++sectorchanged[i];
#endif
        Bmemcpy(&sector[i],&w.sectors[i],sizeof(usectortype));
    }
    for (int i = 0; i < numwalls; ++i)
    {
#ifdef USE_STRUCT_TRACKERS
        if (Bmemcmp(&wall[i],&w.walls[i],sizeof(uwalltype))) ++wallchanged[i];
#endif
        Bmemcpy(&wall[i],&w.walls[i],sizeof(uwalltype));
    }
    for (int i = 0; i < MAXSPRITES; ++i)
    {
        bool const sameSprite = sprite[i].statnum < MAXSTATUS && w.sprites[i].statnum < MAXSTATUS &&
            sprite[i].picnum == w.sprites[i].picnum && sprite[i].owner == w.sprites[i].owner;
        if (sameSprite)
        {
            w.extensions[i].mdanimcur = spriteext[i].mdanimcur;
            w.extensions[i].mdanimtims = spriteext[i].mdanimtims;
        }
#ifdef USE_STRUCT_TRACKERS
        if (Bmemcmp(&sprite[i],&w.sprites[i],sizeof(uspritetype))) ++spritechanged[i];
#endif
        Bmemcpy(&sprite[i],&w.sprites[i],sizeof(uspritetype));
#ifdef POLYMER
        // GPU light handles belong to this renderer, not the remote process.
        w.actors[i].lightptr = actor[i].lightptr;
        w.actors[i].lightId = actor[i].lightId;
        w.actors[i].lightcount = actor[i].lightcount;
        w.actors[i].lightmaxrange = actor[i].lightmaxrange;
#endif
        actor[i] = w.actors[i];
        w.extensions[i].tspr = nullptr;
        spriteext[i] = w.extensions[i];
    }
    rebuildLists(w);
#ifndef NEW_MAP_FORMAT
    Bmemcpy(yax_bunchnum,w.sectorBunch.data(),numsectors*2*sizeof(int16_t));
    Bmemcpy(yax_nextwall,w.wallLink.data(),numwalls*2*sizeof(int16_t));
    for (int i = 0; i < numwalls; ++i) wallext[i].blend = w.wallBlend[i];
#else
    for (int i = 0; i < numsectors; ++i)
    {
        sector[i].ceilingbunch = w.sectorBunch[2*i];
        sector[i].floorbunch = w.sectorBunch[2*i+1];
    }
    for (int i = 0; i < numwalls; ++i)
    {
        wall[i].upwall = w.wallLink[2*i]; wall[i].dnwall = w.wallLink[2*i+1];
        wall[i].blend = w.wallBlend[i];
    }
#endif
    numyaxbunches = w.bunches;
    yax_update(w.bunches > 0 ? 2 : 1);
    int tail = -1, count = 0;
    connecthead = -1;
    std::fill_n(connectpoint2,MAXPLAYERS,int16_t(-1));
    for (int i = 0; i < MAXPLAYERS; ++i)
    {
        G_MaybeAllocPlayer(i);
        const auto& p = w.players[i];
        DukePlayer_t previous = *g_player[i].ps;
        *g_player[i].ps = p.state;
        // Preserve a full frame of previous authoritative camera position for
        // the engine's existing interpolation without running client gameplay.
        if (p.playing && previous.i == p.state.i && previous.cursectnum >= 0)
        {
            g_player[i].ps->opos = previous.pos;
            g_player[i].ps->oq16ang = previous.q16ang;
            g_player[i].ps->oq16horiz = previous.q16horiz;
            g_player[i].ps->oq16horizoff = previous.q16horizoff;
        }
        *g_player[i].inputBits = p.input;
        g_player[i].pcolor = p.color; g_player[i].pteam = p.team;
        Bmemcpy(g_player[i].frags,p.frags,sizeof(p.frags));
        Bmemcpy(g_player[i].wchoice,p.choices,sizeof(p.choices));
        Bmemcpy(g_player[i].user_name,p.name,sizeof(p.name));
        g_player[i].connected = p.playing;
        g_player[i].playerquitflag = p.playing;
        if (!p.playing) continue;
        if (tail >= 0) connectpoint2[tail] = int16_t(i); else connecthead = int16_t(i);
        tail = i; ++count;
    }
    if (connecthead < 0) connecthead = int16_t(localSlot);
    numplayers = int16_t(std::max(1,count));
    // Slots remain stable when peers leave. CON's player bound therefore must
    // cover all possible slots, including a lone client assigned above zero.
    g_mostConcurrentPlayers = MAXPLAYERS;
    myconnectindex = int16_t(localSlot);
    if (!index(screenpeek,MAXPLAYERS) || !w.players[screenpeek].playing) screenpeek = localSlot;
    auto& ps = *g_player[localSlot].ps;
    constexpr unsigned localModes = MODE_MENU | MODE_TYPE | MODE_SENDTOWHOM;
    ps.gm = (ps.gm & ~localModes) | (local.gm & localModes) | MODE_GAME;
    ps.aim_mode = local.aim_mode; ps.weaponswitch = local.weaponswitch;
    ps.crouch_toggle = local.crouch_toggle;
    ps.over_shoulder_on = rules::canChase() ? local.over_shoulder_on : 0;
#define APPLY_OPTION(x) ud.x = w.x;
    OPTION_FIELDS(APPLY_OPTION)
#undef APPLY_OPTION
#define APPLY_MISC(x) Bmemcpy(&::x,&w.x,sizeof(w.x));
    MISC_FIELDS(APPLY_MISC)
#undef APPLY_MISC
    // RRRA fog swaps palette lookup tables as well as changing the visibility
    // flag. Clients do not execute the actor that normally performs that swap.
    if (RRRA && previousFog != g_fogType) G_SetFog(g_fogType ? 2 : 0);
    randomseed = w.seed;
    lockclock = w.gameClock;
    // Keep the local real-time clock monotonic; interpolation starts when a
    // snapshot arrives rather than jumping the input/network pump clock.
    ototalclock = totalclock;
    g_animateCnt = 0;
    g_interpolationCnt = 0;
    for (const auto& v : w.vars)
    {
        auto& var = aGameVars[v.id];
        if (v.kind) for (size_t j = 0; j < v.values.size(); ++j) var.pValues[j] = v.values[j];
        else var.global = v.values[0];
    }
    for (const auto& q : w.quotes)
    {
        if (!apStrings[q.id]) apStrings[q.id] = (char*)Xcalloc(MAXQUOTELEN,1);
        Bmemcpy(apStrings[q.id],q.text,MAXQUOTELEN);
    }
    resetPrediction();
    P_UpdateScreenPal(g_player[screenpeek].ps);
    return true;
}

void resetPrediction()
{
    if (!index(myconnectindex,MAXPLAYERS) || !g_player[myconnectindex].ps) return;
    const auto& p = *g_player[myconnectindex].ps;
    mypos = p.pos; omypos = p.opos; myvel = p.vel;
    myang = p.q16ang; omyang = p.oq16ang; myhoriz = p.q16horiz; omyhoriz = p.oq16horiz;
    myhorizoff = p.q16horizoff; omyhorizoff = p.oq16horizoff; mycursectnum = p.cursectnum;
    myjumpingcounter = p.jumping_counter; myjumpingtoggle = p.jumping_toggle;
    myonground = p.on_ground; myhardlanding = p.hard_landing; myreturntocenter = p.return_to_center;
    my_moto_speed = p.moto_speed; my_not_on_water = p.not_on_water; my_moto_on_ground = p.moto_on_ground;
    my_moto_do_bump = p.moto_do_bump; my_moto_bump_fast = p.moto_bump_fast;
    my_moto_on_oil = p.moto_on_oil; my_moto_on_mud = p.moto_on_mud;
    my_moto_bump = p.moto_bump; my_moto_bump_target = p.moto_bump_target; my_moto_turb = p.moto_turb;
    my_stairs = p.stairs;
}
} // namespace rnet
