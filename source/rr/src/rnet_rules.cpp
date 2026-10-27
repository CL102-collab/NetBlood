// SPDX-License-Identifier: GPL-2.0-or-later
#include "duke3d.h"
#include "premap.h"
#include "rnet.h"
#include "rnet_rules.h"
#include "rnet_rules_model.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>

namespace rnet { namespace rules {
namespace {
enum : uint8_t { State = RulesFirst, Cinematic, CinematicDone };
using namespace rulemodel;
struct Settings
{
    int mode = 0, friendlyFire = 0, fragLimit = 0, timeLimit = 0, difficulty = 2;
    int autoAim = 1, weaponStay = 1, items = 0, monsters = 1, inventory = 0;
    int keys = 0, holster = 0, chase = 0, lifeLimit = 4;
} settings;
std::array<Life, MAXPLAYERS> life;
std::array<Score, MAXPLAYERS> scores;
struct CarriedInventory
{
    bool pending=false;
    uint32_t weapons=0;
    std::array<int16_t,MAX_WEAPONS> ammo{};
    std::array<int16_t,GET_MAX> inventory{};
    int16_t health=0, subweapon=0;
    int8_t weapon=0;
    uint8_t icon=0;
};
std::array<CarriedInventory,MAXPLAYERS> carried;
std::array<std::string, MAXPLAYERS> identities;
std::map<std::string, Life> departed;
std::vector<std::string> maps;
size_t mapIndex = 0;
uint32_t startedAt = 0, finishAt = 0, lastStateTick = 0;
bool ending = false, restartAfter = false, watching = false, pendingCinema = false;
bool cinemaRunning = false, levelExit = false;
bool introPending = false;
uint32_t cinemaSerial = 0;
std::array<bool, MAXPLAYERS> cinemaPending;
int cinematicVolume = 0, cinematicEog = 0, cinematicLevel = 0;
std::array<int, MAXGAMETYPES> originalFlags;
bool flagsSaved = false;
bool monstersSet = false;
std::array<int, 16> originalOptions;

bool validPlayer(int n) { return unsigned(n) < MAXPLAYERS && g_player[n].ps; }
bool finiteLives() { return active() && coop() && settings.lifeLimit > 0 && (dedicated() || client() || playback()); }
bool usable(int n)
{
    return validPlayer(n) && playing(n) && unsigned(g_player[n].ps->i) < MAXSPRITES
        && unsigned(g_player[n].ps->cursectnum) < unsigned(numsectors);
}
void put16(std::vector<uint8_t>& out, unsigned n) { out.push_back(n); out.push_back(n >> 8); }
void put32(std::vector<uint8_t>& out, uint32_t n) { for (int i=0;i<4;++i) out.push_back(n >> (i*8)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
void announce(const char* text)
{
    initprintf("NET rules: %s\n", text);
    if (!dedicated()) G_AddUserQuote(text);
}
std::vector<uint8_t> encodeState()
{
    std::vector<uint8_t> out;
    put32(out, mapSerial());
    out.push_back(settings.mode); out.push_back(settings.friendlyFire);
    put16(out, settings.fragLimit); put16(out, settings.timeLimit);
    out.push_back(settings.difficulty); out.push_back(settings.autoAim);
    out.push_back(settings.weaponStay); out.push_back(settings.items);
    out.push_back(settings.monsters); out.push_back(settings.inventory);
    out.push_back(settings.keys); out.push_back(canHolster()); out.push_back(canChase());
    out.push_back(finiteLives() ? settings.lifeLimit : 0);
    out.push_back(ending); out.push_back(watching);
    put32(out, ending && finishAt > rnet::tick() ? finishAt-rnet::tick() : 0);
    for (int i=0;i<MAXPLAYERS;++i)
    {
        out.push_back(life[i].remaining); out.push_back(life[i].state);
        out.push_back(giveUpProgress(i));
    }
    return out;
}
void state(int target = -1)
{
    if (!server()) return;
    send(target, State, encodeState());
    lastStateTick = rnet::tick();
}
bool integer(const char* text, int lo, int hi, int& value)
{
    char* end = nullptr;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || !*text || *end || n < lo || n > hi) return false;
    value = int(n); return true;
}
bool boolean(const char* text, int& value)
{
    if (!Bstrcasecmp(text,"on") || !Bstrcasecmp(text,"yes") || !strcmp(text,"1")) { value=1; return true; }
    if (!Bstrcasecmp(text,"off") || !Bstrcasecmp(text,"no") || !strcmp(text,"0")) { value=0; return true; }
    return false;
}
std::string identity(int n)
{
    std::string ip = address(n);
    return ip + "\n" + g_player[n].user_name;
}
void stopBody(int n)
{
    if (!usable(n)) return;
    auto* p = g_player[n].ps;
    p->vel.x = p->vel.y = p->vel.z = 0;
    p->kickback_pic = p->quick_kick = p->knee_incs = 0;
    p->extra_extra8 = 0;
    actor[p->i].extra = -1;
    if (life[n].state == Down)
    {
        sprite[p->i].extra = p->last_extra = 1;
        sprite[p->i].yrepeat = std::max(1, int(life[n].standingHeight)/2);
        sprite[p->i].cstat = (sprite[p->i].cstat & ~32768) | 257;
        p->dead_flag = 0;
    }
    else if (life[n].state == Out)
    {
        sprite[p->i].extra = p->last_extra = 0;
        sprite[p->i].cstat = 32768;
    }
}
void restoreInventory(int n)
{
    if (!usable(n) || !carried[n].pending) return;
    auto& saved=carried[n]; auto* p=g_player[n].ps;
    p->gotweapon=saved.weapons;
    std::copy(saved.ammo.begin(),saved.ammo.end(),p->ammo_amount);
    std::copy(saved.inventory.begin(),saved.inventory.end(),p->inv_amount);
    // Keys belong to the completed level, unlike equipment and health.
    p->got_access=0;
    Bmemset(p->keys,0,sizeof(p->keys));
    p->curr_weapon=saved.weapon; p->subweapon=saved.subweapon; p->inven_icon=saved.icon;
    sprite[p->i].extra=p->last_extra=saved.health>0?saved.health:p->max_player_health;
    p->last_weapon=-1; p->weapon_pos=WEAPON_POS_RAISE;
    saved.pending=false;
}
void completeCinema()
{
    if (!server() || !watching) return;
    for (int i=0;i<MAXPLAYERS;++i)
        if (connected(i) && cinemaPending[i]) return;
    watching = false;
    state();
    if (levelExit) { levelExit=false; nextMap(); }
}
std::string episodeNext()
{
    int volume = ud.volume_number, level = ud.level_number;
    // The native exit has already selected the next level. If no exit did,
    // advance here (time limits can also end co-op maps).
    const std::string current = mapName();
    if (unsigned(volume) < MAXVOLUMES && unsigned(level) < MAXLEVELS)
    {
        const char* filename = g_mapInfo[volume*MAXLEVELS+level].filename;
        if (filename && *filename)
        {
            char name[32]; Bsnprintf(name,sizeof(name),"E%dL%d",volume+1,level+1);
            if (current != name && current != filename) return name;
        }
    }
    for (int tries=0; tries<MAXVOLUMES*MAXLEVELS; ++tries)
    {
        if (++level >= MAXLEVELS) { level=0; volume=(volume+1)%MAXVOLUMES; }
        const char* filename=g_mapInfo[volume*MAXLEVELS+level].filename;
        if (filename && *filename) { char name[32]; Bsnprintf(name,sizeof(name),"E%dL%d",volume+1,level+1); return name; }
    }
    return current;
}
bool sameMap(const std::string& entry, const std::string& current)
{
    auto equal=[](const char* a,const char* b) {
        while (*a=='/' || *a=='\\') ++a;
        while (*b=='/' || *b=='\\') ++b;
        return !Bstrcasecmp(a,b);
    };
    if (equal(entry.c_str(),current.c_str())) return true;
    int volume=0,level=0,n=0;
    if ((sscanf(entry.c_str(),"E%dL%d%n",&volume,&level,&n)==2
         || sscanf(entry.c_str(),"e%dl%d%n",&volume,&level,&n)==2) && size_t(n)==entry.size()
        && volume>=1 && volume<=MAXVOLUMES && level>=1 && level<=MAXLEVELS)
    {
        const char* filename=g_mapInfo[(volume-1)*MAXLEVELS+level-1].filename;
        return filename && equal(filename,current.c_str());
    }
    return false;
}
void resetLifeLimit()
{
    // An explicit operator change starts fresh budgets. Restore kneeling or
    // eliminated bodies in place, preserving all weapons, inventory and keys.
    for (int n=0;n<MAXPLAYERS;++n)
    {
        Life previous=life[n];
        life[n]=Life(); life[n].remaining=settings.lifeLimit;
        if (!usable(n) || previous.state==Alive) continue;
        auto* p=g_player[n].ps;
        auto& body=sprite[p->i];
        body.yrepeat=std::max(1,int(previous.standingHeight));
        body.cstat=(body.cstat & ~32768) | 257;
        body.extra=p->last_extra=std::min(50,p->max_player_health);
        p->dead_flag=0; p->hurt_delay=30; p->extra_extra8=0;
        p->frag_ps=n; p->wackedbyactor=-1;
        p->vel.x=p->vel.y=p->vel.z=0;
        p->kickback_pic=p->quick_kick=p->knee_incs=0;
        actor[p->i].extra=-1;
    }
    departed.clear();
}
int command(osdcmdptr_t parm)
{
    if (!server()) { OSD_Printf("This command requires a server.\n"); return OSDCMD_OK; }
    const char* name=parm->name;
    if (!Bstrcasecmp(name,"nextmap")) { nextMap(); return OSDCMD_OK; }
    if (!Bstrcasecmp(name,"restartmap")) { queueMap(mapName(),true); return OSDCMD_OK; }
    if (!Bstrcasecmp(name,"maplist"))
    {
        for (size_t i=0;i<maps.size();++i) OSD_Printf("%u: %s%s\n",unsigned(i+1),maps[i].c_str(),i==mapIndex?" *":"");
        return OSDCMD_OK;
    }
    if (parm->numparms != 2) return OSDCMD_SHOWHELP;
    std::string option="-"; option+=parm->parms[0];
    const char* argv[]={"server",option.c_str(),parm->parms[1]}; int index=1;
    int const oldLimit=settings.lifeLimit;
    if (!parseArgument(3,argv,index)) return OSDCMD_SHOWHELP;
    applySettings();
    if (oldLimit!=settings.lifeLimit) resetLifeLimit();
    state(); return OSDCMD_OK;
}
}

bool coop() { return settings.mode == 0; }
bool teams() { return settings.mode == 2; }
bool intermission() { return active() && ending; }
bool frozen() { return active() && (ending || watching); }
unsigned remainingSeconds() { return ending && finishAt>rnet::tick() ? (finishAt-rnet::tick()+TickRate-1)/TickRate : 0; }
int lives(int n) { return finiteLives() && validPlayer(n) ? life[n].remaining : -1; }
int32_t score(int n) { return validPlayer(n) ? scores[n].total : 0; }
std::vector<uint8_t> demoState() { return encodeState(); }
bool downed(int n) { return finiteLives() && validPlayer(n) && life[n].state==Down; }
bool spectating(int n) { return finiteLives() && validPlayer(n) && life[n].state==Out; }
unsigned giveUpProgress(int n) { return validPlayer(n) ? std::min(100u,life[n].hold*100u/(3u*TickRate)) : 0; }
bool canHolster() { return !active() || coop() || teams() || (!dedicated() && !client() && !playback()) || settings.holster; }
bool canChase() { return !active() || coop() || teams() || (!dedicated() && !client() && !playback()) || settings.chase; }
bool keepKeys() { return active() && coop() && settings.keys; }
const std::vector<std::string>& rotation() { return maps; }

static bool weaponTile(int tile)
{
    int const mapped = DYNAMICTILEMAP(tile);
    return (mapped >= FIRSTGUNSPRITE__STATIC && mapped <= DEVISTATORSPRITE__STATIC)
        || mapped == AMMOBOX__STATIC || mapped == GROWSPRITEICON__STATIC || mapped == FREEZEAMMO__STATIC
        || (mapped >= AMMO__STATIC && mapped <= SHOTGUNAMMO__STATIC)
        || (RRRA && (tile == MOTOAMMO || tile == BOATAMMO));
}
bool respawnSprite(int n)
{
    if (unsigned(n) >= MAXSPRITES) return false;
    if (A_CheckEnemySprite(&sprite[n])) return settings.monsters == 2;
    if (weaponTile(sprite[n].picnum)) return settings.weaponStay >= 2;
    if (A_CheckInventorySprite(&sprite[n])) return settings.inventory || settings.items;
    return settings.items != 0;
}
bool respawnMarker(int n)
{
    if (!active()) return true;
    if (unsigned(n) >= MAXSPRITES) return false;
    return weaponTile(sprite[n].picnum) ? settings.weaponStay == 3 : settings.items == 2;
}

void applySettings()
{
    if (!active()) return;
    if (!flagsSaved)
    {
        for (int i=0;i<MAXGAMETYPES;++i) originalFlags[i]=g_gametypeFlags[i];
        originalOptions={{ud.coop,ud.m_coop,ud.ffire,ud.m_ffire,ud.player_skill,ud.m_player_skill,
            ud.respawn_monsters,ud.m_respawn_monsters,ud.respawn_items,ud.m_respawn_items,
            ud.respawn_inventory,ud.m_respawn_inventory,ud.monsters_off,ud.m_monsters_off,ud.marker,ud.m_marker}};
        flagsSaved=true;
    }
    int flags=GAMETYPE_SCORESHEET;
    if (coop()) flags |= GAMETYPE_COOP | GAMETYPE_COOPSPAWN | GAMETYPE_COOPVIEW | GAMETYPE_COOPSOUND | GAMETYPE_FIXEDRESPAWN
        | GAMETYPE_OTHERPLAYERSINMAP | GAMETYPE_PLAYERSFRIENDLY | GAMETYPE_ACCESSCARDSPRITES | GAMETYPE_PRESERVEINVENTORYDEATH;
    else flags |= GAMETYPE_FRAGBAR | GAMETYPE_DMSWITCHES | GAMETYPE_ACCESSATSTART;
    if (teams()) flags |= GAMETYPE_TDM | GAMETYPE_TDMSPAWN;
    if (settings.weaponStay == 1) flags |= GAMETYPE_WEAPSTAY;
    if (settings.items) flags |= GAMETYPE_ITEMRESPAWN;
    g_gametypeFlags[MAXGAMETYPES-1]=flags;
    ud.coop=ud.m_coop=MAXGAMETYPES-1;
    ud.ffire=ud.m_ffire=settings.friendlyFire;
    ud.player_skill=ud.m_player_skill=settings.difficulty;
    ud.respawn_monsters=ud.m_respawn_monsters=settings.monsters == 2;
    ud.monsters_off=ud.m_monsters_off=settings.monsters == 0;
    ud.respawn_items=ud.m_respawn_items=(settings.items || settings.weaponStay >= 2);
    ud.respawn_inventory=ud.m_respawn_inventory=(settings.inventory || settings.items);
    ud.marker=ud.m_marker=settings.items == 2 || settings.weaponStay == 3;
    for (int i=0;i<MAXPLAYERS;++i)
        if (validPlayer(i)) { g_player[i].ps->auto_aim=settings.autoAim; if (!canChase()) g_player[i].ps->over_shoulder_on=0; }
}
void startup() { applySettings(); }
void useMenuSettings()
{
    int const flags = g_gametypeFlags[clamp(ud.m_coop, 0, MAXGAMETYPES - 1)];
    settings.mode = flags & GAMETYPE_COOP ? 0 : flags & GAMETYPE_TDM ? 2 : 1;
    settings.difficulty = clamp(ud.m_player_skill, 0, 4);
    settings.friendlyFire = ud.m_ffire != 0;
    settings.monsters = ud.m_monsters_off ? 0 : ud.m_respawn_monsters ? 2 : 1;
    settings.weaponStay = flags & GAMETYPE_WEAPSTAY ? 1 : 0;
    settings.items = ud.m_respawn_items ? 1 : 0;
    settings.inventory = ud.m_respawn_inventory ? 1 : 0;
}
void mapChanging(bool restart)
{
    for (int n=0;n<MAXPLAYERS;++n)
    {
        auto& saved=carried[n]; saved.pending=false;
        if (restart || !server() || !coop() || !usable(n)) continue;
        auto* p=g_player[n].ps;
        saved.pending=true; saved.weapons=p->gotweapon;
        std::copy(p->ammo_amount,p->ammo_amount+MAX_WEAPONS,saved.ammo.begin());
        std::copy(p->inv_amount,p->inv_amount+GET_MAX,saved.inventory.begin());
        saved.weapon=p->curr_weapon; saved.subweapon=p->subweapon; saved.icon=p->inven_icon;
        saved.health=downed(n)||spectating(n)?p->max_player_health:sprite[p->i].extra;
    }
}
void shutdown()
{
    if (flagsSaved)
    {
        for (int i=0;i<MAXGAMETYPES;++i) g_gametypeFlags[i]=originalFlags[i];
        ud.coop=originalOptions[0]; ud.m_coop=originalOptions[1];
        ud.ffire=originalOptions[2]; ud.m_ffire=originalOptions[3];
        ud.player_skill=originalOptions[4]; ud.m_player_skill=originalOptions[5];
        ud.respawn_monsters=originalOptions[6]; ud.m_respawn_monsters=originalOptions[7];
        ud.respawn_items=originalOptions[8]; ud.m_respawn_items=originalOptions[9];
        ud.respawn_inventory=originalOptions[10]; ud.m_respawn_inventory=originalOptions[11];
        ud.monsters_off=originalOptions[12]; ud.m_monsters_off=originalOptions[13];
        ud.marker=originalOptions[14]; ud.m_marker=originalOptions[15];
    }
    flagsSaved=false; ending=watching=pendingCinema=levelExit=introPending=false; departed.clear();
    scores.fill(Score());
    for (auto& saved:carried) saved.pending=false;
}
void mapStart()
{
    applySettings(); startedAt=rnet::tick(); ending=watching=pendingCinema=levelExit=false;
    introPending=coop() && !RR && !REALITY && !DEER && ud.volume_number==3 && ud.level_number==0 && !ud.lockout;
    cinemaPending.fill(false); departed.clear();
    scores.fill(Score());
    for (int i=0;i<MAXPLAYERS;++i) { life[i]=Life(); life[i].remaining=settings.lifeLimit; identities[i].clear(); }
    for (size_t i=0;i<maps.size();++i) if (sameMap(maps[i],mapName())) { mapIndex=i; break; }
    for (int i=0;i<MAXPLAYERS;++i) if (connected(i) && validPlayer(i)) identities[i]=identity(i);
    state();
}
void playerJoined(int n)
{
    if (!server() || !validPlayer(n)) return;
    std::string const key=identity(n);
    if (identities[n]==key)
    {
        restoreInventory(n); stopBody(n); applySettings(); state(n);
        return;
    }
    life[n]=Life(); life[n].remaining=settings.lifeLimit;
    scores[n]=Score(); g_player[n].ps->frag=g_player[n].ps->fraggedself=0;
    identities[n]=key;
    auto saved=departed.find(identities[n]);
    if (saved!=departed.end()) { life[n]=saved->second; life[n].hold=0; life[n].useWasDown=false; departed.erase(saved); }
    restoreInventory(n); stopBody(n); applySettings();
    // A hot-joiner does not add a new obligation to a cinematic already playing.
    cinemaPending[n]=false;
    state(n);
}
void playerLeft(int n)
{
    if (!server() || !validPlayer(n)) return;
    if (finiteLives() && !identities[n].empty()) departed[identities[n]]=life[n];
    identities[n].clear();
    carried[n].pending=false;
    cinemaPending[n]=false;
    completeCinema(); state();
}
bool death(int n)
{
    if (!finiteLives() || !validPlayer(n)) return false;
    if (life[n].state==Dead) return false;
    if (server() && life[n].state==Alive)
    {
        knockDown(life[n],sprite[g_player[n].ps->i].yrepeat);
        stopBody(n); state();
        char line[128]; Bsnprintf(line,sizeof(line),"%s is down! Use nearby to revive.",g_player[n].user_name); announce(line);
    }
    return true;
}
bool allowRespawn(int n)
{
    return !finiteLives() || (validPlayer(n) && canRespawn(life[n]));
}
void respawned(int n)
{
    if (!finiteLives() || !validPlayer(n)) return;
    life[n].state=Alive; life[n].hold=0; life[n].useWasDown=false; state();
}
bool damageAllowed(int victim,int attacker)
{
    if (!active()) return true;
    if (frozen() || downed(victim) || spectating(victim)) return false;
    if (victim==attacker || !validPlayer(attacker) || !validPlayer(victim) || settings.friendlyFire) return true;
    return !coop() && (!teams() || g_player[victim].ps->team != g_player[attacker].ps->team);
}
void beginIntermission(bool restart)
{
    if (!server() || ending) return;
    ending=true; restartAfter=restart; finishAt=rnet::tick()+15u*TickRate; state();
    announce(restart?"Team eliminated. Restarting in 15 seconds.":"Match complete. Next map in 15 seconds.");
}
void nextMap()
{
    if (!server()) return;
    ending=watching=levelExit=false;
    if (!maps.empty()) { mapIndex=(mapIndex+1)%maps.size(); queueMap(maps[mapIndex]); }
    else if (coop()) queueMap(episodeNext());
    else queueMap(mapName(),true);
}
void shuffleRotation()
{
    if (!server() || maps.empty()) return;
    std::mt19937 generator(std::random_device{}());
    std::shuffle(maps.begin(),maps.end(),generator); mapIndex=maps.size()-1;
}
bool validMap(const std::string& map)
{
    if (map.empty() || map.size()>240 || map.find("..")!=std::string::npos || map[0]=='/' || map[0]=='\\' || map.find(':')!=std::string::npos) return false;
    if (!maps.empty() && std::find(maps.begin(),maps.end(),map)==maps.end()) return false;
    int volume=0,level=0,n=0;
    if ((sscanf(map.c_str(),"E%dL%d%n",&volume,&level,&n)==2 || sscanf(map.c_str(),"e%dl%d%n",&volume,&level,&n)==2) && size_t(n)==map.size())
        return volume>=1 && volume<=MAXVOLUMES && level>=1 && level<=MAXLEVELS && g_mapInfo[(volume-1)*MAXLEVELS+level-1].filename;
    if (map.size()<5 || Bstrcasecmp(map.c_str()+map.size()-4,".map")) return false;
    int handle=kopen4load(map.c_str(),0); if (handle<0) return false; kclose(handle); return true;
}

void cutsceneBegin()
{
    if (!server() || watching) return;
    watching=true; cinemaSerial=mapSerial();
    cinematicVolume=ud.volume_number; cinematicEog=ud.eog; cinematicLevel=ud.level_number;
    for (int i=0;i<MAXPLAYERS;++i) cinemaPending[i]=playing(i);
    pendingCinema=!dedicated();
    std::vector<uint8_t> out; put32(out,cinemaSerial); out.push_back(cinematicVolume); out.push_back(cinematicEog); out.push_back(cinematicLevel);
    broadcast(Cinematic,out); state(); completeCinema();
    OSD_Printf("NET cinematic begin generation=%u volume=%d level=%d\n",cinemaSerial,cinematicVolume,cinematicLevel);
}
void cutsceneDone()
{
    if (!watching) return;
    if (server()) { if (validPlayer(localPlayer())) cinemaPending[localPlayer()]=false; completeCinema(); }
    else { std::vector<uint8_t> out; put32(out,cinemaSerial); send(-1,CinematicDone,out); }
    OSD_Printf("NET cinematic local_finished generation=%u\n",cinemaSerial);
}
bool cutsceneWaiting() { return active() && watching; }
bool cinematicPlaying() { return cinemaRunning; }
void levelEnded()
{
    if (!server() || ending || watching) return;
    if (coop() && (ud.eog || RRRA) && !ud.from_bonus) { levelExit=true; cutsceneBegin(); }
    else beginIntermission();
}
void tick()
{
    if (!active()) return;
    if (!canChase()) for (int i=0;i<MAXPLAYERS;++i) if (validPlayer(i)) g_player[i].ps->over_shoulder_on=0;
    if (!server()) return;
    for (int n=0;n<MAXPLAYERS;++n) if (connected(n) && validPlayer(n))
        updateScore(scores[n],g_player[n].ps->frag,g_player[n].ps->fraggedself);
    for (int n=0;n<MAXPLAYERS;++n) restoreInventory(n);
    for (int n=0;n<MAXPLAYERS;++n) if (validPlayer(n)) g_player[n].ps->auto_aim=settings.autoAim;
    if (ending)
    {
        if (rnet::tick()>=finishAt) { ending=false; if (restartAfter) queueMap(mapName(),true); else nextMap(); }
        else if (rnet::tick()-lastStateTick>=TickRate) state();
        return;
    }
    if (watching) { completeCinema(); return; }
    if (!inGame()) return;
    if (introPending)
    {
        bool any=false, ready=true;
        for (int n=0;n<MAXPLAYERS;++n) if (connected(n)) { any=true; ready &= playing(n); }
        if (any && ready) { introPending=false; cutsceneBegin(); return; }
    }
    if (finiteLives())
    {
        bool any=false, allOut=true, changed=false;
        for (int n=0;n<MAXPLAYERS;++n)
        {
            if (!usable(n)) continue;
            any=true; auto& l=life[n]; auto* p=g_player[n].ps;
            if (l.state!=Out) allOut=false;
            if (l.state==Alive && sprite[p->i].extra<=0) { death(n); changed=true; }
            if (l.state==Down)
            {
                stopBody(n);
                bool use=TEST_SYNC_KEY(g_player[n].inputBits->bits,SK_OPEN);
                if (giveUp(l,use,3u*TickRate))
                {
                    sprite[p->i].yrepeat=l.standingHeight;
                    p->last_extra=sprite[p->i].extra=0; p->dead_flag=0;
                    P_FragPlayer(n); stopBody(n); changed=true;
                }
                for (int helper=0;helper<MAXPLAYERS && l.state==Down;++helper)
                {
                    if (helper==n || !usable(helper) || life[helper].state!=Alive) continue;
                    auto* h=g_player[helper].ps;
                    if (sprite[h->i].extra<=0 || !TEST_SYNC_KEY(g_player[helper].inputBits->bits,SK_OPEN)) continue;
                    if (klabs(h->pos.x-p->pos.x)+klabs(h->pos.y-p->pos.y)>1024 || klabs(h->pos.z-p->pos.z)>(48<<8)) continue;
                    if (!cansee(h->pos.x,h->pos.y,h->pos.z,h->cursectnum,p->pos.x,p->pos.y,p->pos.z,p->cursectnum)) continue;
                    revive(l);
                    sprite[p->i].yrepeat=l.standingHeight;
                    sprite[p->i].extra=p->last_extra=std::min(50,p->max_player_health); p->dead_flag=0;
                    p->frag_ps=n; p->wackedbyactor=-1;
                    p->hurt_delay=30; changed=true;
                    char line[128]; Bsnprintf(line,sizeof(line),"%s revived %s.",g_player[helper].user_name,g_player[n].user_name); announce(line);
                }
            }
            else if (l.state==Out) stopBody(n);
            else if (l.state==Dead && !TEST_SYNC_KEY(g_player[n].inputBits->bits,SK_OPEN)) l.useWasDown=false;
        }
        if (any && allOut) { beginIntermission(true); return; }
        if (changed || rnet::tick()-lastStateTick>=TickRate/3) state();
    }
    if (settings.timeLimit && rnet::tick()-startedAt>=uint32_t(settings.timeLimit)*60u*TickRate) { beginIntermission(); return; }
    if (!coop() && settings.fragLimit)
    {
        int64_t teamScores[2]={0,0};
        for (int n=0;n<MAXPLAYERS;++n) if (usable(n))
        {
            int32_t const total=score(n);
            if (teams()) teamScores[g_player[n].ps->team&1]+=total;
            else if (total>=settings.fragLimit) { beginIntermission(); return; }
        }
        if (teams() && std::max(teamScores[0],teamScores[1])>=settings.fragLimit) beginIntermission();
    }
}
void draw()
{
    if (!active() || dedicated()) return;
    if (pendingCinema && !cinemaRunning)
    {
        pendingCinema=false; cinemaRunning=true;
        int volume=ud.volume_number, eog=ud.eog, level=ud.level_number;
        ud.volume_number=cinematicVolume; ud.eog=cinematicEog; ud.level_number=cinematicLevel;
        playCutscene();
        ud.volume_number=volume; ud.eog=eog; ud.level_number=level;
        cinemaRunning=false; cutsceneDone();
    }
    int n=localPlayer();
    if (spectating(n))
    {
        if (!usable(screenpeek) || life[screenpeek].state!=Alive)
            for (int i=0;i<MAXPLAYERS;++i) if (usable(i) && life[i].state==Alive) { screenpeek=i; break; }
        const char* key=KB_ScanCodeToString(ud.config.KeyboardKeys[gamefunc_See_Coop_View][0]);
        char text[120];
        Bsnprintf(text,sizeof(text),"OUT - %s TO SPECTATE: %s",key&&*key?key:"SEE CO-OP VIEW",validPlayer(screenpeek)?g_player[screenpeek].user_name:"");
        minitext(32,176,text,0,2+8+16);
    }
    else if (downed(n))
    {
        const char* key=KB_ScanCodeToString(ud.config.KeyboardKeys[gamefunc_Open][0]);
        char text[100]; Bsnprintf(text,sizeof(text),"DOWN - HOLD %s TO GIVE UP (%u%%)",key&&*key?key:"USE",giveUpProgress(n));
        minitext(65,176,text,0,2+8+16);
    }
    if (watching && !pendingCinema && !cinemaRunning) minitext(56,184,"WAITING FOR PLAYERS TO FINISH THE CUTSCENE",0,2+8+16);
}
bool message(int player,uint8_t type,const uint8_t* data,size_t size)
{
    if (type<RulesFirst || type>=PresenceFirst) return false;
    if (server())
    {
        if (type==CinematicDone && size==4 && validPlayer(player) && watching && get32(data)==cinemaSerial)
        {
            cinemaPending[player]=false;
            OSD_Printf("NET cinematic player_finished player=%d generation=%u\n",player,cinemaSerial);
            completeCinema();
        }
        return true;
    }
    if (type==State)
    {
        if (size!=26u+3u*MAXPLAYERS || get32(data)!=mapSerial()) return true;
        if (data[4]>2 || data[5]>1 || data[10]>4 || data[11]>1 || data[12]>3 || data[13]>2
            || data[14]>2 || data[15]>1 || data[16]>1 || data[17]>1 || data[18]>1 || data[20]>1 || data[21]>1) return true;
        for (int i=0;i<MAXPLAYERS;++i) if (data[27+3*i]>Out || data[28+3*i]>100) return true;
        settings.mode=data[4]; settings.friendlyFire=data[5];
        settings.fragLimit=data[6]|unsigned(data[7])<<8; settings.timeLimit=data[8]|unsigned(data[9])<<8;
        settings.difficulty=data[10]; settings.autoAim=data[11]; settings.weaponStay=data[12]; settings.items=data[13];
        settings.monsters=data[14]; settings.inventory=data[15]; settings.keys=data[16]; settings.holster=data[17]; settings.chase=data[18];
        settings.lifeLimit=data[19]; ending=data[20]!=0; watching=data[21]!=0; finishAt=rnet::tick()+get32(data+22);
        for (int i=0;i<MAXPLAYERS;++i)
        {
            const uint8_t* p=data+26+3*i;
            life[i].remaining=p[0]; life[i].state=LifeState(p[1]); life[i].hold=p[2]*(3u*TickRate)/100u;
        }
        applySettings();
    }
    else if (type==Cinematic && size==7 && get32(data)==mapSerial())
    {
        cinemaSerial=get32(data); cinematicVolume=data[4]; cinematicEog=data[5]; cinematicLevel=data[6];
        if (cinematicVolume>=MAXVOLUMES || cinematicLevel>=MAXLEVELS) return true;
        watching=pendingCinema=true;
    }
    return true;
}
bool parseArgument(int argc,const char* const* argv,int& index)
{
    const char* name=argv[index]; while (*name=='-') ++name;
    if (!Bstrcasecmp(name,"nodudes")) { settings.monsters=0; monstersSet=true; return true; }
    // Observe the legacy compact skill option, but let the native parser keep
    // its single-player initialization and nightmare-respawn behavior.
    if (name[0]=='s' && name[1]>='0' && name[1]<='9' && !name[2])
    { settings.difficulty=std::min(4,int(name[1]-'0')); return false; }
    bool mode=!Bstrcasecmp(name,"gametype"), rotationOption=!Bstrcasecmp(name,"maprotation") || !Bstrcasecmp(name,"maplist");
    bool weaponMode=!Bstrcasecmp(name,"weaponsstay"), itemMode=!Bstrcasecmp(name,"itemsstay"), monsterMode=!Bstrcasecmp(name,"monsters");
    int* value=nullptr; bool toggle=false; int hi=65535;
    if (!Bstrcasecmp(name,"friendlyfire")) { value=&settings.friendlyFire; toggle=true; }
    else if (!Bstrcasecmp(name,"fraglimit") || !Bstrcasecmp(name,"scorelimit")) value=&settings.fragLimit;
    else if (!Bstrcasecmp(name,"timelimit")) { value=&settings.timeLimit; hi=10080; }
    else if (!Bstrcasecmp(name,"difficulty") || !Bstrcasecmp(name,"s")) { value=&settings.difficulty; hi=4; }
    else if (!Bstrcasecmp(name,"autoaim")) { value=&settings.autoAim; toggle=true; }
    else if (!Bstrcasecmp(name,"weaponstay")) { value=&settings.weaponStay; toggle=true; }
    else if (!Bstrcasecmp(name,"respawnitems") || !Bstrcasecmp(name,"itemrespawn")) { value=&settings.items; toggle=true; }
    else if (!Bstrcasecmp(name,"respawnmonsters") || !Bstrcasecmp(name,"monsterrespawn")) { value=&settings.monsters; toggle=true; }
    else if (!Bstrcasecmp(name,"respawninventory")) { value=&settings.inventory; toggle=true; }
    else if (!Bstrcasecmp(name,"keepkeys")) { value=&settings.keys; toggle=true; }
    else if (!Bstrcasecmp(name,"losekeys")) { value=&settings.keys; toggle=true; }
    else if (!Bstrcasecmp(name,"holsterweapon")) { value=&settings.holster; toggle=true; }
    else if (!Bstrcasecmp(name,"chaseview")) { value=&settings.chase; toggle=true; }
    else if (!Bstrcasecmp(name,"lives")) { value=&settings.lifeLimit; hi=255; }
    if (!value && !mode && !rotationOption && !weaponMode && !itemMode && !monsterMode) return false;
    if (index+1>=argc) { initprintf("Missing value for -%s\n",name); return true; }
    const char* arg=argv[++index]; bool good=true;
    if (mode)
    {
        if (!Bstrcasecmp(arg,"coop")) settings.mode=0;
        else if (!Bstrcasecmp(arg,"deathmatch") || !Bstrcasecmp(arg,"dukematch")) settings.mode=1;
        else if (!Bstrcasecmp(arg,"teams")) settings.mode=2;
        else good=false;
        if (!monstersSet) settings.monsters=settings.mode?0:1;
    }
    else if (rotationOption)
    {
        maps.clear(); std::string list(arg);
        if (!Bstrcasecmp(name,"maplist"))
        {
            std::ifstream file(arg,std::ios::binary|std::ios::ate);
            if (!file || file.tellg()<0 || file.tellg()>128*1024)
            { initprintf("Unable to read map list '%s' (maximum 128 KiB).\n",arg); return true; }
            list.resize(size_t(file.tellg())); file.seekg(0); file.read(&list[0],list.size());
            std::string cleaned;
            size_t lineBegin=0;
            while (lineBegin<list.size())
            {
                size_t lineEnd=list.find('\n',lineBegin); if (lineEnd==std::string::npos) lineEnd=list.size();
                std::string line=list.substr(lineBegin,lineEnd-lineBegin);
                auto comment=line.find_first_of("#;"); if (comment!=std::string::npos) line.erase(comment);
                auto first=line.find_first_not_of(" \t\r");
                if (first!=std::string::npos)
                {
                    line=line.substr(first,line.find_last_not_of(" \t\r")-first+1);
                    if (!cleaned.empty()) cleaned+=',';
                    cleaned+=line;
                }
                lineBegin=lineEnd+1;
            }
            list=cleaned;
        }
        size_t begin=0;
        while (begin<list.size())
        {
            size_t end=list.find(',',begin); if (end==std::string::npos) end=list.size();
            std::string map=list.substr(begin,end-begin);
            if (map.empty() || map.size()>240 || map.find("..")!=std::string::npos || maps.size()>=256) { good=false; break; }
            maps.push_back(map); begin=end+1;
        }
        mapIndex=maps.empty()?0:maps.size()-1;
    }
    else if (weaponMode)
    {
        if (!Bstrcasecmp(arg,"norespawn")) settings.weaponStay=0;
        else if (!Bstrcasecmp(arg,"permanent")) settings.weaponStay=1;
        else if (!Bstrcasecmp(arg,"respawn")) settings.weaponStay=2;
        else if (!Bstrcasecmp(arg,"markers")) settings.weaponStay=3;
        else good=false;
    }
    else if (itemMode)
    {
        if (!Bstrcasecmp(arg,"norespawn")) settings.items=0;
        else if (!Bstrcasecmp(arg,"respawn")) settings.items=1;
        else if (!Bstrcasecmp(arg,"markers")) settings.items=2;
        else good=false;
    }
    else if (monsterMode)
    {
        if (!Bstrcasecmp(arg,"none")) settings.monsters=0;
        else if (!Bstrcasecmp(arg,"spawn")) settings.monsters=1;
        else if (!Bstrcasecmp(arg,"respawn")) settings.monsters=2;
        else good=false;
        if (good) monstersSet=true;
    }
    else good=toggle?boolean(arg,*value):integer(arg,0,hi,*value);
    if (good && !Bstrcasecmp(name,"losekeys")) settings.keys=!settings.keys;
    if (good && (!Bstrcasecmp(name,"difficulty") || !Bstrcasecmp(name,"s")))
        ud.m_player_skill=ud.player_skill=settings.difficulty;
    if (good && (!Bstrcasecmp(name,"respawnmonsters") || !Bstrcasecmp(name,"monsterrespawn")))
    { settings.monsters=settings.monsters?2:1; monstersSet=true; }
    if (!good) initprintf("Invalid value '%s' for -%s\n",arg,name);
    return true;
}
void status()
{
    unsigned viewers=0;
    for (int n=0;n<MAXPLAYERS;++n) viewers+=watching && connected(n) && cinemaPending[n];
    OSD_Printf("NET rules gametype=%s friendly_fire=%d scorelimit=%d timelimit=%d difficulty=%d autoaim=%d weapon_mode=%d item_mode=%d monster_mode=%d keepkeys=%d holster_weapon=%d chase_view=%d\n",
        coop()?"coop":teams()?"teams":"deathmatch",settings.friendlyFire,settings.fragLimit,settings.timeLimit,settings.difficulty,
        settings.autoAim,settings.weaponStay,settings.items,settings.monsters,settings.keys,canHolster(),canChase());
    OSD_Printf("NET intermission active=%d remaining_seconds=%u cinematic=%d level_ticks=%d viewers=%u\n",intermission(),remainingSeconds(),watching,
        validPlayer(connecthead)?g_player[connecthead].ps->player_par:0,viewers);
    for (int n=0;n<MAXPLAYERS;++n) if (connected(n))
    {
        OSD_Printf("NET lives player=%d remaining=%d down=%d out=%d giveup=%u\n",n,lives(n),downed(n),spectating(n),giveUpProgress(n));
        if (server() && validPlayer(n)) OSD_Printf("NET score player=%d total=%d native_frags=%u native_penalties=%u\n",
            n,score(n),unsigned(g_player[n].ps->frag),unsigned(g_player[n].ps->fraggedself));
    }
}
void registerCommands()
{
    OSD_RegisterFunction("server_rule","server_rule <name> <value>: set an authoritative server gameplay rule",command);
    OSD_RegisterFunction("nextmap","nextmap: advance the server map rotation",command);
    OSD_RegisterFunction("maplist","maplist: list server map rotation",command);
}
} }
