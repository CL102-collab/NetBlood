// Screen-space ambient occlusion shared by OpenGL and native Vulkan.
// Copyright (C) 2026 NetBlood contributors. GPL-2.0-only.
// AO_DEPTH reads normalized framebuffer coordinates with a bottom-left origin,
// returning 1 for skies, portal surfaces, or pixels without world geometry.

bool aoInside(vec2 pixel)
{
    return all(greaterThanEqual(pixel, max(u_aoViewport.xy, vec2(0.5)))) &&
           all(lessThan(pixel, min(u_aoViewport.xy + u_aoViewport.zw,
                                   vec2(1.0) / u_aoTarget.xy)));
}

float aoReadDepth(vec2 pixel)
{
    if (!aoInside(pixel)) return 1.0;
    return AO_DEPTH(pixel * u_aoTarget.xy);
}

vec3 aoPosition(vec2 pixel, float depth)
{
    vec2 ndc = (pixel - u_aoViewport.xy) / u_aoViewport.zw * 2.0 - 1.0;
    // Polymost looks along positive eye Z; clip.w is eye.z.
    float z = u_aoDepth.y / (2.0 * depth - 1.0 - u_aoDepth.x);
    return vec3((ndc - u_aoProjection.zw) * z / u_aoProjection.xy, z);
}

vec3 aoDerivative(vec2 pixel, vec2 stepPixel, vec3 position)
{
    float beforeDepth = aoReadDepth(pixel - stepPixel);
    float afterDepth = aoReadDepth(pixel + stepPixel);
    if (beforeDepth >= 0.999999 && afterDepth >= 0.999999) return vec3(0.0);
    if (beforeDepth >= 0.999999) return aoPosition(pixel + stepPixel, afterDepth) - position;
    if (afterDepth >= 0.999999) return position - aoPosition(pixel - stepPixel, beforeDepth);
    vec3 before = position - aoPosition(pixel - stepPixel, beforeDepth);
    vec3 after = aoPosition(pixel + stepPixel, afterDepth) - position;
    // Avoid constructing a normal across a foreground silhouette.
    return abs(before.z) < abs(after.z) ? before : after;
}

float ambientOcclusion(vec2 pixel)
{
    float depth = aoReadDepth(pixel);
    if (depth >= 0.999999 || depth <= 0.0) return 1.0;

    vec3 position = aoPosition(pixel, depth);
    vec3 dx = aoDerivative(pixel, vec2(1.0, 0.0), position);
    vec3 dy = aoDerivative(pixel, vec2(0.0, 1.0), position);
    float dxLength = length(dx), dyLength = length(dy);
    if (dxLength < 0.00000000000000000001 || dyLength < 0.00000000000000000001) return 1.0;
    // Normalize before crossing: Polymost's eye-space units change with the
    // viewport size, so an absolute normal-length threshold would lose AO at
    // high resolutions or when close to a surface.
    vec3 normal = cross(dx / dxLength, dy / dyLength);
    float normalLength = length(normal);
    if (normalLength < 0.00001) return 1.0;
    normal /= normalLength;
    if (dot(normal, position) > 0.0) normal = -normal;

    float radius = u_aoDepth.z;
    vec2 radiusPixels = 0.5 * u_aoViewport.zw * u_aoProjection.xy * radius / position.z;
    // Limit work to a local neighborhood even when the camera touches a wall.
    radiusPixels *= min(1.0, 64.0 / max(radiusPixels.x, radiusPixels.y));
    float occlusion = 0.0;
    for (int direction = 0; direction < 8; ++direction)
    {
        float angle = float(direction) * 0.78539816339;
        vec2 ray = vec2(cos(angle), sin(angle)) * radiusPixels;
        for (int ring = 0; ring < 2; ++ring)
        {
            float scale = ring == 0 ? 0.35 : 0.80;
            vec2 samplePixel = floor(pixel + ray * scale) + 0.5;
            float sampleDepth = aoReadDepth(samplePixel);
            if (sampleDepth >= 0.999999 || sampleDepth <= 0.0) continue;
            vec3 delta = aoPosition(samplePixel, sampleDepth) - position;
            float distanceToSample = length(delta);
            if (distanceToSample <= radius * 0.001 || distanceToSample >= radius) continue;
            float falloff = 1.0 - distanceToSample / radius;
            // Coplanar surfaces contribute zero: this is contact shading, not
            // a uniform darkening filter or a depth-edge outline.
            float horizon = max(dot(normal, delta / distanceToSample) - u_aoDepth.w, 0.0);
            occlusion += horizon * falloff * falloff;
        }
    }
    return clamp(1.0 - u_aoTarget.z * occlusion / 16.0, 0.35, 1.0);
}
