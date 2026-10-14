// CPU regression checks for map geometry supplied to point-light shadow maps.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#include "build.h"
#include "realtime_lighting.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <initializer_list>
#include <utility>

sectortype sectorStorage[MAXSECTORS + M32_FIXME_SECTORS];
walltype wallStorage[MAXWALLS + M32_FIXME_WALLS];
sectortype *sector=sectorStorage;
walltype *wall=wallStorage;
uint32_t sectorchanged[MAXSECTORS + M32_FIXME_SECTORS];
uint32_t wallchanged[MAXWALLS + M32_FIXME_WALLS];
int16_t numsectors = 0, numwalls = 0;
void *_xmalloc(bsize_t size) { return malloc(size); }
void *_xrealloc(void *ptr, bsize_t size) { return realloc(ptr, size); }
void _xfree(void *ptr) { free(ptr); }
int16_t yax_getbunch(int16_t, int16_t) { return -1; }
void getzsofslopeptr(usectorptr_t sec, int32_t, int32_t y, int32_t *cz, int32_t *fz)
{
    // An exact linear height provider isolates mesh splitting/cache behavior
    // from the engine's fixed-point slope calculation and game initialization.
    *cz = sec->ceilingz + (sec->ceilingstat & 2 ? sec->ceilingheinum * y : 0);
    *fz = sec->floorz + (sec->floorstat & 2 ? sec->floorheinum * y : 0);
}
void require(bool condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", name); exit(1); }
}
void contour(std::initializer_list<std::pair<int,int>> points)
{
    int first = numwalls;
    for (auto p : points)
    {
        auto &w = wall[numwalls];
        w.x = p.first; w.y = p.second; w.point2 = ++numwalls; w.nextsector = w.nextwall = -1; w.cstat = 0;
    }
    wall[numwalls-1].point2 = first;
}
void reset()
{
    numwalls = 0; numsectors = 1;
    auto &s = sector[0];
    s.wallptr = 0; s.wallnum = 0; s.ceilingz = 0; s.floorz = 1600;
    s.ceilingstat = s.floorstat = s.ceilingheinum = s.floorheinum = 0;
}
double triangleArea(const RealtimeShadowVertex *v)
{
    double a[3], b[3], c[3];
    for (int i=0;i<3;++i) { a[i]=v[1].position[i]-v[0].position[i]; b[i]=v[2].position[i]-v[0].position[i]; }
    for (int i=0;i<3;++i) c[i]=a[(i+1)%3]*b[(i+2)%3]-a[(i+2)%3]*b[(i+1)%3];
    return sqrt(c[0]*c[0]+c[1]*c[1]+c[2]*c[2])*.5;
}
double planeArea(const std::vector<RealtimeShadowVertex>&mesh, int axis, float coordinate)
{
    double sum=0;
    for(size_t i=0;i<mesh.size();i+=3)
        if (mesh[i].position[axis]==coordinate && mesh[i+1].position[axis]==coordinate && mesh[i+2].position[axis]==coordinate)
            sum+=triangleArea(&mesh[i]);
    return sum;
}
int main()
{
    RealtimeLight light = { {50,50,50,1000}, {1,1,1,1} };
    std::vector<RealtimeShadowVertex> mesh;
    reset(); contour({{0,0},{100,0},{100,100},{0,100}}); sector[0].wallnum=numwalls;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(mesh.size()==36 && planeArea(mesh,2,100)==10000,"rectangle");
    reset(); contour({{0,0},{100,0},{100,40},{40,40},{40,100},{0,100}}); sector[0].wallnum=numwalls;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,100)==6400,"concave floor");
    reset(); contour({{0,0},{100,0},{100,100},{0,100}}); contour({{30,30},{30,70},{70,70},{70,30}}); sector[0].wallnum=numwalls;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,100)==8400,"hole floor");
    for (size_t i=0;i<mesh.size();i+=3)
    {
        if (mesh[i].position[2]!=100 || mesh[i+1].position[2]!=100 || mesh[i+2].position[2]!=100) continue;
        float const x=(mesh[i].position[0]+mesh[i+1].position[0]+mesh[i+2].position[0])/3;
        float const y=(mesh[i].position[1]+mesh[i+1].position[1]+mesh[i+2].position[1])/3;
        require(!(x>30 && x<70 && y>30 && y<70),"hole remains empty");
    }
    // Connect the two contours into an open notch without changing any XY.
    wall[0].point2=4; wall[7].point2=1;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,100)==6300,"point2 cache invalidation");
    wall[0].point2=1; wall[7].point2=4;
    wall[1].x=wall[2].x=200;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,100)==18400,"XY cache invalidation");
    sector[0].floorz=3200;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,200)==18400,"floor moves without retessellation");
    sector[0].ceilingstat=sector[0].floorstat=1;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,2,200)==0 && planeArea(mesh,2,0)==0,"skies excluded");
    reset(); contour({{0,0},{100,0},{100,100},{0,100}}); sector[0].wallnum=4;
    contour({{100,0},{200,0},{200,100},{100,100}}); numsectors=2;
    sector[1]=sector[0]; sector[1].wallptr=4; sector[1].ceilingz=400; sector[1].floorz=1200;
    wall[1].nextsector=1; wall[1].nextwall=7; wall[7].nextsector=0; wall[7].nextwall=1;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,0,100)==5000,"portal structural steps preserve opening");
    wall[1].cstat=CSTAT_WALL_MASKED|CSTAT_WALL_TRANSLUCENT;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,0,100)==5000,"masked middle excluded with steps retained");
    sector[1].ceilingz=sector[1].floorz=800;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(planeArea(mesh,0,100)==10000,"closed door geometry updated");
    sector[1].ceilingz=-800; sector[1].floorz=1600; sector[1].ceilingstat=2; sector[1].ceilingheinum=16;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(std::abs(planeArea(mesh,0,100)-2500)<.01,"crossing slopes clipped at crossover");
    light.positionRadius[0]=10000; light.positionRadius[3]=1;
    buildRealtimeShadowMesh(&light,1,mesh);
    require(mesh.empty(),"distant geometry culled");
    puts("PASS: shadow mesh rectangles, concavity, holes, topology changes, moving planes, skies, portals, masked spans, closed doors, crossing slopes and light culling");
}
