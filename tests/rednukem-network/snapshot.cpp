// SPDX-License-Identifier: GPL-2.0-or-later
// Tests use the production codec and graph validator directly. The game-only
// capture/apply integration is discarded by linker section GC.
#include "../../source/rr/src/rnet_snapshot.cpp"
#include <cstdio>
#include <cstdlib>

int32_t APLAYER = 1405;

namespace
{
unsigned assertions = 0;
void require(bool good, const char* why)
{
    ++assertions;
    if (!good) { fprintf(stderr,"snapshot test failed: %s\n",why); std::exit(1); }
}
std::unique_ptr<rnet::World> world()
{
    auto w = std::unique_ptr<rnet::World>(new rnet::World);
    w->sectorCount = 1; w->wallCount = 4; w->multimode = 1; w->scriptSize = 32;
    w->g_curViewscreen = -1;
    w->sectors.resize(1); w->sectors[0].wallnum = 4;
    w->sectors[0].floorz = 16384;
    w->walls.resize(4);
    for (int i = 0; i < 4; ++i)
    {
        auto& v = w->walls[i]; v.point2 = (i+1)%4;
        v.nextwall = v.nextsector = -1;
        v.x = (i == 1 || i == 2) ? 4096 : 0; v.y = i >= 2 ? 4096 : 0;
    }
    w->sprites.resize(MAXSPRITES);
    for (auto& s : w->sprites) { s.sectnum = MAXSECTORS; s.statnum = MAXSTATUS; }
    auto& s = w->sprites[0]; s.sectnum = 0; s.statnum = STAT_PLAYER; s.picnum = APLAYER;
    s.owner = 0; s.x = s.y = 2048; s.z = 8192;
    w->actors.resize(MAXSPRITES); w->extensions.resize(MAXSPRITES);
    w->wallBlend.resize(4); w->wallLink.assign(8,-1); w->sectorBunch.assign(2,-1);
    auto& p = w->players[0]; p.playing = 1; p.state.i = 0; p.state.cursectnum = 0;
    p.state.curr_weapon = 1; p.state.last_weapon = -1; p.state.ammo_amount[1] = 42;
    p.state.pos.x = s.x; p.state.pos.y = s.y; p.state.pos.z = s.z;
    p.state.drug_aspect = -32768; p.state.noise_x = INT32_MIN; p.state.noise_y = INT32_MAX;
    p.state.gotweapon = 0x12345678u;
    Bstrcpy(p.name,"Duke01");
    rnet::GameVar v; v.id = 3; v.values = {-13};
    w->vars.push_back(v); w->varCount = 1;
    rnet::Quote q; q.id = 4; Bstrcpy(q.text,"Authoritative quote");
    w->quotes.push_back(q); w->quoteCount = 1;
    return w;
}
std::vector<uint8_t> encode(rnet::World& w)
{
    std::vector<uint8_t> bytes;
    rnet::Writer writer{bytes};
    require(rnet::archive(writer,w),"fixture encoding");
    return bytes;
}
void rejected(rnet::World& w,const char* why)
{
    std::string error;
    require(!rnet::validate(encode(w),error),why);
    require(!error.empty(),"rejections have diagnostics");
}
}

int main()
{
    auto w = world(); auto bytes = encode(*w);
    std::string error;
    require(rnet::validate(bytes,error),error.c_str());
    auto decoded = std::unique_ptr<rnet::World>(new rnet::World);
    require(rnet::decode(bytes,*decoded,error),"decode valid world");
    require(encode(*decoded) == bytes,"byte-exact portable roundtrip");
    require(decoded->players[0].state.ammo_amount[1] == 42,"authoritative HUD ammo");
    require(decoded->players[0].state.noise_x == INT32_MIN && decoded->players[0].state.noise_y == INT32_MAX,
            "signed 32-bit packed fields");
    require(decoded->players[0].state.drug_aspect == -32768,"unaligned RR state roundtrip");
    require(decoded->vars[0].values[0] == -13 && Bstrcmp(decoded->quotes[0].text,"Authoritative quote") == 0,
            "mutable CON render state");
    require(rnet::checkViewpoint(*decoded,0,error),"active local viewpoint");
    require(!rnet::checkViewpoint(*decoded,-1,error),"reject negative local viewpoint");
    require(!rnet::checkViewpoint(*decoded,MAXPLAYERS,error),"reject oversized local viewpoint");
    require(bytes[0] == 'R' && bytes[1] == 'N' && bytes[2] == 'S' && bytes[3] == '1',"LE magic");

    auto original = bytes;
    bytes.push_back(0); require(!rnet::validate(bytes,error),"reject trailing data");
    bytes = original; bytes[0] ^= 1; require(!rnet::validate(bytes,error),"reject incompatible magic");
    for (size_t n : {size_t(0),size_t(1),size_t(38),size_t(39),size_t(1200),original.size()-1})
    {
        bytes.assign(original.begin(),original.begin()+n);
        require(!rnet::validate(bytes,error),"reject truncated wire state");
    }
    w=world(); w->walls[0].point2=4; rejected(*w,"wall point outside array");
    w=world(); w->walls[0].point2=2; rejected(*w,"wall cycle with duplicate predecessor");
    w=world(); w->walls[0].nextwall=1; w->walls[0].nextsector=0; rejected(*w,"nonreciprocal portal");
    w=world(); w->sectors[0].wallptr=2; rejected(*w,"sector range outside wall array");
    w=world(); w->sprites[0].sectnum=1; rejected(*w,"sprite sector outside loaded world");
    w=world(); w->sprites[4].sectnum=0; rejected(*w,"inconsistent free-list membership");
    w=world(); w->sprites[0].owner=MAXSPRITES; rejected(*w,"sprite owner outside array");
    w=world(); w->sprites[0].owner=-1; w->sprites[0].yvel=MAXPLAYERS;
    rejected(*w,"ownerless APLAYER still requires a valid player slot");
    w=world(); w->sprites[0].picnum=0; w->sprites[0].yvel=MAXPLAYERS;
    rejected(*w,"player status still requires a valid player slot");
    w=world(); w->actors[0].t_data[4]=31; rejected(*w,"CON action extends beyond program");
    w=world(); w->players[0].state.i=MAXSPRITES; rejected(*w,"invalid player sprite reference");
    w=world(); w->players[0].state.curr_weapon=MAX_WEAPONS; rejected(*w,"invalid HUD weapon index");
    w=world(); w->players[0].state.kickback_pic=-1; rejected(*w,"negative HUD animation frame");
    w=world(); w->players[0].state.numloogs=65; rejected(*w,"invalid HUD loogie array count");
    w=world(); w->players[0].state.ftq=MAXQUOTES; rejected(*w,"invalid quote index");
    w=world(); w->players[0].name[31]='x'; rejected(*w,"unterminated player name");
    w=world(); w->g_cloudCnt=257; rejected(*w,"unbounded cloud count");
    w=world(); w->g_mirrorCount=1; w->g_mirrorWall[0]=4; rejected(*w,"invalid mirror reference");
    w=world(); w->wallLink[0]=4; rejected(*w,"invalid TROR wall reference");
    w=world(); w->sectorBunch[0]=0; rejected(*w,"invalid TROR bunch reference");
    w=world(); w->vars.push_back(w->vars[0]); w->varCount=2; rejected(*w,"duplicate gamevar id");
    w=world(); w->quotes[0].text[MAXQUOTELEN-1]='x'; rejected(*w,"unterminated CON quote");
    w=world(); w->extensions[0].alpha=std::numeric_limits<float>::quiet_NaN(); rejected(*w,"NaN sprite alpha");
    w=world(); w->players[0].playing=0;
    require(rnet::validate(encode(*w),error),"empty dedicated-server roster");
    require(!rnet::checkViewpoint(*w,0,error),"inactive slot cannot become the local camera");
    w->players[0].state.cursectnum=INT16_MAX;
    rejected(*w,"inactive APLAYER cannot reference an invalid sector");
    w=world(); w->players[0].playing=0; w->players[0].state.i=-1;
    rejected(*w,"inactive APLAYER cannot reference an invalid body");
    w=world(); w->players[0].playing=0; w->players[0].state.curr_weapon=-1;
    rejected(*w,"inactive APLAYER cannot reference an invalid HUD weapon");
    w=world(); w->players[1].state.cursectnum=INT16_MAX; w->players[1].state.i=-1;
    require(rnet::validate(encode(*w),error),"unreferenced inactive slots need no world references");
    w=world(); w->players[7]=w->players[0]; w->players[0].playing=0;
    w->players[7].state.frag_ps=7; w->sprites[0].yvel=7;
    require(rnet::validate(encode(*w),error),"sparse roster with a lone higher-numbered slot");
    require(rnet::checkViewpoint(*w,7,error),"sparse active local viewpoint");
    w=world(); w->players[1]=w->players[0]; w->players[1].playing=0; w->players[1].state.i=1;
    w->sprites[1]=w->sprites[0]; w->sprites[1].yvel=1; w->sprites[1].owner=1;
    w->sprites[1].statnum=STAT_NETALLOC; w->sprites[1].cstat=32768;
    require(rnet::validate(encode(*w),error),"provisioned dormant player remains valid");
    w=world(); w->sprites[1].sectnum=0; w->sprites[1].statnum=STAT_STANDABLE;
    w->sprites[1].owner=-2;
    require(rnet::validate(encode(*w),error),"legal crane owner sentinel");
    w=world(); w->g_geoSectorCnt=1; w->g_geoSectorWarp[0]=w->g_geoSectorWarp2[0]=-1;
    require(rnet::validate(encode(*w),error),"optional RR geometry warps");
    w=world(); w->sprites[1].sectnum=0; w->sprites[1].statnum=STAT_EFFECTOR;
    w->sprites[1].owner=int16_t(0xf0f1);
    require(rnet::validate(encode(*w),error),"sector effector packed palette owner");
    printf("Rednukem canonical snapshots: %u assertions passed (%zu-byte fixture)\n",assertions,original.size());
}
