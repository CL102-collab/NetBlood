// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace rnet { namespace audio {
// Read duration without opening a mixer. Dedicated CON soundonce/ifsound and
// ambient actors must advance exactly as they do with a working audio device.
inline uint64_t little(const uint8_t* p, unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;++i) v |= uint64_t(p[i])<<(i*8); return v;
}
inline uint32_t durationMs(const uint8_t* p,size_t n) {
    auto tag=[&](size_t off,const char* s,size_t len) {return off<=n && len<=n-off && !std::memcmp(p+off,s,len);};
    double seconds=0;
    if(tag(0,"RIFF",4)&&tag(8,"WAVE",4)) {
        uint32_t rate=0; uint64_t bytes=0;
        for(size_t off=12;off+8<=n;) {
            size_t size=little(p+off+4,4); if(size>n-off-8) return 0;
            if(tag(off,"fmt ",4)&&size>=16) rate=uint32_t(little(p+off+16,4));
            if(tag(off,"data",4)) bytes+=size;
            off+=8+size+(size&1);
        }
        if(rate) seconds=double(bytes)/rate;
    } else if(tag(0,"Creative Voice File\x1a",20)&&n>=26) {
        size_t off=little(p+20,2); uint32_t rate=0,channels=1,bits=8;
        bool extended=false; double repeatStart=0; unsigned repeats=0;
        while(off<n&&p[off]) {
            if(n-off<4) return 0;
            unsigned type=p[off]; size_t size=little(p+off+1,3);off+=4;
            if(size>n-off) return 0;
            const auto* b=p+off;size_t bytes=0;
            if(type==1&&size>=2) {
                if(!extended) {rate=1000000u/(256u-b[0]);channels=1;}
                bits=8; extended=false; if(b[1]==0) bytes=size-2;
            } else if(type==2) bytes=size;
            else if(type==3&&size>=3) seconds+=double(little(b,2)+1)/(1000000u/(256u-b[2]));
            else if(type==6&&size>=2) {repeats=unsigned(little(b,2));repeatStart=seconds;}
            else if(type==7) {
                if(repeats==65535) return UINT32_MAX;
                seconds+=(seconds-repeatStart)*repeats;repeats=0;
            } else if(type==8&&size>=4) {
                channels=unsigned(b[3])+1;rate=256000000u/(channels*(65536u-unsigned(little(b,2))));extended=true;
            } else if(type==9&&size>=12) {
                rate=uint32_t(little(b,4));bits=b[4];channels=b[5];
                if((bits==8||bits==16)&&channels&&channels<=2) bytes=size-12;
            }
            if(rate&&bits&&channels) seconds+=double(bytes)*8/(double(rate)*bits*channels);
            off+=size;
        }
    } else if(tag(0,"fLaC",4)&&n>=42&&(p[4]&127)==0&&little(p+5,3)==0x220000) {
        uint64_t info=0;for(unsigned i=18;i<26;++i) info=(info<<8)|p[i];
        uint32_t rate=uint32_t(info>>44);uint64_t samples=info&UINT64_C(0xfffffffff);
        if(rate)seconds=double(samples)/rate;
    } else if(tag(0,"OggS",4)) {
        uint32_t rate=0,serial=0;uint64_t samples=0;
        for(size_t off=0;off+27<=n;) {
            if(!tag(off,"OggS",4)||p[off+4]!=0)return 0;
            size_t count=p[off+26],start=off+27+count;if(start>n)return 0;
            size_t bytes=0;for(size_t i=0;i<count;++i)bytes+=p[off+27+i];if(bytes>n-start)return 0;
            if(!rate&&bytes>=16&&p[start]==1&&tag(start+1,"vorbis",6)) {
                rate=uint32_t(little(p+start+12,4));serial=uint32_t(little(p+off+14,4));
            }
            uint64_t granule=little(p+off+6,8);
            if(rate&&little(p+off+14,4)==serial&&granule!=UINT64_MAX)samples=std::max(samples,granule);
            off=start+bytes;
        }
        if(rate)seconds=double(samples)/rate;
    }
    if(!(seconds>0))return 0;
    return uint32_t(std::min(86400000.,seconds*1000.+0.999999));
}
} }
