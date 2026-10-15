// SPDX-License-Identifier: GPL-2.0-or-later
#include "duke3d.h"
#include "common_game.h"
#include "gamedef.h"
#include "net.h"
#include "rnet.h"
#include "rnet_content.h"
#include "rnet_restart.h"
#include "rnet_map.h"
#include "rnet_map_binding.h"
#include "rnet_uri.h"
#include <algorithm>
#include <map>
#include <set>
#include <utility>
#ifdef _WIN32
# include "windows_inc.h"
#else
# include <sys/stat.h>
# include <unistd.h>
#endif

namespace {
using Kind = rnetaddon::SelectionKind;
enum : uint8_t { Offer=32, RequestFile, FileData, ContentReady };
struct Group { std::string path; bool base, preload, primary; };
std::vector<Group> groups;
std::vector<std::string> trackedFiles, roots, sources;
std::vector<rnetaddon::Selection> selected;
rnetaddon::Profile localProfile, requiredProfile, activationProfile;
std::vector<netassets::Asset> bases;
std::string initializationError, mapError, activationDirectory, localDirectory;
std::string mapLogical, mapAlias, mapSource;
char loadedMapSearchFirst=0;
rnet::content::MapBinding mapBinding;
std::vector<uint8_t> mapBytes;
rnetaddon::File mapFile;
rnetaddon::Cache download, mapCache;
bool captured=false, selectionsCaptured=false, finalized=false, activated=false, preloaded=false, receiving=false, receivingMap=false;
bool receivedOffer=false;
uint32_t offeredSerial=0;
uint32_t requiredScriptCRC=0;
size_t incomingIndex=0;
uint64_t incomingOffset=0;
uint32_t lastProgress=0;
struct Sending { uint32_t serial=0; size_t index=0; uint64_t offset=0; bool map=false, pending=false; };
std::map<int,Sending> outgoing;
std::map<std::string,std::string> activatedPaths;
std::set<std::string> trustedArchives;
std::string lower(std::string value)
{ for (char& c:value) if(c>='A' && c<='Z') c += 'a'-'A'; return value; }
std::string portable(std::string value)
{ std::replace(value.begin(),value.end(),'\\','/'); while(value.compare(0,2,"./")==0) value.erase(0,2); return value; }
std::string leafName(const std::string& value)
{ auto p=value.find_last_of("/\\"); return p==std::string::npos ? value : value.substr(p+1); }
bool regular(const std::string& path)
{
#ifdef _WIN32
    DWORD a=GetFileAttributesA(path.c_str());
    return a!=INVALID_FILE_ATTRIBUTES && !(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat s{}; return lstat(path.c_str(),&s)==0 && S_ISREG(s.st_mode);
#endif
}
bool directory(const std::string& path)
{
#ifdef _WIN32
    DWORD a=GetFileAttributesA(path.c_str());
    return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY) && !(a&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat s{}; return lstat(path.c_str(),&s)==0 && S_ISDIR(s.st_mode);
#endif
}
std::string absolute(const std::string& path)
{
    if(path.empty()) return {};
    char buffer[BMAX_PATH];
#ifdef _WIN32
    DWORD count=GetFullPathNameA(path.c_str(),sizeof(buffer),buffer,nullptr);
    return count && count<sizeof(buffer) ? portable(buffer) : std::string();
#else
    (void)buffer;
    char* resolved=realpath(path.c_str(),nullptr);
    if(!resolved)return {};
    std::string result=portable(resolved);
    free(resolved);
    return result.size()<BMAX_PATH ? result : std::string();
#endif
}
std::string resolve(const std::string& path)
{
    // Installed game libraries may use symlinks. Canonicalize trusted local
    // selections before hashing/opening their regular target; peer paths never
    // reach this lookup and the download cache remains strictly no-follow.
    auto direct=absolute(path);
    if(!direct.empty()&&regular(direct))return direct;
    char* found=nullptr;
    if(findfrompath(path.c_str(),&found)==0 && found)
    { std::string result=absolute(found);Xfree(found);return regular(result)?result:std::string(); }
    return {};
}
bool original(const std::string& name)
{
    static std::set<std::string> const names={"duke3d.grp","duke.rts","redneck.grp","redneck.rts",
        "rrra.grp","nam.grp","napalm.grp","ww2gi.grp","dn64widescreen.pk3",
        "dukedc.grp","vacation.grp","nwinter.grp","platoonl.grp"};
    if(names.count(lower(leafName(name)))) return true;
    for(auto const& g:groups) if(g.base && lower(g.path)==lower(name)) return true;
    return false;
}
std::string logical(const std::string& requested,const std::string& source)
{
    for(auto const& root:roots)
        if(source.size()>root.size() && source.compare(0,root.size(),root)==0 && source[root.size()]=='/')
            return source.substr(root.size()+1);
    auto result=portable(requested);
    return rnetaddon::validPath(result)?result:leafName(result);
}
bool addFile(const std::string& name,const std::string& source,std::string& error)
{
    if(original(source) || original(name)) return true;
    rnetaddon::File f; f.path=name;
    if(!rnetaddon::allowedFile(name)) { error="Cannot distribute unsupported resource: "+name; return false; }
    if(!rnetaddon::inspectFile(source,f,error)) return false;
    // An unchanged extracted resource already supplied by an identified
    // archive needs no loose download (including supplementary base GRPs).
    auto originalFile=kopen4load(name.c_str(),2);
    if(originalFile!=buildvfs_kfd_invalid)
    {
        bool same=kfilelength(originalFile)>=0 && uint64_t(kfilelength(originalFile))==f.size;
        netassets::MD5 originalDigest;uint8_t chunk[16384];uint64_t remaining=f.size;
        while(same&&remaining)
        {
            size_t count=size_t(std::min<uint64_t>(sizeof(chunk),remaining));
            same=kread(originalFile,chunk,int(count))==int(count);
            if(same){originalDigest.update(chunk,count);remaining-=count;}
        }
        kclose(originalFile);
        if(same&&originalDigest.finish()==f.md5)return true;
    }
    for(size_t i=0;i<localProfile.files.size();++i)
        if(lower(localProfile.files[i].path)==lower(name))
        {
            if(localProfile.files[i].path==name && localProfile.files[i].size==f.size && localProfile.files[i].md5==f.md5) return true;
            error="Resource paths collide: "+name; return false;
        }
    localProfile.files.push_back(f); sources.push_back(source);
    return rnetaddon::validate(localProfile,error);
}
bool scan(const std::string& root,const std::string& relative,unsigned depth,std::string& error)
{
    if(depth>16) {error="Addon nesting exceeds 16 directories";return false;}
    auto* dir=Bopendir((root+(relative.empty()?"":"/"+relative)).c_str());
    if(!dir) {error="Cannot scan addon directory";return false;}
    std::vector<std::string> entries;
    while(auto const* e=Breaddir(dir)) if(e->name[0]!='.') entries.emplace_back(e->name);
    Bclosedir(dir); std::sort(entries.begin(),entries.end());
    for(auto const& e:entries)
    {
        std::string const path=relative.empty()?e:relative+"/"+e;
        std::string const source=root+"/"+path;
        if(directory(source))
        {
            if(lower(e)=="autoload" || lower(e)=="mods" || lower(e)=="addons") continue;
            if(!scan(root,path,depth+1,error)) return false;
        }
        else if(regular(source) && rnetaddon::allowedFile(path) && !original(source))
            if(!addFile(path,source,error)) return false;
    }
    return true;
}
#ifndef USE_PHYSFS
int32_t resolveActivated(const char* name,char** output)
{
    if(!name) return 0;
    if(!mapAlias.empty()&&portable(name)==mapAlias)
    { *output=Xstrdup(mapAlias.c_str());return 1; }
    // initgroupfile uses this resolver too. The locally selected primary game
    // and commercial dependency archives remain readable from the installation;
    // they are not members of the downloaded addon cache.
    if(trustedArchives.count(lower(portable(name)))||original(name)
        ||lower(leafName(name))==lower(leafName(G_GrpFile())))return 0;
    auto const found=activatedPaths.find(lower(portable(name)));
    if(found!=activatedPaths.end()) { *output=Xstrdup(found->second.c_str()); return 1; }
    // All loose game resources must belong to the verified activation profile.
    // Remaining resources come from the user's base archives or addon archives.
    return activated ? 2 : 0;
}
#endif
bool vfsRead(const std::string& name,std::vector<uint8_t>& bytes,std::string& error,char searchFirst=0)
{
    auto f=kopen4load(name.c_str(),searchFirst);
    if(f==buildvfs_kfd_invalid) {error="Map is missing: "+leafName(name);return false;}
    int const size=kfilelength(f);
    if(size<0 || size_t(size)>rnet::content::MaxMapBytes) {kclose(f);error="Map exceeds download size limit";return false;}
    bytes.resize(size);
    bool ok=kread(f,bytes.data(),size)==size; kclose(f);
    if(!ok) {error="Cannot read complete map";return false;}
    return rnet::content::validateMap(bytes,error);
}
netassets::Digest digest(const std::vector<uint8_t>& bytes)
{ netassets::MD5 md5; md5.update(bytes.data(),bytes.size()); return md5.finish(); }
void write64(netwire::Writer& w,uint64_t v) {w.u32(uint32_t(v));w.u32(uint32_t(v>>32));}
uint64_t read64(netwire::Reader& r) {uint64_t v=r.u32();return v|(uint64_t(r.u32())<<32);}
void errorClient(const std::string& error)
{ receiving=false; receivedOffer=false; mapBinding.clear();download.cancel();mapCache.cancel();rnet::fail("Content: "+error); }
void requestNext()
{
    netwire::Writer w;w.u32(offeredSerial);w.u8(receivingMap?1:0);w.u32(uint32_t(incomingIndex));write64(w,incomingOffset);
    rnet::send(-1,RequestFile,w.data);
}
bool resourceArguments(const rnetaddon::Profile& profile,const std::string& root,std::vector<std::string>& args,std::string& error)
{
    if(!rnetaddon::validate(profile,error)) return false;
    for(auto const& file:profile.files)
        if(root.size()+1+file.path.size()>=BMAX_PATH)
        {error="Addon cache path exceeds the engine's filename limit: "+file.path;return false;}
    args={"-noautoload"};
    if(!root.empty()) {args.push_back("-j");args.push_back(root);}
    for(auto const& s:profile.selections)
    {
        const char* option=nullptr;
        switch(s.kind)
        {
        case Kind::Archive: option="-g";break;
        case Kind::PreloadArchive: continue;
        case Kind::Definition: option="-h";break;
        case Kind::DefinitionModule: option="-mh";break;
        case Kind::Script: option="-x";break;
        case Kind::ScriptModule: option="-mx";break;
        }
        if(!option) {error="Unknown addon selection";return false;}
        args.push_back(option);
        auto const file=std::find_if(profile.files.begin(),profile.files.end(),[&](const rnetaddon::File& f){return f.path==s.path;});
        std::string path=file==profile.files.end()?s.path:root+"/"+s.path;
        if(file==profile.files.end()&&s.kind==Kind::Archive)
            for(auto const& group:groups)
                if(group.base&&lower(leafName(group.path))==lower(s.path)){path=group.path;break;}
        args.push_back(path);
    }
    return true;
}
bool scheduleActivation(const std::string& root,std::string& error)
{
    std::vector<std::string> args;
    if(!resourceArguments(requiredProfile,root,args,error)) return false;
    rnet::uri::Endpoint endpoint;
    if(!rnet::uri::parse("rednukem://"+rnet::endpoint(),endpoint,error)) return false;
    if(!rnetAddonRestartRequest(args,endpoint.host,endpoint.port,g_netPassword,
        rnetaddon::identity(requiredProfile),requiredProfile,root,error)) return false;
    OSD_Printf("Server addon verified. Restarting Rednukem and reconnecting...\n");
    receiving=false; return true;
}
void readyMap()
{
    if(!mapAlias.empty()&&mapAlias.size()>=BMAX_PATH)
    {errorClient("Downloaded map cache path exceeds the engine's filename limit");return;}
    mapBinding.verified(mapAlias.empty()?mapLogical:mapAlias);
    receiving=false;
    netwire::Writer w;w.u32(offeredSerial);w.bytes(mapFile.md5.data(),mapFile.md5.size());
    rnet::send(-1,ContentReady,w.data);
    rnet::contentReady(rnet::localPlayer());
}
bool locateCachedMap(std::string& error)
{
    std::string const directory=absolute(mapCache.directory());
    if(directory.empty()) {error="Cannot resolve the verified map cache directory";return false;}
    mapAlias=directory+"/"+mapFile.path;
    return true;
}
void prepareMap()
{
    std::string error;std::vector<uint8_t> bytes;
    if(vfsRead(mapLogical,bytes,error) && bytes.size()==mapFile.size && digest(bytes)==mapFile.md5)
    { mapAlias.clear();readyMap();return; }
    rnetaddon::Profile p;p.name="Multiplayer map";p.files.push_back(mapFile);
    if(!mapCache.begin(p,error)) {errorClient(error);return;}
    if(mapCache.hasFile(0))
    {
        if(!locateCachedMap(error)){errorClient(error);return;}
        // Validate cached bytes independently of their content digest.
        FILE* f=fopen(mapAlias.c_str(),"rb");bytes.resize(size_t(mapFile.size));
        bool ok=f && fread(bytes.data(),1,bytes.size(),f)==bytes.size();if(f)fclose(f);
        if(!ok || !rnet::content::validateMap(bytes,error)) {errorClient(error.empty()?"Cannot read cached map":error);return;}
        if(!mapCache.finish(error)) {errorClient(error);return;}
        readyMap();return;
    }
    receiving=true;receivingMap=true;incomingIndex=0;incomingOffset=0;mapBytes.clear();
    mapBytes.reserve(size_t(mapFile.size));lastProgress=timerGetTicks();
    OSD_Printf("Downloading map %s (%u bytes)...\n",mapFile.path.c_str(),unsigned(mapFile.size));
    requestNext();
}
void prepareAddon()
{
    std::string error;
    if(rnetaddon::identity(requiredProfile)==rnetaddon::identity(localProfile))
    {
        if(requiredScriptCRC!=g_scriptcrc)
        {
            OSD_Printf("CON mismatch after resource verification: server %08x, client %08x\n",requiredScriptCRC,g_scriptcrc);
            errorClient("Compiled CON differs despite matching files; check included script dependencies and load order");return;
        }
        prepareMap();return;
    }
    OSD_Printf("Server addon differs from this installation. Required files:\n");
    for(auto const& f:requiredProfile.files)
    {
        auto found=std::find_if(localProfile.files.begin(),localProfile.files.end(),[&](const rnetaddon::File& v){return lower(v.path)==lower(f.path);});
        if(found==localProfile.files.end() || found->size!=f.size || found->md5!=f.md5)
            OSD_Printf("  %s: %s, MD5 %s (%llu bytes)\n",f.path.c_str(),found==localProfile.files.end()?"missing":"different",netassets::hex(f.md5).c_str(),(unsigned long long)f.size);
    }
    if(requiredProfile.files.empty()) {if(!scheduleActivation("",error)) errorClient(error);return;}
    // Reuse complete local installations without changing their contents.
    for(auto const& root:roots)
        if(rnetaddon::matches(root,requiredProfile,error)) {if(!scheduleActivation(root,error)) errorClient(error);return;}
    if(!download.begin(requiredProfile,error)) {errorClient(error);return;}
    // Copy matching installed files through the same digest-checked cache writer.
    for(size_t index=0;index<requiredProfile.files.size();++index)
    {
        if(download.hasFile(index)) continue;
        auto const& wanted=requiredProfile.files[index];
        for(size_t j=0;j<localProfile.files.size();++j)
        {
            if(localProfile.files[j].size!=wanted.size || localProfile.files[j].md5!=wanted.md5) continue;
            uint64_t offset=0;std::vector<uint8_t> chunk;
            bool ok=true;
            do {
                if(!rnetaddon::readChunk(sources[j],wanted,offset,rnetaddon::ChunkBytes,chunk,error)
                    || !download.write(index,offset,chunk.data(),chunk.size(),error)) {ok=false;break;}
                offset+=chunk.size();
            } while(offset<wanted.size);
            if(!ok || !download.finishFile(index,error)) {errorClient(error);return;}
            break;
        }
    }
    incomingIndex=download.nextMissingFile();incomingOffset=0;
    if(incomingIndex==requiredProfile.files.size())
    { if(!download.finish(error) || !scheduleActivation(absolute(download.directory()),error)) errorClient(error);return; }
    receiving=true;receivingMap=false;lastProgress=timerGetTicks();requestNext();
}
}

bool rnetAddonActivateProfile(const rnetaddon::Profile& profile,const std::string& directory,std::string& error)
{
    if(!rnetaddon::validate(profile,error) || (!profile.files.empty() && !rnetaddon::matches(directory,profile,error))) return false;
    for(auto const& file:profile.files)
        if(directory.size()+1+file.path.size()>=BMAX_PATH)
        {error="Addon resource path exceeds the engine's filename limit";return false;}
    activationProfile=profile;activationDirectory=directory;activated=true;
    for(auto const& file:profile.files)
    {
        auto const source=directory+"/"+file.path;
        activatedPaths[lower(file.path)]=source;
        activatedPaths[lower(portable(source))]=source;
    }
#ifndef USE_PHYSFS
    g_resolveReadPath=resolveActivated;
#else
    error="Downloaded addon activation requires the native resource filesystem";return false;
#endif
    return true;
}
const std::string& rnetAddonActiveDirectory() {return localDirectory;}

namespace rnet { namespace content {
bool earlyArguments(int& argc,const char* const*& argv,std::string& error)
{
    if(!rnetAddonRestartInitialize(argc,argv,error)) return false;
    rnetTrackGroup=trackGroup;
    rnetPreloadArchives=preloadArchives;
#ifndef USE_PHYSFS
    // A plain base-game client can also receive a custom map. Give the VFS
    // the verified absolute cache filename directly instead of rebasing it
    // against each ordinary game search directory.
    g_resolveReadPath=resolveActivated;
#endif
    return true;
}
void captureArguments(int argc,const char* const* argv)
{
    if(captured)return;
    captured=true;
    rnetAddonRestartRememberStartup(argc,argv);
    for(int i=1;i<argc;++i)
    {
        std::string option=argv[i];if(option.empty()||option[0]!='-')continue;
        option=option.substr(1);auto equal=option.find('=');std::string value;
        if(equal!=std::string::npos){value=option.substr(equal+1);option.resize(equal);}
        option=lower(option);
        if(option=="g"||option=="grp"||option=="gamegrp")
        {
            if(equal==std::string::npos&&i+1<argc)value=argv[++i];
            if(!value.empty())trustedArchives.insert(lower(portable(value)));
        }
        else if(option=="j"||option=="game_dir")
        {
            if(equal==std::string::npos && i+1<argc)value=argv[++i];
            auto root=absolute(value);if(!root.empty()&&directory(root))roots.push_back(root);
            if(option=="game_dir")localDirectory=root;
        }
        else if(option=="h"||option=="mh")
        {
            if(equal==std::string::npos && i+1<argc)value=argv[++i];
            auto path=absolute(value);auto slash=path.find_last_of('/');
            if(slash!=std::string::npos)
            {
                auto root=path.substr(0,slash);
                if(root!=absolute(".") && std::find(roots.begin(),roots.end(),root)==roots.end())roots.push_back(root);
            }
        }
    }
}
void trackGroup(const char* path,bool base,bool preload)
{
    if(finalized||!path||!*path)return;
    auto source=resolve(path);
    if(source.empty()) {initializationError="Cannot identify loaded archive: "+leafName(path);return;}
    groups.push_back({source,base||original(source)||original(path),preload,base});
}
void trackFile(const char* path)
{
    if(finalized||!path||!*path)return;
    // Entries read from an archive already have a precise archive identity.
    auto file=kopen4loadfrommod(path,0);if(file==buildvfs_kfd_invalid)return;
#ifndef USE_PHYSFS
    bool const packed=kfileparent(file)!=nullptr;
#else
    bool const packed=false;
#endif
    kclose(file);if(packed)return;
    auto source=resolve(path);if(source.empty())return;
    if(std::find(trackedFiles.begin(),trackedFiles.end(),source)==trackedFiles.end())trackedFiles.push_back(source);
}
void captureSelections()
{
    if(finalized||selectionsCaptured)return;
    selectionsCaptured=true;
    selected.clear();
    auto add=[&](Kind kind,const char* path){if(path&&*path)selected.push_back({kind,path});};
    add(Kind::Script,G_ConFile());
    for(char const* m:g_scriptModules)add(Kind::ScriptModule,m);
    // Absent default DEFs do not select a nonexistent resource.
    auto def=kopen4loadfrommod(G_DefFile(),0);
    if(def!=buildvfs_kfd_invalid)kclose(def);
    if(def!=buildvfs_kfd_invalid||g_defNamePtr)add(Kind::Definition,G_DefFile());
    for(char const* m:g_defModules)add(Kind::DefinitionModule,m);
}
bool preloadArchives(std::string& error)
{
    if(!activated||preloaded)return true;
    preloaded=true;
    for(auto const& s:activationProfile.selections)if(s.kind==Kind::PreloadArchive)
    {
        auto const entry=std::find_if(activationProfile.files.begin(),activationProfile.files.end(),
            [&](const rnetaddon::File& f){return f.path==s.path;});
        std::string const source=entry==activationProfile.files.end()?resolve(s.path):activationDirectory+"/"+s.path;
        if(initgroupfile(source.c_str())<0){error="Cannot load verified addon archive: "+s.path;initializationError=error;return false;}
        trackGroup(source.c_str(),false,true);
    }
    return true;
}
void startup()
{
    if(finalized)return;
    if(!initializationError.empty())
    {finalized=true;OSD_Printf("Multiplayer content unavailable: %s\n",initializationError.c_str());return;}
    if(selected.empty())captureSelections();
    localProfile.name="Rednukem addon";
    if(!activated && g_modDir[0]!='/' && *g_modDir)
    {
        auto root=absolute(g_modDir);if(!root.empty()){roots.push_back(root);localDirectory=root;}
    }
    // Main archives and their commercial dependency chain are always local.
    for(auto const& group:groups)if(group.base)
    {
        rnetaddon::File file;std::string error;
        if(!rnetaddon::inspectFile(group.path,file,error)){initializationError=error;break;}
        netassets::Asset asset;asset.key="base/"+std::to_string(bases.size());asset.name=leafName(group.path);
        asset.size=file.size;asset.md5=file.md5;bases.push_back(asset);
    }
    if(!initializationError.empty())
    {finalized=true;OSD_Printf("Multiplayer content unavailable: %s\n",initializationError.c_str());return;}
    if(activated)
    {
        localProfile=activationProfile;localDirectory=activationDirectory;
        if(!localProfile.files.empty() && !rnetaddon::matches(localDirectory,localProfile,initializationError)) {}
        for(auto const& file:localProfile.files)sources.push_back(localDirectory+"/"+file.path);
    }
    else
    {
        bool commandArchivesStarted=false;
        for(auto const& group:groups)if(!group.primary)
        {
            auto const name=group.base?leafName(group.path):logical(group.path,group.path);
            if(!group.base&&!addFile(name,group.path,initializationError))break;
            if(!group.preload)commandArchivesStarted=true;
            // Autoloads belonging to a command-line archive occur after that
            // archive, and must stay there when the profile is activated.
            localProfile.selections.push_back({group.preload&&!commandArchivesStarted?Kind::PreloadArchive:Kind::Archive,name});
        }
        if(initializationError.empty())for(auto const& item:selected)
        {
            auto const source=resolve(item.path),name=logical(item.path,source);
            localProfile.selections.push_back({item.kind,name});
            if(!source.empty()&&!addFile(name,source,initializationError))break;
        }
        if(initializationError.empty())for(auto const& path:trackedFiles)if(!addFile(logical(path,path),path,initializationError))break;
        // Explicit addon roots carry loose artwork, sounds, nested includes and
        // future map-rotation files. Private cfg/ini/executables are excluded.
        if(initializationError.empty())for(auto const& root:roots)
            if(root!=absolute(".")&&!scan(root,"",0,initializationError))break;
        // File ordering is canonical; selection ordering remains load ordering.
        std::vector<std::pair<rnetaddon::File,std::string>> ordered;
        for(size_t i=0;i<sources.size();++i)ordered.push_back({localProfile.files[i],sources[i]});
        std::sort(ordered.begin(),ordered.end(),[](auto const&a,auto const&b){return lower(a.first.path)<lower(b.first.path);});
        localProfile.files.clear();sources.clear();
        for(auto& p:ordered){localProfile.files.push_back(std::move(p.first));sources.push_back(std::move(p.second));}
    }
    if(initializationError.empty())rnetaddon::validate(localProfile,initializationError);
    finalized=true;
    if(!initializationError.empty())OSD_Printf("Multiplayer content unavailable: %s\n",initializationError.c_str());
}
void mapChanged()
{
    outgoing.clear();mapBinding.clear();mapLogical=rnet::mapName();mapAlias.clear();mapBytes.clear();mapSource.clear();mapError.clear();
    if(!rnet::server())return;
    std::string error;
    if(!vfsRead(mapLogical,mapBytes,error,loadedMapSearchFirst)){mapError=error;return;}
    mapFile.path=leafName(mapLogical);mapFile.size=mapBytes.size();mapFile.md5=digest(mapBytes);
    if(!rnetaddon::validPath(mapFile.path)){mapError="Current map has an unsafe portable filename";return;}
    mapSource=mapLogical;
}
void mapLoaded(char searchFirst) {loadedMapSearchFirst=searchFirst&3;}
void offer(int player)
{
    startup();
    if(mapSource!=rnet::mapName())mapChanged();
    if(!initializationError.empty()){rnet::reject(player,initializationError);return;}
    if(!mapError.empty()){rnet::reject(player,mapError);return;}
    std::vector<uint8_t> profile,manifest;std::string error;
    if(!rnetaddon::encode(localProfile,profile,error)||!netassets::encodeManifest(bases,manifest,error))
    {rnet::reject(player,error);return;}
    netwire::Writer w;w.u32(rnet::mapSerial());w.u32(g_scriptcrc);w.text(mapFile.path,rnetaddon::MaxPath);
    write64(w,mapFile.size);w.bytes(mapFile.md5.data(),mapFile.md5.size());
    w.u32(uint32_t(manifest.size()));w.bytes(manifest.data(),manifest.size());
    w.u32(uint32_t(profile.size()));w.bytes(profile.data(),profile.size());
    outgoing[player]=Sending{};outgoing[player].serial=rnet::mapSerial();
    rnet::send(player,Offer,w.data);
}
void handle(int player,uint8_t type,const uint8_t* bytes,size_t size)
{
    if(size>2u*rnetaddon::MaxProfileBytes+1024){if(rnet::server())rnet::reject(player,"Content packet exceeds limit");else errorClient("Content packet exceeds limit");return;}
    netwire::Reader r(bytes,size);
    if(rnet::server())
    {
        auto found=outgoing.find(player);
        if(found==outgoing.end())return;
        uint32_t const serial=r.u32();
        if(serial!=found->second.serial || serial!=rnet::mapSerial())return;
        if(type==ContentReady)
        {
            netassets::Digest md5{};
            if(!r.bytes(md5.data(),md5.size())||!r.end()||md5!=mapFile.md5)
            {rnet::reject(player,"Map identity acknowledgment is invalid");return;}
            found->second.pending=false;
            rnet::contentReady(player);
        }
        else if(type==RequestFile)
        {
            unsigned const kind=r.u8();size_t const index=r.u32();uint64_t const offset=read64(r);
            if(!r.end() || kind>1 || (kind ? index!=0 : index>=localProfile.files.size()))
            {rnet::reject(player,"Invalid content download request");return;}
            auto const& file=kind?mapFile:localProfile.files[index];
            if(offset>file.size || offset%rnetaddon::ChunkBytes || found->second.pending)
            {rnet::reject(player,"Invalid or overlapping content download offset");return;}
            found->second={serial,index,offset,kind!=0,true};
        }
        else rnet::reject(player,"Unexpected client content message");
        return;
    }
    if(!rnet::client()||restartPending())return;
    if(type==Offer)
    {
        startup();
        uint32_t const serial=r.u32();
        uint32_t const scriptCRC=r.u32();
        std::string const name=r.text(rnetaddon::MaxPath);
        rnetaddon::File file;file.path=name;file.size=read64(r);
        if(!r.bytes(file.md5.data(),file.md5.size())||file.size<26||file.size>MaxMapBytes)
        {errorClient("Invalid server map descriptor");return;}
        if(serial!=rnet::mapSerial())return;
        std::string error;
        MapBinding binding;
        if(!binding.announce(rnet::mapName(),name,error)){errorClient(error);return;}
        uint32_t const manifestSize=r.u32();
        if(!r.ok||manifestSize>netassets::MaxManifestBytes||manifestSize>r.size-r.pos)
        {errorClient("Invalid base-game manifest length");return;}
        netwire::Reader mr(r.data+r.pos,manifestSize);r.pos+=manifestSize;
        std::vector<netassets::Asset> requiredBases;
        if(!netassets::decodeManifest(mr,requiredBases,error)){errorClient(error);return;}
        uint32_t const profileSize=r.u32();
        if(!r.ok||profileSize>rnetaddon::MaxProfileBytes||profileSize!=r.size-r.pos)
        {errorClient("Invalid addon manifest length");return;}
        netwire::Reader pr(r.data+r.pos,profileSize);
        rnetaddon::Profile profile;
        if(!rnetaddon::decode(pr,profile,error)){errorClient(error);return;}
        for(auto const& selection:profile.selections)
            if(selection.kind==Kind::Archive||selection.kind==Kind::PreloadArchive)
            {
                bool const included=std::any_of(profile.files.begin(),profile.files.end(),
                    [&](const rnetaddon::File& f){return f.path==selection.path;});
                bool const base=std::any_of(requiredBases.begin(),requiredBases.end(),
                    [&](const netassets::Asset& f){return lower(f.name)==lower(selection.path);});
                if(!included&&!base){errorClient("Addon archive is absent from the verified manifest: "+selection.path);return;}
            }
        auto const issues=netassets::compareManifest(requiredBases,bases);
        if(!issues.empty() || requiredBases.size()!=bases.size())
        {
            OSD_Printf("Server requires a different base game. Commercial game data is never downloaded.\n");
            for(auto const& issue:issues)
                OSD_Printf("  %s: required MD5 %s, size %llu; local %s\n",issue.required.name.c_str(),
                    netassets::hex(issue.required.md5).c_str(),(unsigned long long)issue.required.size,
                    issue.kind==netassets::Missing?"missing":netassets::hex(issue.local.md5).c_str());
            errorClient("Base game mismatch; install the matching game edition shown in the console");return;
        }
        download.cancel();mapCache.cancel();mapBytes.clear();mapAlias.clear();
        requiredProfile=std::move(profile);mapFile=file;
        mapBinding=std::move(binding);mapLogical=mapBinding.lookup();
        offeredSerial=serial;requiredScriptCRC=scriptCRC;receivedOffer=true;receiving=false;
        prepareAddon();return;
    }
    if(type!=FileData||!receivedOffer||!receiving){errorClient("Unexpected server content message");return;}
    uint32_t const serial=r.u32();unsigned const kind=r.u8();size_t const index=r.u32();
    uint64_t const offset=read64(r);uint32_t const length=r.u32();
    if(serial!=offeredSerial)return;
    auto const& file=receivingMap?mapFile:requiredProfile.files[incomingIndex];
    if(!r.ok||kind!=unsigned(receivingMap)||index!=incomingIndex||offset!=incomingOffset
        ||length!=r.size-r.pos||length>rnetaddon::ChunkBytes||offset>file.size
        ||length!=std::min<uint64_t>(rnetaddon::ChunkBytes,file.size-offset))
    {errorClient("Invalid, out-of-order, or oversized file chunk");return;}
    std::string error;
    auto& cache=receivingMap?mapCache:download;
    if(!cache.write(index,offset,r.data+r.pos,length,error)){errorClient(error);return;}
    if(receivingMap)mapBytes.insert(mapBytes.end(),r.data+r.pos,r.data+r.pos+length);
    incomingOffset+=length;lastProgress=timerGetTicks();
    if(incomingOffset==file.size)
    {
        if(receivingMap && !validateMap(mapBytes,error)){errorClient(error);return;}
        if(!cache.finishFile(index,error)){errorClient(error);return;}
        if(receivingMap)
        {
            if(!cache.finish(error)){errorClient(error);return;}
            if(!locateCachedMap(error)){errorClient(error);return;}
            mapBytes.clear();readyMap();return;
        }
        OSD_Printf("Downloaded %s (MD5 %s)\n",file.path.c_str(),netassets::hex(file.md5).c_str());
        incomingIndex=download.nextMissingFile();incomingOffset=0;
        if(incomingIndex==requiredProfile.files.size())
        {
            if(!download.finish(error)||!scheduleActivation(absolute(download.directory()),error))errorClient(error);
            return;
        }
        requestNext();
    }
    else if(incomingOffset%(4*rnetaddon::ChunkBytes)==0)requestNext();
}
void pump()
{
    if(rnet::client())
    {
        if(receiving&&uint32_t(timerGetTicks()-lastProgress)>60000)errorClient("Download stalled for 60 seconds; reconnect to resume");
        return;
    }
    if(!rnet::server())return;
    for(auto& item:outgoing)
    {
        auto& send=item.second;
        if(!send.pending)continue;
        if(!rnet::connected(item.first)||send.serial!=rnet::mapSerial()){send.pending=false;continue;}
        auto const& file=send.map?mapFile:localProfile.files[send.index];
        for(unsigned window=0;window<4&&send.pending;++window)
        {
            std::vector<uint8_t> chunk;std::string error;
            if(send.map)
            {
                size_t const length=size_t(std::min<uint64_t>(rnetaddon::ChunkBytes,file.size-send.offset));
                chunk.assign(mapBytes.begin()+size_t(send.offset),mapBytes.begin()+size_t(send.offset)+length);
            }
            else if(!rnetaddon::readChunk(sources[send.index],file,send.offset,rnetaddon::ChunkBytes,chunk,error))
            {send.pending=false;rnet::reject(item.first,error);break;}
            netwire::Writer w;w.u32(send.serial);w.u8(send.map?1:0);w.u32(uint32_t(send.index));write64(w,send.offset);
            w.u32(uint32_t(chunk.size()));w.bytes(chunk.data(),chunk.size());
            rnet::send(item.first,FileData,w.data);
            send.offset+=chunk.size();
            if(send.offset==file.size)send.pending=false;
        }
        send.pending=false; // Each request grants at most four chunks.
    }
}
const char* resolveMap(const char* name)
{
    if(name&&rnet::playback())
    {
        std::string const path=portable(name);
        auto const slash=path.find_last_of('/');
        if(slash!=std::string::npos&&path.find("/.rednukem-addon-cache/")!=std::string::npos)
        {
            rnetaddon::Profile profile;std::string error;
            if(rnetaddon::loadCached(path.substr(0,slash),profile,error)&&profile.files.size()==1
                &&profile.selections.empty()&&profile.files[0].path==path.substr(slash+1)
                &&profile.files[0].size<=MaxMapBytes)
            {
                std::vector<uint8_t> bytes,chunk;uint64_t offset=0;
                while(offset<profile.files[0].size)
                {
                    if(!rnetaddon::readChunk(path,profile.files[0],offset,rnetaddon::ChunkBytes,chunk,error))break;
                    bytes.insert(bytes.end(),chunk.begin(),chunk.end());offset+=chunk.size();
                }
                if(offset==profile.files[0].size&&validateMap(bytes,error))
                {mapAlias=path;mapLogical=path;mapFile=profile.files[0];return mapAlias.c_str();}
            }
            rnet::fail("Recorded map cache is missing, changed, or invalid: "+error);
            return "";
        }
    }
    if(rnet::client())
    {
        if(auto const* verified=mapBinding.resolve(name))return verified;
        errorClient("Map was not verified for the current server announcement");
        return "";
    }
    return name;
}
bool validateMapForLoad(const char* name,std::string& error,char searchFirst)
{
    if(!name||!*name){error="No verified multiplayer map is available";return false;}
    std::vector<uint8_t> bytes;
    if(!vfsRead(name,bytes,error,searchFirst))return false;
    if(rnet::client()&&(bytes.size()!=mapFile.size||digest(bytes)!=mapFile.md5))
    {error="The map selected by the game differs from the verified server map";return false;}
    return true;
}
void shutdown()
{
    outgoing.clear();download.cancel();mapCache.cancel();receiving=false;receivedOffer=false;mapBytes.clear();mapAlias.clear();mapBinding.clear();
}
bool restartPending(){return rnetAddonRestartPending();}
bool restartReady(){return rnetAddonRestartReady();}
bool restartActive(){return rnetAddonRestartActive();}
bool launchRestart(std::string& error){return rnetAddonRestartLaunch(error);}
} }
