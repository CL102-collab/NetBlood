#version 450
// SPDX-License-Identifier: GPL-2.0-only
// Derived from NBlood's polymost1Frag.glsl; retain its material arithmetic.
// Vulkan adaptation Copyright (C) 2026 NBlood contributors.

layout(set = 0, binding = 0, std140) uniform VulkanUniforms
{
    mat4 modelView;
    mat4 projection;
    mat4 textureMatrix[3];
    mat4 rotation;
    vec4 texturePosSize;
    vec4 halfTexelPalswapPos;
    vec4 palswapSizeClamp;
    vec4 shadeNumVis;
    vec4 fog;
    vec4 fogColor;
    vec4 effects;
    vec4 npot;
    vec4 correction;
    vec4 misc;
    vec4 fixedState;
} u;

layout(set = 0, binding = 1) uniform sampler2D s_texture;
layout(set = 0, binding = 2) uniform sampler2D s_palswap;
layout(set = 0, binding = 3) uniform sampler2D s_palette;
layout(set = 0, binding = 4) uniform sampler2D s_detail;
layout(set = 0, binding = 5) uniform sampler2D s_glow;

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_distance;
layout(location = 2) in float v_fogDistance;
layout(location = 3) in vec4 v_texcoord0;
layout(location = 4) in vec4 v_texcoord3;
layout(location = 5) in vec4 v_texcoord4;
layout(location = 0) out vec4 fragColor;

const float c_basepalScale = 255.0 / 256.0;
const float c_basepalOffset = 0.5 / 256.0;
const vec2 c_vec2_zero_one = vec2(0.0, 1.0);
const vec4 c_vec4_luma_709 = vec4(0.2126, 0.7152, 0.0722, 0.0);

vec4 colorCorrection(vec4 color)
{
    vec4 correction = u.correction;
    vec4 v_cc = vec4(correction.x - 1.0, 0.5 * -(correction.y - 1.0), -(correction.z - 1.0), 1.0);
    return mat4(c_vec2_zero_one.yxxx, c_vec2_zero_one.xyxx, c_vec2_zero_one.xxyx, v_cc.xxxw)
           * mat4(correction.ywww, correction.wyww, correction.wwyw, v_cc.yyyw)
           * mat4((c_vec4_luma_709.xxxw * v_cc.z) + correction.zwww,
                  (c_vec4_luma_709.yyyw * v_cc.z) + correction.wzww,
                  (c_vec4_luma_709.zzzw * v_cc.z) + correction.wwzw,
                  c_vec2_zero_one.xxxy)
           * color;
}

vec2 npotCoordinate(vec2 original)
{
    vec2 coord = mix(original.xy, original.yx, u.effects.y);
    float modY = mod(coord.y, u.npot.y);
    coord = vec2(floor(modY) * u.npot.x + coord.x,
                 floor(coord.y * u.npot.w) + modY);
    return mix(original, mix(coord.xy, coord.yx, u.effects.y), u.npot.z);
}

vec4 polymostColor()
{
    vec2 newCoord = npotCoordinate(v_texcoord0.xy);
    vec2 texCoord = mix(fract(newCoord), clamp(newCoord, 0.0, 1.0), u.palswapSizeClamp.zw);
    texCoord = clamp(u.texturePosSize.zw * texCoord, u.halfTexelPalswapPos.xy,
                     u.texturePosSize.zw - u.halfTexelPalswapPos.xy);
    vec4 color = textureGrad(s_texture, u.texturePosSize.xy + texCoord,
                             dFdx(v_texcoord0.xy), dFdy(v_texcoord0.xy));

    float shade = clamp(u.shadeNumVis.x + clamp(u.shadeNumVis.w * v_distance
                        - 0.5 * u.misc.y, 0.0, u.shadeNumVis.y), 0.0, u.shadeNumVis.y - 1.0);
    float shadeFrac = mod(shade, 1.0);
    float colorIndex = texture(s_palswap, vec2(color.r, floor(shade) * u.shadeNumVis.z)
                              * u.palswapSizeClamp.xy + u.halfTexelPalswapPos.zw).r
                       * c_basepalScale + c_basepalOffset;
    float colorIndexNext = texture(s_palswap, u.palswapSizeClamp.xy
                                  * vec2(color.r, (floor(shade) + 1.0) * u.shadeNumVis.z)
                                  + u.halfTexelPalswapPos.zw).r
                           * c_basepalScale + c_basepalOffset;
    vec4 palettedColor = texture(s_palette, vec2(colorIndex, 0.0));
    vec4 palettedColorNext = texture(s_palette, vec2(colorIndexNext, 0.0));
    palettedColor.rgb = mix(palettedColor.rgb, palettedColorNext.rgb, shadeFrac * u.misc.y);
    float fullbright = mix(u.effects.y * palettedColor.a, 0.0, u.effects.x);
    palettedColor.a = 1.0 - floor(color.r);
    color = mix(color, palettedColor, u.effects.y);

    vec4 detailColor = texture(s_detail, v_texcoord3.xy);
    detailColor = mix(vec4(1.0), 2.0 * detailColor, u.effects.z * detailColor.a);
    color.rgb *= detailColor.rgb;
    color = mix(color, vec4(1.0), u.effects.x);
    color.rgb = mix(v_color.rgb * color.rgb, color.rgb, fullbright);

    fullbright = clamp(-(u.fog.x - 1.0), fullbright, 1.0);
    float fogFactor = clamp((u.fog.y - v_fogDistance) * u.fog.z, fullbright, 1.0);
    color.rgb = mix(u.fogColor.rgb, color.rgb, fogFactor);

    vec4 glowColor = texture(s_glow, npotCoordinate(v_texcoord4.xy));
    color.rgb = mix(color.rgb, glowColor.rgb, u.effects.w * glowColor.a * -(u.effects.x - 1.0));
    color.a *= v_color.a;
    color.rgb = pow(color.rgb, vec3(u.misc.x));

    return colorCorrection(color);
}

vec4 fixedColor()
{
    vec4 color = v_color;
    if (u.fixedState.x != 0.0)
    {
        vec4 texel = texture(s_texture, v_texcoord0.xy / v_texcoord0.w);
        color = u.fixedState.x == 2.0 ? texel : color * texel;
    }
    if (u.fixedState.y != 0.0)
    {
        float fogFactor = clamp((u.fog.y - v_fogDistance) * u.fog.z, 0.0, 1.0);
        if (int(u.fixedState.w) == 0x0800) // GL_EXP
            fogFactor = clamp(exp(-u.fog.x * v_fogDistance), 0.0, 1.0);
        else if (int(u.fixedState.w) == 0x0801) // GL_EXP2
        {
            float densityDistance = u.fog.x * v_fogDistance;
            fogFactor = clamp(exp(-densityDistance * densityDistance), 0.0, 1.0);
        }
        color.rgb = mix(u.fogColor.rgb, color.rgb, fogFactor);
    }
    return color;
}

vec4 videoColor()
{
    // Match the packed YUV conversion in animvpx.cpp.
    vec3 yuv = texture(s_texture, v_texcoord0.xy).rgb;
    float y = 1.1643 * (yuv.r - 0.0625);
    float cb = yuv.g - 0.5;
    float cr = yuv.b - 0.5;
    return vec4(y + 1.5958 * cr, y - 0.39173 * cb - 0.81290 * cr, y + 2.017 * cb, 1.0);
}

vec4 surfaceColor()
{
    // Match glsurface.cpp's palette lookup and color correction shader.
    vec4 color = texture(s_texture, v_texcoord0.xy);
    color.r = c_basepalScale * color.r + c_basepalOffset;
    color.rgb = texture(s_palette, color.rg).rgb;
    return colorCorrection(color);
}

bool alphaTest(float alpha)
{
    // GL_NEVER through GL_ALWAYS are the contiguous enum range 0x0200..0207.
    float value = clamp(alpha, 0.0, 1.0);
    float reference = clamp(u.fog.w, 0.0, 1.0);
    switch (int(u.fixedState.z))
    {
        case 0x0200: return false;
        case 0x0201: return value < reference;
        case 0x0202: return value == reference;
        case 0x0203: return value <= reference;
        case 0x0204: return value > reference;
        case 0x0205: return value != reference;
        case 0x0206: return value >= reference;
        default: return true; // GL_ALWAYS
    }
}

void main()
{
    vec4 color;
    switch (int(u.misc.w))
    {
        case 1: color = polymostColor(); break;
        case 2: color = videoColor(); break;
        case 3: color = surfaceColor(); break;
        default: color = fixedColor(); break;
    }
    if (u.misc.z != 0.0 && !alphaTest(color.a))
        discard;
    fragColor = color;
}
