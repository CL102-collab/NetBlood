// Build world geometry shared by Polymost and Vulkan point-light shadow maps.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#include "realtime_lighting.h"

#ifdef USE_OPENGL
#include "build.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <setjmp.h>
#include <vector>

namespace
{
// Polymer includes the same implementation in the global namespace. Keep this
// instance private, including its types, so both backends work without Polymer.
namespace shadowTess
{
#define LIBTESS2_IMPLEMENTATION
#include "libtess2.h"
#undef LIBTESS2_IMPLEMENTATION
}

struct WallPoint
{
    int32_t x, y, point2;
};
struct Point
{
    float x, y;
};
struct SectorGeometry
{
    int wallptr = -1;
    std::vector<WallPoint> walls;
    std::vector<Point> triangles;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool tessellated = false;
};
std::vector<SectorGeometry> sectorGeometry;

bool intersectsLights(double const minimum[3], double const maximum[3],
                      RealtimeLight const *lights, unsigned lightCount)
{
    for (unsigned light = 0; light < lightCount; ++light)
    {
        auto const &p = lights[light].positionRadius;
        if (!(p[3] > 0.f) || !std::isfinite(p[3])) continue;
        double distance2 = 0;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            double const distance = std::max(minimum[axis] - p[axis], std::max(0., double(p[axis]) - maximum[axis]));
            distance2 += distance * distance;
        }
        if (distance2 <= double(p[3]) * p[3]) return true;
    }
    return false;
}

void refreshGeometry(int sectorIndex, SectorGeometry &geometry)
{
    auto const &sec = sector[sectorIndex];
    bool changed = geometry.wallptr != sec.wallptr || geometry.walls.size() != unsigned(sec.wallnum);
    if (!changed)
        for (int i = 0; i < sec.wallnum; ++i)
        {
            auto const &old = geometry.walls[i];
            auto const &current = wall[sec.wallptr + i];
            if (old.x != current.x || old.y != current.y || old.point2 != current.point2)
            {
                changed = true;
                break;
            }
        }
    if (!changed) return;

    geometry.wallptr = sec.wallptr;
    geometry.walls.resize(sec.wallnum);
    geometry.triangles.clear();
    geometry.tessellated = false;
    geometry.minX = geometry.maxX = wall[sec.wallptr].x;
    geometry.minY = geometry.maxY = wall[sec.wallptr].y;
    for (int i = 0; i < sec.wallnum; ++i)
    {
        auto const &current = wall[sec.wallptr + i];
        geometry.walls[i] = { current.x, current.y, current.point2 };
        geometry.minX = std::min(geometry.minX, double(current.x));
        geometry.minY = std::min(geometry.minY, double(current.y));
        geometry.maxX = std::max(geometry.maxX, double(current.x));
        geometry.maxY = std::max(geometry.maxY, double(current.y));
    }
}

void triangulate(SectorGeometry &geometry)
{
    geometry.tessellated = true;
    auto *tess = shadowTess::tessNewTess(nullptr);
    if (!tess) return;
    std::vector<bool> visited(geometry.walls.size());
    std::vector<Point> contour;
    bool valid = true;
    for (unsigned start = 0; start < geometry.walls.size() && valid; ++start)
    {
        if (visited[start]) continue;
        contour.clear();
        unsigned next = start;
        do
        {
            if (next >= geometry.walls.size() || visited[next])
            {
                valid = false;
                break;
            }
            visited[next] = true;
            auto const &point = geometry.walls[next];
            contour.push_back({ float(point.x), float(point.y) });
            next = unsigned(point.point2 - geometry.wallptr);
        } while (next != start);
        if (valid && contour.size() >= 3)
            shadowTess::tessAddContour(tess, 2, contour.data(), sizeof(Point), int(contour.size()));
    }
    float const normal[3] = { 0, 0, 1 };
    if (valid && shadowTess::tessTesselate(tess, shadowTess::TESS_WINDING_ODD,
                                         shadowTess::TESS_POLYGONS, 3, 2, normal))
    {
        auto const *points = shadowTess::tessGetVertices(tess);
        auto const *indices = shadowTess::tessGetElements(tess);
        int const count = shadowTess::tessGetElementCount(tess);
        int const pointCount = shadowTess::tessGetVertexCount(tess);
        geometry.triangles.reserve(size_t(count) * 3);
        for (int triangle = 0; triangle < count; ++triangle)
        {
            auto const *index = indices + triangle * 3;
            if (unsigned(index[0]) >= unsigned(pointCount) || unsigned(index[1]) >= unsigned(pointCount) ||
                unsigned(index[2]) >= unsigned(pointCount)) continue;
            for (unsigned corner = 0; corner < 3; ++corner)
                geometry.triangles.push_back({ points[index[corner] * 2], points[index[corner] * 2 + 1] });
        }
    }
    shadowTess::tessDeleteTess(tess);
}

int32_t coordinate(float value)
{
    return int32_t(std::max(double(std::numeric_limits<int32_t>::min()),
                           std::min(double(std::numeric_limits<int32_t>::max()), std::round(double(value)))));
}

void emitTriangle(RealtimeShadowVertex const &a, RealtimeShadowVertex const &b, RealtimeShadowVertex const &c,
                  RealtimeLight const *lights, unsigned lightCount, std::vector<RealtimeShadowVertex> &vertices)
{
    double minimum[3], maximum[3];
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        minimum[axis] = std::min(a.position[axis], std::min(b.position[axis], c.position[axis]));
        maximum[axis] = std::max(a.position[axis], std::max(b.position[axis], c.position[axis]));
    }
    if (!intersectsLights(minimum, maximum, lights, lightCount)) return;
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
}

bool castsPlane(int sectorIndex, bool floor)
{
    auto const &sec = sector[sectorIndex];
    if ((floor ? sec.floorstat : sec.ceilingstat) & 1) return false; // Parallax sky.
#ifdef YAX_ENABLE
    if (yax_getbunch(sectorIndex, floor ? YAX_FLOOR : YAX_CEILING) >= 0) return false;
#endif
    return true;
}

void emitPlanes(int sectorIndex, SectorGeometry &geometry, RealtimeLight const *lights,
                unsigned lightCount, std::vector<RealtimeShadowVertex> &vertices)
{
    bool const ceiling = castsPlane(sectorIndex, false), floor = castsPlane(sectorIndex, true);
    if (!ceiling && !floor) return;
    if (!geometry.tessellated) triangulate(geometry);
    for (size_t triangle = 0; triangle < geometry.triangles.size(); triangle += 3)
    {
        RealtimeShadowVertex ceilingVertices[3], floorVertices[3];
        for (unsigned corner = 0; corner < 3; ++corner)
        {
            auto const &p = geometry.triangles[triangle + corner];
            int32_t cz, fz;
            getzsofslope(sectorIndex, coordinate(p.x), coordinate(p.y), &cz, &fz);
            ceilingVertices[corner] = { { p.x, p.y, cz / 16.f } };
            floorVertices[corner] = { { p.x, p.y, fz / 16.f } };
        }
        if (ceiling) emitTriangle(ceilingVertices[0], ceilingVertices[2], ceilingVertices[1], lights, lightCount, vertices);
        if (floor) emitTriangle(floorVertices[0], floorVertices[1], floorVertices[2], lights, lightCount, vertices);
    }
}

double interpolate(double first, double second, double amount)
{
    return first + (second - first) * amount;
}

void emitWalls(int sectorIndex, RealtimeLight const *lights, unsigned lightCount,
               std::vector<RealtimeShadowVertex> &vertices)
{
    auto const &sec = sector[sectorIndex];
    for (int i = sec.wallptr; i < sec.wallptr + sec.wallnum; ++i)
    {
        auto const &a = wall[i];
        if (a.point2 < sec.wallptr || a.point2 >= sec.wallptr + sec.wallnum) continue;
        auto const &b = wall[a.point2];
        int32_t c0, f0, c1, f1;
        getzsofslope(sectorIndex, a.x, a.y, &c0, &f0);
        getzsofslope(sectorIndex, b.x, b.y, &c1, &f1);
        bool const portal = unsigned(a.nextsector) < unsigned(numsectors);
        int32_t nc0 = c0, nf0 = f0, nc1 = c1, nf1 = f1;
        if (portal)
        {
            getzsofslope(a.nextsector, a.x, a.y, &nc0, &nf0);
            getzsofslope(a.nextsector, b.x, b.y, &nc1, &nf1);
        }
        double const first[4] = { c0 / 16., f0 / 16., nc0 / 16., nf0 / 16. };
        double const second[4] = { c1 / 16., f1 / 16., nc1 / 16., nf1 / 16. };
        // Split wherever sloping planes cross. Within each interval, min/max
        // portal heights are linear; clipping only the endpoints can seal an
        // opening or miss the triangular end of a ceiling/floor step.
        double splits[8] = { 0, 1 };
        unsigned splitCount = 2;
        for (unsigned p = 0; p < 4; ++p)
            for (unsigned q = p + 1; q < 4; ++q)
            {
                double const d0 = first[p] - first[q], d1 = second[p] - second[q];
                if ((d0 < 0 && d1 > 0) || (d0 > 0 && d1 < 0))
                    splits[splitCount++] = d0 / (d0 - d1);
            }
        // At most eight endpoints: insertion sort also avoids libstdc++'s
        // sixteen-element sort fast path confusing GCC's bounds analysis.
        for (unsigned p = 1; p < splitCount; ++p)
        {
            double const value = splits[p];
            unsigned q = p;
            while (q && splits[q - 1] > value)
            {
                splits[q] = splits[q - 1];
                --q;
            }
            splits[q] = value;
        }
        bool const solid = !portal || ((a.cstat & CSTAT_WALL_1WAY) &&
            !(a.cstat & (CSTAT_WALL_MASKED | CSTAT_WALL_TRANSLUCENT | CSTAT_WALL_TRANS_FLIP)));
        bool const upper = !portal || !((sec.ceilingstat & sector[a.nextsector].ceilingstat) & 1);
        bool const lower = !portal || !((sec.floorstat & sector[a.nextsector].floorstat) & 1);
        for (unsigned part = 1; part < splitCount; ++part)
        {
            double const t0 = splits[part - 1], t1 = splits[part];
            if (t1 <= t0) continue;
            double middle[4];
            for (unsigned p = 0; p < 4; ++p) middle[p] = interpolate(first[p], second[p], (t0 + t1) * .5);
            auto span = [&](unsigned top, unsigned bottom)
            {
                if (middle[bottom] <= middle[top]) return;
                RealtimeShadowVertex quad[4] = {
                    { { float(interpolate(a.x, b.x, t0)), float(interpolate(a.y, b.y, t0)), float(interpolate(first[top], second[top], t0)) } },
                    { { float(interpolate(a.x, b.x, t1)), float(interpolate(a.y, b.y, t1)), float(interpolate(first[top], second[top], t1)) } },
                    { { float(interpolate(a.x, b.x, t1)), float(interpolate(a.y, b.y, t1)), float(interpolate(first[bottom], second[bottom], t1)) } },
                    { { float(interpolate(a.x, b.x, t0)), float(interpolate(a.y, b.y, t0)), float(interpolate(first[bottom], second[bottom], t0)) } },
                };
                emitTriangle(quad[0], quad[1], quad[2], lights, lightCount, vertices);
                emitTriangle(quad[0], quad[2], quad[3], lights, lightCount, vertices);
            };
            if (solid) span(0, 1);
            else
            {
                if (upper) span(0, middle[1] < middle[2] ? 1 : 2);
                if (lower) span(middle[0] > middle[3] ? 0 : 3, 1);
                // Masked/translucent portal middles need texture alpha; they
                // must not become solid rectangles in the shadow map.
            }
        }
    }
}
} // namespace

void buildRealtimeShadowMesh(RealtimeLight const *lights, unsigned lightCount,
                           std::vector<RealtimeShadowVertex> &vertices)
{
    vertices.clear();
    if (!lights || !lightCount || numsectors <= 0 || numwalls <= 0) return;
    lightCount = std::min(lightCount, kMaxRealtimeLights);
    sectorGeometry.resize(std::min(int(numsectors), MAXSECTORS));
    // The exact topology snapshots also invalidate sectors reused by a new map.
    // Only tessellation is cached: slopes, doors and portal spans stay current.
    for (unsigned i = 0; i < sectorGeometry.size(); ++i)
    {
        auto const &sec = sector[i];
        if (sec.wallptr < 0 || sec.wallnum < 3 || int(sec.wallptr) + sec.wallnum > numwalls) continue;
        auto &geometry = sectorGeometry[i];
        refreshGeometry(i, geometry);
        double const minimum[3] = { geometry.minX, geometry.minY, -std::numeric_limits<double>::max() };
        double const maximum[3] = { geometry.maxX, geometry.maxY, std::numeric_limits<double>::max() };
        if (!intersectsLights(minimum, maximum, lights, lightCount)) continue;
        emitPlanes(i, geometry, lights, lightCount, vertices);
        emitWalls(i, lights, lightCount, vertices);
    }
}
#endif
