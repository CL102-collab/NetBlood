// Realtime point lighting and geometry shadows shared by OpenGL and Vulkan.
// Copyright (C) 2026 NetBlood contributors. GPL-2.0-only.
// Requires ambient_occlusion.glsl for positive-Z reconstruction and derivatives,
// mat4 u_eyeToWorld, and vec4 u_lightParams (count, shadows, face size, columns).
// LIGHT_POSITION(i) returns vec4(world position, radius); LIGHT_COLOR(i) returns
// vec4(RGB, intensity). LIGHTING_SHADOW(uv) reads radial depth from the atlas.
// Framebuffer pixels and shadow atlas coordinates both use a bottom-left origin.

float shadowVisibility(int lightIndex, vec3 fromLight, float radius, float diffuse)
{
    if (u_lightParams.y < 0.5) return 1.0;
    vec3 a = abs(fromLight);
    vec3 face;
    int faceIndex;
    if (a.x >= a.y && a.x >= a.z)
    {
        if (fromLight.x > 0.0) { faceIndex = 0; face = vec3(-fromLight.z, -fromLight.y, a.x); }
        else { faceIndex = 1; face = vec3(fromLight.z, -fromLight.y, a.x); }
    }
    else if (a.y >= a.z)
    {
        if (fromLight.y > 0.0) { faceIndex = 2; face = vec3(fromLight.x, fromLight.z, a.y); }
        else { faceIndex = 3; face = vec3(fromLight.x, -fromLight.z, a.y); }
    }
    else
    {
        if (fromLight.z > 0.0) { faceIndex = 4; face = vec3(fromLight.x, -fromLight.y, a.z); }
        else { faceIndex = 5; face = vec3(-fromLight.x, -fromLight.y, a.z); }
    }
    vec2 cell = vec2(lightIndex, faceIndex);
    vec2 dimensions = vec2(u_lightParams.w, 6.0);
    vec2 facePixel = (face.xy / max(face.z, 0.000001) * 0.5 + 0.5) * u_lightParams.z;
    float receiver = length(fromLight) / radius;
    // Bias scales with the light's radius and atlas texel footprint. Grazing
    // surfaces need more bias, without fixed Build-unit constants in this API.
    float bias = (0.35 + 1.25 * (1.0 - diffuse)) / u_lightParams.z;
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 pixel = clamp(facePixel + vec2(x, y), vec2(0.5), vec2(u_lightParams.z - 0.5));
            vec2 uv = (cell + pixel / u_lightParams.z) / dimensions;
            visibility += receiver - bias <= LIGHTING_SHADOW(uv) ? 1.0 : 0.0;
        }
    return visibility / 9.0;
}

vec3 realtimeLighting(vec2 pixel)
{
    float depth = aoReadDepth(pixel);
    if (depth >= 0.999999 || depth <= 0.0) return vec3(0.0);
    vec3 eye = aoPosition(pixel, depth);
    vec3 position = (u_eyeToWorld * vec4(eye, 1.0)).xyz;
    vec3 dx = mat3(u_eyeToWorld) * aoDerivative(pixel, vec2(1.0, 0.0), eye);
    vec3 dy = mat3(u_eyeToWorld) * aoDerivative(pixel, vec2(0.0, 1.0), eye);
    if (length(dx) < 1e-20 || length(dy) < 1e-20) return vec3(0.0);
    vec3 normal = cross(normalize(dx), normalize(dy));
    if (length(normal) < 0.00001) return vec3(0.0);
    normal = normalize(normal);
    if (dot(normal, position - u_eyeToWorld[3].xyz) > 0.0) normal = -normal;
    vec3 illumination = vec3(0.0);
    for (int i = 0; i < int(u_lightParams.x); ++i)
    {
        vec3 toLight = LIGHT_POSITION(i).xyz - position;
        float distanceToLight = length(toLight);
        float radius = LIGHT_POSITION(i).w;
        if (distanceToLight >= radius || distanceToLight < 0.000001) continue;
        float diffuse = max(dot(normal, toLight / distanceToLight), 0.0);
        if (diffuse <= 0.0) continue;
        float attenuation = 1.0 - distanceToLight / radius;
        float visibility = shadowVisibility(i, -toLight, radius, diffuse);
        illumination += LIGHT_COLOR(i).rgb * LIGHT_COLOR(i).a *
            (attenuation * attenuation * diffuse * visibility);
    }
    return clamp(illumination, 0.0, 1.0);
}
