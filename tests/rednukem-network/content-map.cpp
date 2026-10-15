// Validate untrusted binary Build maps before invoking the legacy map loader.
#include "rnet_map.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
using Bytes=std::vector<uint8_t>;
static void word(Bytes& b,size_t p,unsigned v) {b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
static Bytes rooms(unsigned sectors,unsigned sprites,unsigned version)
{
    size_t const wallStart=24+sectors*40,spriteCount=wallStart+sectors*3*32;
    Bytes b(spriteCount+2+sprites*44);
    word(b,0,version);word(b,20,sectors);word(b,wallStart-2,sectors*3);word(b,spriteCount,sprites);
    for(unsigned i=0;i<sectors;++i)
    {
        word(b,22+i*40,i*3);word(b,24+i*40,3);
        for(unsigned j=0;j<3;++j)
        {
            size_t const p=wallStart+(i*3+j)*32;
            word(b,p+8,i*3+(j+1)%3);word(b,p+10,65535);word(b,p+12,65535);
        }
    }
    return b;
}
int main(int argc,char** argv)
{
    std::string error;
    Bytes b(22+40+2+4*32+2+44);
    word(b,0,7);word(b,18,0);word(b,20,1);word(b,22+2,4);word(b,62,4);
    for(unsigned i=0;i<4;++i)
    {size_t p=64+i*32;word(b,p+8,(i+1)%4);word(b,p+10,65535);word(b,p+12,65535);}
    word(b,192,1);
    assert(rnet::content::validateMap(b,error));
    for(size_t n=0;n<b.size();++n)
    {Bytes truncated(b.begin(),b.begin()+n);assert(!rnet::content::validateMap(truncated,error));}
    for(auto field:std::vector<std::pair<size_t,unsigned>>{{0,6},{18,1},{20,1025},{24,0},{62,8193},
        {64+8,4},{64+10,0},{64+12,0},{64+16,30720},{64+18,65535},{192,4097},{194+24,1},{194+26,1024}})
    {Bytes changed=b;word(changed,field.first,field.second);assert(!rnet::content::validateMap(changed,error));}
    Bytes trailing=b;trailing.push_back(0);assert(!rnet::content::validateMap(trailing,error));
    for(unsigned version:{8,9})
    {
        Bytes extended=rooms(2731,4097,version);
        assert(rnet::content::validateMap(extended,error));
        word(extended,0,7);assert(!rnet::content::validateMap(extended,error));
    }
    Bytes tror=rooms(2,0,9);size_t const walls=104;
    // Sector 0's ceiling is joined to sector 1's floor. Each upper wall
    // carries its reciprocal lower-wall index in the v9 lotag/extra fields.
    word(tror,22+12,1024);word(tror,62+14,1024);tror[22+22]=5;tror[62+30]=5;
    for(unsigned j=0;j<3;++j)
    {
        word(tror,walls+j*32+14,1024);word(tror,walls+j*32+26,j+3);
        word(tror,walls+(j+3)*32+14,2048);word(tror,walls+(j+3)*32+30,j);
    }
    assert(rnet::content::validateMap(tror,error));
    for(auto field:std::vector<std::pair<size_t,unsigned>>{{walls+26,6},{walls+26,65535},
        {walls+26,0},{walls+3*32+30,2},{walls+3*32+14,0},{62+14,0}})
    {Bytes changed=tror;word(changed,field.first,field.second);assert(!rnet::content::validateMap(changed,error));}
    tror[62+30]=6;assert(!rnet::content::validateMap(tror,error));
    word(b,0,10);assert(!rnet::content::validateMap(b,error));word(b,0,7);
    std::mt19937 random(0x44bead);
    for(unsigned i=0;i<10000;++i)
    {
        Bytes changed=b;
        word(changed,0,7+i%3);
        for(unsigned j=0;j<8;++j)changed[random()%changed.size()]=uint8_t(random());
        (void)rnet::content::validateMap(changed,error);
    }
    for(int i=1;i<argc;++i)
    {
        std::ifstream file(argv[i],std::ios::binary);assert(file);
        Bytes map(std::istreambuf_iterator<char>(file),{});
        if(!rnet::content::validateMap(map,error)){std::fprintf(stderr,"%s: %s\n",argv[i],error.c_str());return 1;}
    }
    std::puts("Build v7/v8/v9 map validation: truncation, range, topology, TROR and 10000 mutation cases passed");
}
