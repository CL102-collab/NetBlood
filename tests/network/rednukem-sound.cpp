// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../source/rr/src/rnet_sound_model.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
using rnet::audio::durationMs;
void put(std::vector<uint8_t>& d,size_t at,uint64_t v,unsigned count) {for(unsigned i=0;i<count;++i)d[at+i]=uint8_t(v>>(i*8));}
int main() {
    std::vector<uint8_t> wav(44+8000,128);
    std::memcpy(wav.data(),"RIFF",4);put(wav,4,wav.size()-8,4);std::memcpy(wav.data()+8,"WAVEfmt ",8);
    put(wav,16,16,4);put(wav,20,1,2);put(wav,22,1,2);put(wav,24,8000,4);put(wav,28,8000,4);put(wav,32,1,2);put(wav,34,8,2);
    std::memcpy(wav.data()+36,"data",4);put(wav,40,8000,4);
    assert(durationMs(wav.data(),wav.size())==1000);
    for(size_t n=0;n<wav.size();++n)assert(durationMs(wav.data(),n)==0);
    std::vector<uint8_t> voc(26+4+2+8000+1,128);
    std::memcpy(voc.data(),"Creative Voice File\x1a",20);put(voc,20,26,2);voc[26]=1;put(voc,27,8002,3);voc[30]=131;voc[31]=0;voc.back()=0;
    assert(durationMs(voc.data(),voc.size())==1000);
    voc[31]=1;assert(durationMs(voc.data(),voc.size())==0);voc[31]=0;
    std::vector<uint8_t> flac(42,0);std::memcpy(flac.data(),"fLaC",4);flac[4]=128;flac[7]=34;
    uint64_t info=(uint64_t(44100)<<44)|88200;for(unsigned i=0;i<8;++i)flac[18+i]=uint8_t(info>>(56-i*8));
    assert(durationMs(flac.data(),flac.size())==2000);
    std::vector<uint8_t> ogg(44,0);std::memcpy(ogg.data(),"OggS",4);put(ogg,6,48000,8);ogg[26]=1;ogg[27]=16;ogg[28]=1;
    std::memcpy(ogg.data()+29,"vorbis",6);put(ogg,40,48000,4);assert(durationMs(ogg.data(),ogg.size())==1000);
    // Untrusted mod resources must never read outside the provided bytes.
    uint32_t rng=1;
    for(auto data:{wav,voc,flac,ogg})for(unsigned i=0;i<20000;++i) {
        rng=rng*1664525+1013904223;size_t off=rng%data.size();auto saved=data[off];data[off]=uint8_t(rng>>24);
        rng=rng*1664525+1013904223;durationMs(data.data(),rng%(data.size()+1));data[off]=saved;
    }
    std::puts("Rednukem sound duration: WAV/VOC/Vorbis/FLAC and 80000 malformed/truncated resource checks passed");
}
