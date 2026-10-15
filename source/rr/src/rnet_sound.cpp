// SPDX-License-Identifier: GPL-2.0-or-later
#include "duke3d.h"
#include "rnet.h"
#include "rnet_sound.h"
#include "rnet_sound_model.h"
#include "../../blood/src/netprotocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace rnet { namespace audio { namespace {
enum Kind { Play, Stop, StopAll, Pitch };
struct Voice {
    int number=0,emitter=-1,tile=-1,owner=-1,pitch=0;
    bool loop=false,valid=true;
    double remaining=0;
    uint32_t born=0;
};
struct Metadata {std::string filename;uint32_t duration=0;bool read=false;};
std::array<Metadata,MAXSOUNDS> metadata;
std::array<uint32_t,MAXSOUNDS> startCount{},receiveCount{},playCount{};
std::vector<Voice> voices,pending;
std::array<uint32_t,MAXPLAYERS> lastTaunt{};
std::array<bool,MAXPLAYERS> taunted{};
unsigned depth=0;
bool simulation=false,replay=false;
uint32_t started=0,received=0,audible=0,expired=0,taunts=0;
constexpr size_t MaxVoices=4096;
bool publish() {return server()&&inGame()&&simulation&&!replay&&depth==1;}
bool valid(int n) {return n>=0&&n<MAXSOUNDS&&n<=g_highestSoundIdx;}
bool emitterValid(const Voice& v) {
    return v.emitter==-1 || (v.emitter>=0&&v.emitter<MAXSPRITES&&sprite[v.emitter].statnum<MAXSTATUS&&sprite[v.emitter].statnum!=STAT_NETALLOC&&
        sprite[v.emitter].picnum==v.tile&&sprite[v.emitter].owner==v.owner);
}
void removed(const Voice& v) {
    // Same actor callback as S_Cleanup, even when no mixer exists.
    if(v.emitter>=0&&emitterValid(v)&&sprite[v.emitter].picnum==MUSICANDSFX&&
       sprite[v.emitter].sectnum>=0&&sprite[v.emitter].sectnum<numsectors&&sector[sprite[v.emitter].sectnum].lotag<3)
        actor[v.emitter].t_data[0]=0;
}
uint32_t duration(int n) {
    auto& m=metadata[n]; const char* filename=g_sounds[n].filename;
    if(!filename)return 0;
    if(m.filename!=filename)m=Metadata{};
    if(m.read)return m.duration;
    m.filename=filename;m.read=true;
    int handle=S_OpenAudio(filename,g_loadFromGroupOnly,0);
    if(handle<0)return 0;
    int size=kfilelength(handle);
    if(size>0&&size<=64*1024*1024) {
        std::vector<uint8_t> bytes(size);
        if(kread(handle,bytes.data(),size)==size)m.duration=durationMs(bytes.data(),bytes.size());
    }
    kclose(handle);
    if(!m.duration)OSD_Printf("NET sound unavailable id=%d file=%s (unsupported or invalid audio)\n",n,filename);
    return m.duration;
}
void writeVoice(netwire::Writer& out,const Voice& v) {
    out.u16(uint16_t(v.number));out.u16(uint16_t(v.emitter));out.u16(uint16_t(v.tile));out.u16(uint16_t(v.owner));
    out.u32(uint32_t(v.pitch));out.u8(v.loop);out.u32(v.born);
}
Voice readVoice(netwire::Reader& in) {
    Voice v;v.number=in.u16();v.emitter=int16_t(in.u16());v.tile=int16_t(in.u16());v.owner=int16_t(in.u16());
    v.pitch=int32_t(in.u32());unsigned loop=in.u8();v.loop=loop!=0;v.valid=loop<=1;v.born=in.u32();return v;
}
bool validVoice(const Voice& v) {
    return v.valid&&valid(v.number)&&v.emitter>=-1&&v.emitter<MAXSPRITES&&v.tile>=-1&&v.tile<MAXTILES&&
        v.pitch>=-32768&&v.pitch<=32767;
}
void event(Kind kind,const Voice& v=Voice{}) {
    netwire::Writer out;out.u32(mapSerial());out.u8(kind);writeVoice(out,v);broadcast(Event,out.data);
}
void playLocal(const Voice& v,bool fallback=false) {
    if(dedicated())return;
    replay=true;
    auto flags=g_sounds[v.number].m;
    if(v.loop)g_sounds[v.number].m|=SF_LOOP;
    int id=v.emitter>=0&&!fallback ? S_PlaySound3D(v.number,v.emitter,(const vec3_t*)&sprite[v.emitter]) : S_PlaySound(v.number);
    g_sounds[v.number].m=flags;
    if(id>=0) {
        ++audible;++playCount[v.number];
        S_ChangeSoundPitch(v.number,fallback?-1:v.emitter,v.pitch);
    }
    replay=false;
}
void localEvent(Kind kind,const Voice& v) {
    ++received;
    if(kind==Play) {
        ++receiveCount[v.number];
        if(v.loop) {
            voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& p){return p.number==v.number&&p.emitter==v.emitter;}),voices.end());
            if(voices.size()>=MaxVoices)return;
            voices.push_back(v);
        }
        if(v.emitter==-1)playLocal(v);
        else pending.push_back(v); // wait for state on the separate snapshot channel
    } else {
        replay=true;
        if(kind==StopAll){pending.clear();voices.clear();S_StopAllSounds();}
        else if(kind==Stop) {
            voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& p){return p.number==v.number&&(v.emitter==-1||p.emitter==v.emitter);}),voices.end());
            pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const Voice& p){return p.number==v.number&&(v.emitter==-1||p.emitter==v.emitter);}),pending.end());
            S_StopEnvSound(v.number,v.emitter);
        } else if(kind==Pitch) {
            for(auto& p:voices)if(p.number==v.number&&(v.emitter==-1||p.emitter==v.emitter))p.pitch=v.pitch;
            for(auto& p:pending)if(p.number==v.number&&(v.emitter==-1||p.emitter==v.emitter))p.pitch=v.pitch;
            S_ChangeSoundPitch(v.number,v.emitter,v.pitch);
        }
        replay=false;
    }
}
void relayTaunt(int sender,int number) {
    if(sender<0||sender>=MAXPLAYERS||!rnet::playing(sender)||number<1||number>10)return;
    uint32_t now=timerGetTicks();
    if(taunted[sender]&&now-lastTaunt[sender]<1000)return;
    taunted[sender]=true;lastTaunt[sender]=now;++taunts;
    netwire::Writer out;out.u8(uint8_t(number));out.u8(uint8_t(sender));
    for(int p=0;p<MAXPLAYERS;++p)if(p!=sender&&connected(p))send(p,Taunt,out.data);
    if(!dedicated()&&localPlayer()!=sender)G_StartRTS(number,0);
}
int32_t command(const osdfuncparm_t*) {
    unsigned loops=0;for(auto const& v:voices)loops+=v.loop;
    OSD_Printf("NET audio active=%u loops=%u pending=%u started=%u received=%u played=%u expired=%u taunts=%u\n",
        unsigned(voices.size()),loops,unsigned(pending.size()),started,received,audible,expired,taunts);
    for(auto const& v:voices)OSD_Printf("NET voice sound=%d emitter=%d loop=%d remaining_ms=%d pitch=%d\n",v.number,v.emitter,v.loop,v.loop?0:int(v.remaining),v.pitch);
    for(int n=0;n<MAXSOUNDS;++n)if(startCount[n]||receiveCount[n]||playCount[n])
        OSD_Printf("NET audio_sound sound=%d started=%u received=%u played=%u local_voices=%d\n",n,startCount[n],receiveCount[n],playCount[n],g_sounds[n].num);
    return OSDCMD_OK;
}
} // namespace
Scope::Scope(){++depth;}
Scope::~Scope(){--depth;}
void reset() {
    simulation=false;
    if(!dedicated()&&(!voices.empty()||!pending.empty())){replay=true;S_StopAllSounds();replay=false;}
    voices.clear();pending.clear();startCount.fill(0);receiveCount.fill(0);playCount.fill(0);
    taunted.fill(false);lastTaunt.fill(0);started=received=audible=expired=taunts=0;
}
bool authoritative(){return server()&&(simulation||dedicated())&&!replay;}
bool playing(int number,int emitter) {
    return std::any_of(voices.begin(),voices.end(),[&](const Voice& v){return (number==-1||v.number==number)&&(emitter==-1||v.emitter==emitter);});
}
int start(int n,int emitter) {
    if(!publish())return -2;
    if(!valid(n)||emitter<-1||emitter>=MAXSPRITES)return -1;
    auto const& snd=g_sounds[n];
    if(snd.m&SF_DTAG)emitter=-1;
    if(emitter>=0&&sprite[emitter].statnum>=MAXSTATUS)return -1;
    uint32_t ms=duration(n);if(!ms)return -1;
    bool loop=(snd.m&SF_LOOP)||ms==UINT32_MAX;
    if(loop&&(snd.m&SF_ONEINST_INTERNAL)&&playing(n,-1))return -1;
    if(loop&&emitter>=0&&playing(n,emitter))return -1;
    // A sprite has one instance of a given non-ambient sound. This replacement
    // is implicit in the Play event and never sends a second Stop event.
    if(emitter>=0&&sprite[emitter].picnum!=MUSICANDSFX)
        voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& v){return v.number==n&&v.emitter==emitter;}),voices.end());
    unsigned count=0;for(auto const& v:voices)count+=v.number==n;
    if(count>=MAXSOUNDINSTANCES&&(emitter<0||sprite[emitter].picnum!=MUSICANDSFX)) {
        auto it=std::find_if(voices.begin(),voices.end(),[&](const Voice& v){return v.number==n;});
        if(it!=voices.end()){if(it->loop||loop)return -1;removed(*it);voices.erase(it);}
    }
    if(voices.size()>=MaxVoices)return -1;
    Voice v;v.number=n;v.emitter=emitter;v.loop=loop;v.remaining=ms;v.born=tick();
    v.pitch=(int(snd.ps)+int(snd.pe))/2;
    if(emitter>=0){v.tile=sprite[emitter].picnum;v.owner=sprite[emitter].owner;}
    voices.push_back(v);++started;++startCount[n];event(Play,v);return 1;
}
void stop(int n,int emitter) {
    if(!publish()||!valid(n)||emitter<-1||emitter>=MAXSPRITES)return;
    voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& v){
        if(v.number!=n||(emitter!=-1&&v.emitter!=emitter))return false;
        removed(v);return true;
    }),voices.end());
    Voice v;v.number=n;v.emitter=emitter;event(Stop,v);
}
void stopAll() {
    if(!publish())return;
    for(auto const& v:voices)removed(v);
    voices.clear();event(StopAll);
}
void pitch(int n,int emitter,int value) {
    if(!publish()||!valid(n)||emitter<-1||emitter>=MAXSPRITES)return;
    value=std::max(-32768,std::min(32767,value));
    for(auto& v:voices)if(v.number==n&&(emitter==-1||v.emitter==emitter))v.pitch=value;
    Voice v;v.number=n;v.emitter=emitter;v.pitch=value;event(Pitch,v);
}
void beginTick() {
    simulation=true;
    voices.erase(std::remove_if(voices.begin(),voices.end(),[](Voice& v){
        if(!v.loop)v.remaining-=1000.0/TickRate*std::exp2(v.pitch/1200.0);
        if(!v.loop&&v.remaining<=0){removed(v);++expired;return true;}return false;
    }),voices.end());
}
void endTick() {
    voices.erase(std::remove_if(voices.begin(),voices.end(),[](const Voice& v){
        if(emitterValid(v))return false;
        if(v.loop){event(Stop,v);if(!dedicated()){replay=true;S_StopEnvSound(v.number,v.emitter);replay=false;}}
        return true;
    }),voices.end());
    simulation=false;
}
std::vector<uint8_t> demoState() {
    netwire::Writer out;out.u32(mapSerial());unsigned count=0;
    for(auto const& v:voices)if(v.loop&&emitterValid(v))++count;
    out.u16(uint16_t(count));for(auto const& v:voices)if(v.loop&&emitterValid(v))writeVoice(out,v);
    return out.data;
}
void deleted(int emitter) {
    if(!server()||!inGame()||!simulation)return;
    voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& v){
        if(v.emitter!=emitter)return false;
        if(v.loop) { event(Stop,v);if(!dedicated()){replay=true;S_StopEnvSound(v.number,emitter);replay=false;} }
        return true;
    }),voices.end());
}
void ready(int p) {
    if(server())send(p,Baseline,demoState());
}
void snapshot() {
    std::vector<Voice> later;
    for(auto const& v:pending) {
        if(emitterValid(v))playLocal(v);
        else if(int32_t(tick()-v.born)>int32_t(TickRate/2)) {if(!v.loop)playLocal(v,true);}
        else later.push_back(v);
    }
    pending.swap(later);
}
bool handle(int sender,uint8_t type,const std::vector<uint8_t>& body) {
    if(type!=Event&&type!=Taunt&&type!=Baseline)return false;
    netwire::Reader in(body.data(),body.size());
    if(type==Taunt) {
        int number=in.u8();
        if(server()){if(in.end())relayTaunt(sender,number);}
        else {int who=in.u8();if(sender==-1&&in.end()&&number>=1&&number<=10&&who>=0&&who<MAXPLAYERS){++taunts;G_StartRTS(number,0);}}
        return true;
    }
    if(server()||sender!=-1)return true;
    uint32_t serial=in.u32();if(serial!=mapSerial())return true;
    if(type==Event) {
        unsigned kind=in.u8();Voice v=readVoice(in);
        if(in.end()&&kind<=Pitch&&validVoice(v)&&(kind!=Play||pending.size()<MaxVoices))localEvent(Kind(kind),v);
    } else {
        unsigned count=in.u16();if(count>MaxVoices)return true;
        std::vector<Voice> loops;for(unsigned i=0;i<count;++i){Voice v=readVoice(in);if(!validVoice(v)||!v.loop)return true;loops.push_back(v);}
        if(!in.end())return true;
        replay=true;S_StopAllSounds();replay=false;voices=loops;pending=std::move(loops);
    }
    return true;
}
void taunt(int number) {
    if(!active()||number<1||number>10)return;
    if(server())relayTaunt(localPlayer(),number);
    else {netwire::Writer out;out.u8(uint8_t(number));send(-1,Taunt,out.data);}
}
void registerCommands(){OSD_RegisterFunction("net_audiostatus","net_audiostatus: show authoritative sound state",command);}
} }
