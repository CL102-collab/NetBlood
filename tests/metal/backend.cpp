// Native Metal backend contract checks. No retail game assets are required.
// SPDX-License-Identifier: GPL-2.0-only
#include "metal_renderer.h"
#include <SDL.h>
#include <glad/glad.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

NativeDrawState defaults(int width = 64, int height = 64)
{
    NativeDrawState state = {};
    state.viewport[2] = width;
    state.viewport[3] = height;
    state.topology = GL_TRIANGLES;
    state.depthFunc = GL_LESS;
    state.depthRange[1] = 1.f;
    state.depthWrite = true;
    state.blendSrc = GL_SRC_ALPHA;
    state.blendDst = GL_ONE_MINUS_SRC_ALPHA;
    state.blendEquation = GL_FUNC_ADD;
    state.frontFace = GL_CCW;
    state.cullFace = GL_BACK;
    state.polygonMode = GL_FILL;
    state.stencilFunc = GL_ALWAYS;
    state.stencilFail = state.stencilDepthFail = state.stencilPass = GL_KEEP;
    state.stencilCompareMask = state.stencilWriteMask = 255;
    state.lineWidth = 1.f;
    for (unsigned i = 0; i < 4; ++i)
    {
        state.colorWrite[i] = true;
        state.uniforms.modelView[i * 5] = state.uniforms.projection[i * 5] = state.uniforms.rotation[i * 5] = 1.f;
        for (unsigned j = 0; j < 3; ++j) state.uniforms.textureMatrix[j][i * 5] = 1.f;
    }
    for (auto &sample : state.samplers)
    {
        sample.minFilter = sample.magFilter = GL_NEAREST;
        sample.wrapS = sample.wrapT = GL_CLAMP_TO_EDGE;
        sample.anisotropy = 1.f;
        sample.maxLod = 1000.f;
    }
    return state;
}

void require(bool condition, char const *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        metalDestroy();
        SDL_Quit();
        std::exit(1);
    }
}

void triangle(NativeDrawState const &state, float r, float g, float b, float a = 1.f, float z = 0.f)
{
    NativeVertex vertices[3] = {};
    float const positions[3][2] = { { -1.f, -1.f }, { 3.f, -1.f }, { -1.f, 3.f } };
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[i].position[0] = positions[i][0];
        vertices[i].position[1] = positions[i][1];
        vertices[i].position[2] = z;
        vertices[i].position[3] = 1.f;
        vertices[i].color[0] = r;
        vertices[i].color[1] = g;
        vertices[i].color[2] = b;
        vertices[i].color[3] = a;
        vertices[i].texcoord[0][0] = (positions[i][0] + 1.f) * 0.5f;
        vertices[i].texcoord[0][1] = (positions[i][1] + 1.f) * 0.5f;
        vertices[i].texcoord[0][3] = 1.f;
    }
    metalDraw(vertices, 3, state);
    require(metalActive(), "draw must leave the renderer active");
}

void pixel(char const *label, int r, int g, int b, int x = 32, int y = 32)
{
    unsigned char rgba[4] = {};
    require(metalReadPixels(x, y, 1, 1, rgba), "read back rendered pixel");
    if (std::abs(static_cast<int>(rgba[0]) - r) > 2 || std::abs(static_cast<int>(rgba[1]) - g) > 2 ||
        std::abs(static_cast<int>(rgba[2]) - b) > 2)
    {
        std::fprintf(stderr, "%s: expected RGB %d %d %d; actual RGBA %u %u %u %u\n",
                     label, r, g, b, rgba[0], rgba[1], rgba[2], rgba[3]);
        require(false, label);
    }
    std::printf("PASS: %s\n", label);
}


// The constant-colored geometry removes texture/shade variation from AO checks.
// A flat front-facing wall must remain unoccluded. Two planes meeting in a
// 90-degree concave corner must darken only near their common vertical edge.
void ambientFrame(int width, int height, bool corner, bool apply, float strength = 1.f,
                  bool eligible = true, bool hud = false, bool inset = false, bool isolated = false)
{
    NativeDrawState state = defaults(width, height);
    float const background[] = { 0.12f, 0.23f, 0.34f, 1.f };
    metalClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                background, 1.f, 0, state);
    state.depthTest = true;
    state.stencilTest = true;
    state.stencilReference = eligible ? 128 : 0;
    state.stencilWriteMask = 128;
    state.stencilPass = GL_REPLACE;
    if (isolated)
    {
        state.scissorTest = true;
        state.scissor[0] = width / 2;
        state.scissor[1] = height / 2;
        state.scissor[2] = state.scissor[3] = 1;
    }
    if (inset)
    {
        state.viewport[0] = width / 8;
        state.viewport[1] = height / 8;
        state.viewport[2] = width * 3 / 4;
        state.viewport[3] = height * 3 / 4;
    }
    // Polymost's positive-Z perspective with near=1 and far=16; the backend preserves
    // this projection's depth regardless of depth-clip-control availability.
    std::fill(state.uniforms.projection, state.uniforms.projection + 16, 0.f);
    state.uniforms.projection[0] = state.uniforms.projection[5] = 1.f;
    state.uniforms.projection[10] = isolated ? 1.f : 17.f / 15.f;
    state.uniforms.projection[11] = 1.f;
    state.uniforms.projection[14] = isolated ? -2.f : -32.f / 15.f;
    auto wall = [&state](float left, float leftZ, float right, float rightZ)
    {
        float const positions[4][3] = {
            {left, -4.f, leftZ}, {right, -4.f, rightZ},
            {right, 4.f, rightZ}, {left, 4.f, leftZ}
        };
        unsigned const indices[] = {0, 1, 2, 0, 2, 3};
        NativeVertex vertices[6] = {};
        for (unsigned i = 0; i < 6; ++i)
        {
            for (unsigned axis = 0; axis < 3; ++axis)
                vertices[i].position[axis] = positions[indices[i]][axis];
            vertices[i].position[3] = 1.f;
            vertices[i].color[0] = 0.8f;
            vertices[i].color[1] = 0.6f;
            vertices[i].color[2] = 0.4f;
            vertices[i].color[3] = 1.f;
        }
        metalDraw(vertices, 6, state);
    };
    if (corner)
    {
        wall(-2.f, 2.f, 0.f, 4.f);
        wall(0.f, 4.f, 2.f, 2.f);
    }
    else wall(-4.f, 4.f, 4.f, 4.f);
    if (apply)
        require(metalAmbientOcclusion(state.uniforms.projection, state.viewport, 2.f, strength, 0.06f),
                "apply native GPU ambient occlusion");
    if (hud)
    {
        // Draw after AO exactly as the game's HUD does. Its white/magenta bars
        // cross the otherwise-darkened corner so order mistakes are visible.
        state = defaults(width, height);
        state.scissorTest = true;
        state.scissor[0] = width / 4;
        state.scissor[1] = height / 8;
        state.scissor[2] = width / 2;
        state.scissor[3] = height / 8;
        triangle(state, 1.f, 1.f, 1.f);
        state.scissor[1] += height / 8;
        triangle(state, 1.f, 0.f, 1.f);
    }
}

std::vector<unsigned char> ambientImage(int width, int height)
{
    std::vector<unsigned char> image(static_cast<size_t>(width) * height * 4);
    require(metalReadPixels(0, 0, width, height, image.data()), "read AO comparison image");
    return image;
}

void ambientWriteImage(char const *label, int width, int height, std::vector<unsigned char> const &image)
{
    // Optional lossless evidence, no screenshot library/game assets required.
    char const *directory = std::getenv("NBLOOD_AO_TEST_OUTPUT");
    if (!directory || !*directory) return;
    std::string const path = std::string(directory) + "/" + label + ".ppm";
    FILE *file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "open AO evidence image in the supplied existing directory");
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    // ReadPixels uses OpenGL's bottom-left origin; PPM files start at the top.
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            require(std::fwrite(&image[(size_t(y) * width + x) * 4], 1, 3, file) == 3,
                    "write AO evidence pixels");
    require(std::fclose(file) == 0, "close AO evidence image");
}

void ambientSceneChecks(int width, int height)
{
    ambientFrame(width, height, false, false);
    auto const flatOff = ambientImage(width, height);
    ambientFrame(width, height, false, true);
    auto const flatOn = ambientImage(width, height);
    for (size_t i = 0; i < flatOff.size(); ++i)
        require(std::abs(int(flatOff[i]) - int(flatOn[i])) <= 1,
                "a flat wall has no self-occlusion or alpha changes");

    ambientFrame(width, height, true, false);
    auto const cornerOff = ambientImage(width, height);
    ambientFrame(width, height, true, true);
    auto const cornerOn = ambientImage(width, height);
    unsigned maxDarkening = 0, changed = 0;
    double nearSum = 0, farSum = 0;
    unsigned nearCount = 0, farCount = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            size_t const offset = (size_t(y) * width + x) * 4;
            require(cornerOn[offset + 3] == cornerOff[offset + 3], "AO preserves destination alpha");
            for (unsigned c = 0; c < 3; ++c)
                require(cornerOn[offset + c] <= cornerOff[offset + c] + 1,
                        "AO never brightens the constant-colored scene");
            if (y < height / 3 || y >= height * 2 / 3) continue;
            int const darkening = int(cornerOff[offset]) - int(cornerOn[offset]);
            if (x >= width * 7 / 16 && x < width * 9 / 16)
            {
                nearSum += darkening;
                ++nearCount;
                if (darkening >= 2) ++changed;
                maxDarkening = std::max(maxDarkening, unsigned(std::max(0, darkening)));
            }
            else if ((x >= width / 8 && x < width / 4) ||
                     (x >= width * 3 / 4 && x < width * 7 / 8))
            {
                farSum += darkening;
                ++farCount;
            }
        }
    require(maxDarkening >= 4 && changed > nearCount / 20,
            "the concave corner receives visible local occlusion");
    require(nearSum / nearCount > farSum / farCount + 0.5,
            "the corner darkens more than distant flat wall regions");
    std::printf("PASS: AO corner %dx%d: peak darkening %u, near mean %.3f, distant mean %.3f\n",
                width, height, maxDarkening, nearSum / nearCount, farSum / farCount);
    if (width == 160)
    {
        ambientWriteImage("flat-off", width, height, flatOff);
        ambientWriteImage("flat-on", width, height, flatOn);
        ambientWriteImage("corner-off", width, height, cornerOff);
        ambientWriteImage("corner-on", width, height, cornerOn);
    }

    ambientFrame(width, height, true, true, 0.f);
    require(ambientImage(width, height) == cornerOff, "zero-strength AO is an exact bypass after prior use");
    ambientFrame(width, height, true, false);
    require(ambientImage(width, height) == cornerOff, "disabling AO restores exact original pixels");
    ambientFrame(width, height, true, true, 1.f, false);
    require(ambientImage(width, height) == cornerOff, "stencil-ineligible world content is excluded from AO");
    // A one-pixel silhouette has no valid neighboring depths. With an infinite
    // far plane, reconstructing those invalid depths would produce infinities.
    ambientFrame(width, height, false, false, 1.f, true, false, false, true);
    auto const isolatedOff = ambientImage(width, height);
    ambientFrame(width, height, false, true, 1.f, true, false, false, true);
    require(ambientImage(width, height) == isolatedOff,
            "isolated geometry with an infinite far plane has no invalid-depth artifacts");

    ambientFrame(width, height, true, false, 1.f, true, true);
    auto const hudOff = ambientImage(width, height);
    ambientFrame(width, height, true, true, 1.f, true, true);
    auto const hudOn = ambientImage(width, height);
    for (int y = height / 8; y < (height / 8) * 3; ++y)
        for (int x = width / 4; x < width * 3 / 4; ++x)
            for (unsigned c = 0; c < 4; ++c)
                require(hudOn[(size_t(y) * width + x) * 4 + c] == hudOff[(size_t(y) * width + x) * 4 + c],
                        "HUD drawn after AO remains byte-for-byte unchanged");

    ambientFrame(width, height, true, false, 1.f, true, false, true);
    auto const insetOff = ambientImage(width, height);
    ambientFrame(width, height, true, true, 1.f, true, false, true);
    auto const insetOn = ambientImage(width, height);
    unsigned insetChanges = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            size_t const offset = (size_t(y) * width + x) * 4;
            bool const outside = x < width / 8 || x >= width * 7 / 8 ||
                                 y < height / 8 || y >= height * 7 / 8;
            for (unsigned c = 0; c < 4; ++c)
                if (outside)
                    require(insetOn[offset + c] == insetOff[offset + c], "AO respects the supplied world viewport");
            if (!outside && insetOn[offset] + 2 < insetOff[offset]) ++insetChanges;
        }
    require(insetChanges > 0, "offset viewport still receives corner occlusion");
    std::puts("PASS: AO flat wall, bypass/restore, mask exclusion, post-pass HUD and viewport boundaries");
}


// The receiver and caster have separate projections from the camera and point
// light. In the offscreen case the complete caster lies outside the camera's
// frustum, but still blocks rays to the visible wall. Screen-space occlusion
// cannot satisfy this test.
struct LightingScene
{
    bool apply = true, shadows = true, eligible = true, hud = false, inset = false;
    bool offscreen = false, emptyMesh = false, zeroLights = false, zeroColor = false;
    bool movedLight = false, translatedWorld = false;
};

void lightingFrame(int width, int height, LightingScene const &scene)
{
    NativeDrawState state = defaults(width, height);
    float const background[] = { 0.12f, 0.23f, 0.34f, 0.7f };
    metalClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                background, 1.f, 0, state);
    state.depthTest = true;
    state.stencilTest = true;
    state.stencilReference = scene.eligible ? 128 : 0;
    state.stencilWriteMask = 128;
    state.stencilPass = GL_REPLACE;
    if (scene.inset)
    {
        state.viewport[0] = width / 8;
        state.viewport[1] = height / 8;
        state.viewport[2] = width * 3 / 4;
        state.viewport[3] = height * 3 / 4;
    }
    std::fill(state.uniforms.projection, state.uniforms.projection + 16, 0.f);
    state.uniforms.projection[0] = state.uniforms.projection[5] = 1.f;
    state.uniforms.projection[10] = 17.f / 15.f;
    state.uniforms.projection[11] = 1.f;
    state.uniforms.projection[14] = -32.f / 15.f;
    float eyeToWorld[16] = {};
    for (unsigned i = 0; i < 4; ++i) eyeToWorld[i * 5] = 1.f;
    if (scene.translatedWorld)
    {
        eyeToWorld[12] = 8.f;
        eyeToWorld[13] = -3.f;
        eyeToWorld[14] = 12.f;
    }
    std::vector<RealtimeShadowVertex> shadowMesh;
    auto quad = [&state, &shadowMesh, &eyeToWorld](float left, float right, float bottom, float top, float z)
    {
        float const positions[4][3] = { {left, bottom, z}, {right, bottom, z},
                                         {right, top, z}, {left, top, z} };
        unsigned const indices[] = {0, 2, 1, 0, 3, 2};
        NativeVertex vertices[6] = {};
        for (unsigned i = 0; i < 6; ++i)
        {
            RealtimeShadowVertex shadow = {};
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                vertices[i].position[axis] = positions[indices[i]][axis];
                shadow.position[axis] = positions[indices[i]][axis] + eyeToWorld[12 + axis];
            }
            vertices[i].position[3] = 1.f;
            vertices[i].color[0] = 0.35f;
            vertices[i].color[1] = 0.45f;
            vertices[i].color[2] = 0.55f;
            vertices[i].color[3] = 0.7f;
            shadowMesh.push_back(shadow);
        }
        metalDraw(vertices, 6, state);
    };
    quad(-4.f, 4.f, -4.f, 4.f, 4.f);
    if (scene.offscreen) quad(-4.1f, -3.1f, -0.4f, 0.4f, 1.5f);
    else quad(-0.75f, -0.25f, -0.5f, 0.5f, 2.f);
    RealtimeLight light = {};
    light.positionRadius[0] = scene.offscreen ? -6.f : (scene.movedLight ? 1.f : -1.f);
    light.positionRadius[2] = scene.offscreen ? 0.f : 1.f;
    light.positionRadius[3] = scene.offscreen ? 16.f : 8.f;
    for (unsigned axis = 0; axis < 3; ++axis) light.positionRadius[axis] += eyeToWorld[12 + axis];
    light.color[0] = scene.zeroColor ? 0.f : 0.8f;
    light.color[1] = scene.zeroColor ? 0.f : 0.5f;
    light.color[2] = scene.zeroColor ? 0.f : 0.2f;
    light.color[3] = 1.f;
    if (scene.apply)
        require(metalRealtimeLighting(state.uniforms.projection, eyeToWorld, state.viewport,
                                       scene.zeroLights ? nullptr : &light, scene.zeroLights ? 0 : 1,
                                       scene.emptyMesh ? nullptr : shadowMesh.data(),
                                       scene.emptyMesh ? 0 : static_cast<unsigned>(shadowMesh.size()), scene.shadows),
                "apply native point lighting and geometry shadows");
    if (scene.hud)
    {
        state = defaults(width, height);
        state.scissorTest = true;
        state.scissor[0] = width / 4;
        state.scissor[1] = height / 8;
        state.scissor[2] = width / 2;
        state.scissor[3] = height / 8;
        triangle(state, 1.f, 1.f, 1.f);
        state.scissor[1] += height / 8;
        triangle(state, 1.f, 0.f, 1.f);
    }
}

std::vector<unsigned char> lightingImage(int width, int height)
{
    std::vector<unsigned char> image(static_cast<size_t>(width) * height * 4);
    require(metalReadPixels(0, 0, width, height, image.data()), "read lighting comparison image");
    return image;
}

void lightingWriteImage(char const *label, int width, int height, std::vector<unsigned char> const &image)
{
    char const *directory = std::getenv("NBLOOD_LIGHTING_TEST_OUTPUT");
    if (!directory || !*directory) return;
    std::string const path = std::string(directory) + "/metal-" + label + ".ppm";
    FILE *file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "open lighting evidence image in the supplied existing directory");
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            require(std::fwrite(&image[(size_t(y) * width + x) * 4], 1, 3, file) == 3,
                    "write lighting evidence pixels");
    require(std::fclose(file) == 0, "close lighting evidence image");
}

void lightingSceneChecks(int width, int height)
{
    auto capture = [width, height](LightingScene const &scene)
    {
        lightingFrame(width, height, scene);
        return lightingImage(width, height);
    };
    LightingScene scene;
    scene.apply = false;
    auto const unlit = capture(scene);
    scene.apply = true;
    scene.shadows = false;
    auto const lit = capture(scene);
    scene.shadows = true;
    auto const shadowed = capture(scene);
    unsigned brightened = 0, blocked = 0, unaffected = 0;
    for (size_t i = 0; i < unlit.size(); i += 4)
    {
        require(lit[i + 3] == unlit[i + 3] && shadowed[i + 3] == unlit[i + 3],
                "lighting and shadows preserve destination alpha");
        for (unsigned c = 0; c < 3; ++c)
        {
            require(lit[i + c] + 1 >= unlit[i + c], "point lighting never darkens the base material");
            require(shadowed[i + c] <= lit[i + c] + 1, "shadows only reduce the added light");
            require(shadowed[i + c] + 1 >= unlit[i + c], "shadows preserve the base material lighting");
        }
        if (lit[i] >= unlit[i] + 3) ++brightened;
        if (shadowed[i] + 3 <= lit[i]) ++blocked;
        if (lit[i] >= unlit[i] + 3 && std::abs(int(lit[i]) - int(shadowed[i])) <= 1) ++unaffected;
    }
    require(brightened > unsigned(width * height / 20), "point light visibly illuminates the receiver");
    require(blocked > unsigned(width * height / 400), "geometry blocks point light on visible receiver pixels");
    require(unaffected > unsigned(width * height / 20), "unblocked receiver retains its point lighting");
    std::printf("PASS: lighting %dx%d: %u illuminated, %u shadowed, %u unblocked pixels\n",
                width, height, brightened, blocked, unaffected);
    if (width == 160)
    {
        lightingWriteImage("unlit", width, height, unlit);
        lightingWriteImage("lit", width, height, lit);
        lightingWriteImage("shadowed", width, height, shadowed);
    }
    scene.movedLight = true;
    auto const moved = capture(scene);
    unsigned movedPixels = 0;
    for (size_t i = 0; i < moved.size(); i += 4)
        if (std::abs(int(moved[i]) - int(shadowed[i])) >= 3) ++movedPixels;
    require(movedPixels > unsigned(width * height / 20), "moving the point light updates illumination and shadows");
    scene = LightingScene();
    scene.shadows = false;
    require(capture(scene) == lit, "disabling shadows removes the previous shadow atlas exactly");
    scene.shadows = true;
    scene.emptyMesh = true;
    require(capture(scene) == lit, "an empty caster mesh does not reuse stale shadows");
    scene = LightingScene();
    scene.apply = false;
    require(capture(scene) == unlit, "disabling lighting restores exact original pixels after prior use");
    scene.apply = true;
    scene.zeroLights = true;
    require(capture(scene) == unlit, "zero lights is an exact bypass after prior use");
    scene.zeroLights = false;
    scene.zeroColor = true;
    require(capture(scene) == unlit, "zero-intensity light leaves the base material unchanged");
    scene.zeroColor = false;
    scene.eligible = false;
    require(capture(scene) == unlit, "stencil-ineligible content receives no point lighting or shadows");

    scene = LightingScene();
    scene.translatedWorld = true;
    auto const translated = capture(scene);
    unsigned translatedChanges = 0, translationError = 0;
    for (size_t i = 0; i < translated.size(); ++i)
    {
        unsigned const error = unsigned(std::abs(int(translated[i]) - int(shadowed[i])));
        translationError += error;
        if (error > 1) ++translatedChanges;
    }
    std::printf("Lighting world translation: %u changed channels, aggregate error %u\n",
                translatedChanges, translationError);
    // Translating a shadow edge across floating-point coordinates can select
    // an adjacent PCF texel. Bound that small edge variation, while rejecting a
    // misplaced light or a camera/world reconstruction mismatch.
    require(translatedChanges < translated.size() / 200 && translationError < translated.size() / 20,
            "camera, light and shadow mesh world translation preserves the rendered scene");

    scene = LightingScene();
    scene.offscreen = true;
    scene.shadows = false;
    auto const offscreenLit = capture(scene);
    scene.shadows = true;
    auto const offscreenShadow = capture(scene);
    unsigned offscreenBlocked = 0;
    for (size_t i = 0; i < offscreenLit.size(); i += 4)
        if (offscreenShadow[i] + 2 <= offscreenLit[i]) ++offscreenBlocked;
    require(offscreenBlocked > unsigned(width * height / 200),
            "a caster wholly outside the camera frustum shadows the visible receiver");
    if (width == 160)
    {
        lightingWriteImage("offscreen-lit", width, height, offscreenLit);
        lightingWriteImage("offscreen-shadowed", width, height, offscreenShadow);
    }

    scene = LightingScene();
    scene.hud = true;
    scene.apply = false;
    auto const hudOff = capture(scene);
    scene.apply = true;
    auto const hudOn = capture(scene);
    for (int y = height / 8; y < (height / 8) * 3; ++y)
        for (int x = width / 4; x < width * 3 / 4; ++x)
            for (unsigned c = 0; c < 4; ++c)
                require(hudOn[(size_t(y) * width + x) * 4 + c] == hudOff[(size_t(y) * width + x) * 4 + c],
                        "HUD drawn after lighting remains byte-for-byte unchanged");
    scene = LightingScene();
    scene.inset = true;
    scene.apply = false;
    auto const insetOff = capture(scene);
    scene.apply = true;
    auto const insetOn = capture(scene);
    unsigned insetChanges = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            size_t const offset = (size_t(y) * width + x) * 4;
            bool const outside = x < width / 8 || x >= width / 8 + width * 3 / 4 ||
                                 y < height / 8 || y >= height / 8 + height * 3 / 4;
            for (unsigned c = 0; c < 4; ++c)
                if (outside)
                    require(insetOn[offset + c] == insetOff[offset + c], "lighting respects the supplied world viewport");
            if (!outside && insetOn[offset] >= insetOff[offset] + 3) ++insetChanges;
        }
    require(insetChanges > unsigned(width * height / 40), "offset viewport still receives point lighting");
    std::puts("PASS: lighting motion, shadow bypass, offscreen caster, world transform, alpha, mask, HUD and viewport");
}


void renderingState()
{
    NativeDrawState state = defaults();
    float const blue[] = { 0.f, 0.f, 1.f, 1.f };
    float const black[] = { 0.f, 0.f, 0.f, 1.f };
    metalClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("color clear", 0, 0, 255);
    triangle(state, 1.f, 0.f, 0.f);
    pixel("triangle rasterization", 255, 0, 0);

    metalClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    state.cull = true;
    triangle(state, 1.f, 0.f, 0.f);
    pixel("CCW front face survives backface culling", 255, 0, 0);
    state.cullFace = GL_FRONT;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("front face culling", 255, 0, 0);
    state.cull = false;

    state.depthTest = true;
    triangle(state, 0.f, 1.f, 0.f, 1.f, -0.5f);
    triangle(state, 1.f, 0.f, 0.f, 1.f, 0.5f);
    pixel("depth occlusion", 0, 255, 0);
    state.depthTest = false;
    triangle(state, 1.f, 0.f, 0.f, 1.f, 2.f);
    pixel("far-plane clipping", 0, 255, 0);
    if (metalDepthClampSupported())
    {
        state.depthClamp = true;
        triangle(state, 1.f, 0.f, 0.f, 1.f, 2.f);
        pixel("depth clamping", 255, 0, 0);
        state.depthClamp = false;
        triangle(state, 0.f, 1.f, 0.f);
    }
    state.blend = true;
    triangle(state, 1.f, 0.f, 0.f, 0.5f);
    pixel("source alpha blending", 128, 127, 0);
    state.blendSrc = state.blendDst = GL_ONE;
    state.blendEquation = GL_FUNC_REVERSE_SUBTRACT;
    triangle(state, 0.25f, 0.f, 0.f);
    pixel("reverse subtract tint blending", 64, 127, 0);
    state = defaults();

    state.scissorTest = true;
    state.scissor[0] = state.scissor[1] = 0;
    state.scissor[2] = state.scissor[3] = 16;
    metalClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("scissor bottom-left origin", 0, 0, 255, 8, 8);
    pixel("scissor preserves other pixels", 64, 127, 0, 8, 56);
    state = defaults();

    unsigned char const texture[] = { 255, 0, 0, 255, 0, 255, 0, 255,
                                      0, 0, 255, 255, 255, 255, 255, 255 };
    metalUploadTexture(1, 0, 2, 2, texture);
    state.samplers[0].texture = 1;
    state.uniforms.fixed[0] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("texture bottom-left origin", 255, 0, 0, 8, 8);
    pixel("texture top-left orientation", 0, 0, 255, 8, 56);
    pixel("texture bottom-right orientation", 0, 255, 0, 56, 8);
    unsigned char const yellow[] = { 255, 255, 0, 255 };
    // Exercise an upload in the middle of an unsubmitted render pass.
    triangle(state, 1.f, 1.f, 1.f);
    metalUploadTextureRegion(1, 0, 0, 1, 1, 1, yellow);
    triangle(state, 1.f, 1.f, 1.f);
    pixel("subimage upload between draws", 255, 255, 0, 8, 56);
    pixel("subimage preserves other texels", 0, 255, 0, 56, 8);
    metalUploadTexture(1, 1, 1, 1, yellow);
    state.samplers[0].minFilter = GL_NEAREST_MIPMAP_NEAREST;
    state.samplers[0].minLod = state.samplers[0].maxLod = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("explicit mip level and sampler LOD limits", 255, 255, 0);
    state = defaults();

    // A tall NPOT texture exercises multiple staging copies and the final short
    // row range. Check actual texels around its far edge, not an aggregate image.
    int const largeWidth = 1026, largeHeight = 1027;
    std::vector<unsigned char> largeTexture(static_cast<size_t>(largeWidth) * largeHeight * 4, 255);
    for (int row = 0; row < largeHeight; ++row)
        for (int column = 0; column < largeWidth; ++column)
        {
            auto texel = largeTexture.data() + (static_cast<size_t>(row) * largeWidth + column) * 4;
            texel[0] = row == 0 || row == largeHeight - 2 ? 255 : 0;
            texel[1] = row == 0 ? 0 : 255;
            texel[2] = row == largeHeight - 1 ? 255 : 0;
        }
    metalUploadTexture(2, 0, largeWidth, largeHeight, largeTexture.data());
    state.samplers[0].texture = 2;
    state.uniforms.fixed[0] = 1.f;
    state.uniforms.textureMatrix[0][5] = 0.f;
    int const checkedRows[] = { 0, largeHeight / 2, largeHeight - 2, largeHeight - 1 };
    int const expectedColors[][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 }, { 0, 255, 255 } };
    for (unsigned i = 0; i < sizeof(checkedRows) / sizeof(checkedRows[0]); ++i)
    {
        state.uniforms.textureMatrix[0][13] = (checkedRows[i] + 0.5f) / largeHeight;
        triangle(state, 1.f, 1.f, 1.f);
        pixel("large NPOT texture row", expectedColors[i][0], expectedColors[i][1], expectedColors[i][2]);
    }
    int const regionWidth = largeWidth - 2, regionHeight = largeHeight - 2;
    largeTexture.assign(static_cast<size_t>(regionWidth) * regionHeight * 4, 255);
    for (size_t i = 1; i < largeTexture.size(); i += 4) largeTexture[i] = 0;
    metalUploadTextureRegion(2, 0, 1, 1, regionWidth, regionHeight, largeTexture.data());
    state.uniforms.textureMatrix[0][13] = (largeHeight - 1.5f) / largeHeight;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage final staging chunk", 255, 0, 255);
    state.uniforms.textureMatrix[0][0] = 0.f;
    state.uniforms.textureMatrix[0][12] = 0.5f / largeWidth;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage preserves left edge", 255, 255, 0);
    state.uniforms.textureMatrix[0][12] = 0.5f;
    state.uniforms.textureMatrix[0][13] = (largeHeight - 0.5f) / largeHeight;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage preserves final row", 0, 255, 255);
    metalDeleteTexture(2);
    state = defaults();

    metalClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, blue, 1.f, 3, state);
    state.stencilTest = true;
    state.stencilFunc = GL_EQUAL;
    state.stencilReference = 3;
    triangle(state, 1.f, 0.f, 0.f);
    pixel("stencil comparison accepts matching value", 255, 0, 0);
    state.stencilReference = 1;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("stencil comparison rejects differing value", 255, 0, 0);
    state.stencilWriteMask = 1;
    metalClear(GL_STENCIL_BUFFER_BIT, black, 1.f, 0, state);
    state.stencilReference = 2;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("masked stencil clear preserves other bits", 0, 255, 0);
    state = defaults();
    state.colorWrite[0] = state.colorWrite[2] = state.colorWrite[3] = false;
    metalClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("masked color clear", 0, 0, 0);
    state = defaults();
    require(metalPresent(), "present rendered image");

    metalDeleteTexture(1);
}

void polygonPoints()
{
    NativeVertex vertices[3] = {};
    float const positions[3][2] = {{-.5f, -.5f}, {.5f, -.5f}, {0.f, .5f}};
    int const pixels[3][2] = {{16,16}, {48,16}, {32,48}};
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[i].position[0] = positions[i][0];
        vertices[i].position[1] = positions[i][1];
        vertices[i].position[3] = 1.f;
        vertices[i].color[0] = vertices[i].color[3] = 1.f;
    }
    NativeDrawState state = defaults();
    state.polygonMode = GL_POINT;
    float const black[] = {0.f, 0.f, 0.f, 1.f};
    auto check = [&](bool visible)
    {
        metalClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
        metalDraw(vertices, 3, state);
        auto const image = ambientImage(64, 64);
        unsigned covered[3] = {};
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                size_t const offset = (size_t(y) * 64 + x) * 4;
                if (!image[offset] && !image[offset + 1] && !image[offset + 2]) continue;
                require(visible, "polygon point mode preserves source-triangle face culling");
                require(image[offset] >= 253 && image[offset + 1] <= 2 && image[offset + 2] <= 2,
                        "polygon vertex points retain their material color");
                bool nearVertex = false;
                for (unsigned vertex = 0; vertex < 3; ++vertex)
                    if (std::abs(x - pixels[vertex][0]) <= 1 && std::abs(y - pixels[vertex][1]) <= 1)
                    {
                        nearVertex = true;
                        ++covered[vertex];
                    }
                require(nearVertex, "polygon point mode leaves edges and triangle interiors clear");
            }
        if (visible)
            for (unsigned vertex = 0; vertex < 3; ++vertex)
                require(covered[vertex] > 0, "each projected polygon vertex produces a visible point");
        pixel("polygon point mode leaves triangle center clear", 0, 0, 0);
    };
    check(true);
    state.cull = true;
    check(true); // CCW triangle with ordinary backface culling.
    state.cullFace = GL_FRONT;
    check(false);
    state.cullFace = GL_BACK;
    state.frontFace = GL_CW;
    check(false);
    state.frontFace = GL_CCW;
    state.cullFace = GL_FRONT_AND_BACK;
    check(false);
    std::puts("PASS: polygon point mode draws vertices only and retains triangle face culling");
}

void reversedDepthRange()
{
    NativeDrawState state = defaults();
    state.depthTest = true;
    state.depthRange[0] = 1.f;
    state.depthRange[1] = 0.f;
    float const black[] = {0.f, 0.f, 0.f, 1.f};
    metalClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, black, 1.f, 0, state);
    triangle(state, 1.f, 0.f, 0.f, 1.f, .5f);
    triangle(state, 0.f, 1.f, 0.f, 1.f, -.5f);
    pixel("reversed depth range preserves OpenGL depth comparison semantics", 255, 0, 0);
    // Non-unit descending ranges must be handled too; merely negating depth
    // without retaining the original endpoint mapping cannot satisfy both.
    state.depthRange[0] = .8f;
    state.depthRange[1] = .2f;
    metalClear(GL_DEPTH_BUFFER_BIT, black, .4f, 0, state);
    triangle(state, 0.f, 0.f, 1.f, 1.f, .5f); // Mapped depth .35 passes LESS .4.
    triangle(state, 0.f, 1.f, 0.f, 1.f, -.5f); // Mapped depth .65 fails.
    pixel("non-unit reversed depth range retains both endpoint values", 0, 0, 255);
}

void materialPrograms()
{
    NativeDrawState state = defaults();
    float const blue[] = {0.f, 0.f, 1.f, 1.f};
    unsigned char const index[] = {128,128,128,255};
    unsigned char const palette[] = {51,102,153,0};
    unsigned char const detail[] = {128,64,255,255};
    unsigned char const glow[] = {0,0,255,128};
    for (unsigned i = 0; i < 5; ++i) state.samplers[i].texture = 20 + i;
    metalUploadTexture(20, 0, 1, 1, index);
    metalUploadTexture(21, 0, 1, 1, index);
    metalUploadTexture(22, 0, 1, 1, palette);
    metalUploadTexture(23, 0, 1, 1, detail);
    metalUploadTexture(24, 0, 1, 1, glow);
    state.uniforms.misc[0] = 1.f;
    state.uniforms.misc[3] = 1.f;
    state.uniforms.correction[0] = state.uniforms.correction[1] = state.uniforms.correction[2] = 1.f;
    state.uniforms.texturePosSize[2] = state.uniforms.texturePosSize[3] = 1.f;
    state.uniforms.halfTexelPalswapPos[0] = state.uniforms.halfTexelPalswapPos[1] = 0.5f;
    state.uniforms.palswapSizeClamp[0] = state.uniforms.palswapSizeClamp[1] = 1.f;
    state.uniforms.shadeNumVis[1] = 32.f;
    state.uniforms.shadeNumVis[2] = 1.f / 32.f;
    state.uniforms.npot[1] = state.uniforms.npot[3] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("Polymost true-color material and identity color correction", 128, 128, 128);
    state.uniforms.effects[2] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("Polymost detail texture unit", 129, 64, 255);
    state.uniforms.effects[3] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("Polymost glow texture unit", 64, 32, 255);
    state.uniforms.effects[2] = state.uniforms.effects[3] = 0.f;
    state.uniforms.effects[1] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("Polymost indexed art, palette swap and base palette units", 51, 102, 153);
    metalClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    unsigned char const transparent[] = {255,255,255,255};
    metalUploadTextureRegion(20, 0, 0, 0, 1, 1, transparent);
    state.uniforms.misc[2] = 1.f;
    state.uniforms.fixed[2] = GL_GREATER;
    state.uniforms.fog[3] = 0.5f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("masked palette index 255 discards the fragment", 0, 0, 255);
    metalUploadTextureRegion(20, 0, 0, 0, 1, 1, index);
    triangle(state, 1.f, 1.f, 1.f);
    pixel("opaque indexed art survives alpha testing", 51, 102, 153);

    state.uniforms.misc[2] = 0.f;
    state.uniforms.misc[3] = 2.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("cinematic YUV material program", 131, 130, 131);
    state.uniforms.misc[3] = 3.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("paletted software-surface material program", 51, 102, 153);
    for (unsigned i = 0; i < 5; ++i) metalDeleteTexture(20 + i);

    state = defaults();
    state.uniforms.fixed[1] = 1.f;
    state.uniforms.fog[1] = state.uniforms.fog[2] = 1.f;
    state.uniforms.fogColor[2] = 1.f;
    triangle(state, 1.f, 0.f, 0.f, 1.f, 0.5f);
    pixel("fixed-function linear fog", 128, 0, 128);
}

void pendingResources()
{
    NativeDrawState state = defaults();
    float const black[] = {0.f, 0.f, 0.f, 1.f};
    metalClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 8;
    // Earlier tiles must survive attachment/command-buffer reuse. No intervening
    // readback, texture update or Finish may serialize these presentations.
    for (unsigned frame = 0; frame < 48; ++frame)
    {
        state.scissor[0] = (frame % 8) * 8;
        state.scissor[1] = (frame / 8) * 8;
        unsigned const value = frame + 1;
        triangle(state, float(value & 1), float((value >> 1) & 1), float((value >> 2) & 1));
        require(metalPresent(), "present consecutive frames without a CPU wait");
    }
    auto const image = ambientImage(64, 64);
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
        {
            unsigned const value = y < 48 ? (y / 8) * 8 + x / 8 + 1 : 0;
            size_t const offset = (y * 64 + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                require(image[offset + c] == (((value >> c) & 1) ? 255 : 0),
                        "all prior partial-frame pixels survive queued presentations");
            require(image[offset + 3] == 255, "presentation preserves target alpha");
        }

    state = defaults();
    metalClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    unsigned char const colors[3][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}};
    metalUploadTexture(17, 0, 1, 1, colors[0]);
    state.uniforms.fixed[0] = 1.f;
    state.samplers[0].texture = 17;
    state.scissorTest = true;
    state.scissor[2] = 16;
    state.scissor[3] = 64;
    for (unsigned frame = 0; frame < 3; ++frame)
    {
        if (frame == 1) metalUploadTextureRegion(17, 0, 0, 0, 1, 1, colors[1]);
        if (frame == 2)
        {
            metalDeleteTexture(17);
            metalUploadTexture(17, 0, 1, 1, colors[2]);
        }
        state.scissor[0] = frame * 16;
        for (unsigned draw = 0; draw < 96; ++draw) triangle(state, 1.f, 1.f, 1.f);
        require(metalPresent(), "present sampled images before their mutation or retirement");
    }
    metalDeleteTexture(17);
    pixel("pending texture mutation preserves earlier red tile", 255, 0, 0, 8, 32);
    pixel("pending texture deletion preserves earlier green tile", 0, 255, 0, 24, 32);
    pixel("pending texture ID reuse displays new blue tile", 0, 0, 255, 40, 32);
    state = defaults();
    triangle(state, 1.f, 0.f, 1.f);
    metalFlush();
    pixel("synchronous readback observes asynchronously flushed drawing", 255, 0, 255);
    metalFinish();
    std::puts("PASS: queued frame contents, texture mutation/deletion/ID reuse and flush/readback");
}

void ordering()
{
    NativeDrawState state = defaults();
    float const black[] = {0.f, 0.f, 0.f, 1.f};
    metalClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    state.blend = state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 4;
    for (unsigned pass = 0; pass < 12; ++pass)
        for (unsigned tile = 0; tile < 256; ++tile)
        {
            state.scissor[0] = (tile % 16) * 4;
            state.scissor[1] = (tile / 16) * 4;
            triangle(state, ((pass + tile) % 7) / 6.f, ((pass * 3 + tile) % 5) / 4.f,
                     ((pass * 5 + tile) % 11) / 10.f, 0.25f);
        }
    pixel("transparent painter order and copied per-draw state", 118, 124, 88, 1, 1);
    auto const image = ambientImage(64, 64);
    for (unsigned tile = 0; tile < 256; ++tile)
    {
        int expected[3] = {};
        for (unsigned pass = 0; pass < 12; ++pass)
        {
            float const source[3] = {((pass + tile) % 7) / 6.f,
                ((pass * 3 + tile) % 5) / 4.f, ((pass * 5 + tile) % 11) / 10.f};
            for (unsigned c = 0; c < 3; ++c)
                expected[c] = int(source[c] * 255.f * 0.25f + expected[c] * 0.75f + 0.5f);
        }
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
                for (unsigned c = 0; c < 3; ++c)
                {
                    size_t const offset = (((tile / 16) * 4 + y) * 64 + (tile % 16) * 4 + x) * 4 + c;
                    require(std::abs(int(image[offset]) - expected[c]) <= 3,
                            "every blended tile matches sequential source-alpha compositing");
                }
    }
    std::puts("PASS: all 3072 overlapping draws retain order and per-draw scissors/colors");
}

void streamRollover()
{
    NativeDrawState state = defaults();
    float const black[] = {0.f, 0.f, 0.f, 1.f};
    metalClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 1;
    // More than 64 MiB of streamed vertices crosses the production submission
    // budget without readback/Finish. Only the first triangle in each packet
    // covers a pixel; the remaining valid degenerate triangles do no work.
    std::vector<NativeVertex> vertices(12288);
    for (auto &vertex : vertices) vertex.position[3] = 1.f;
    vertices[0].position[0] = vertices[0].position[1] = -1.f;
    vertices[1].position[0] = 3.f;
    vertices[1].position[1] = -1.f;
    vertices[2].position[0] = -1.f;
    vertices[2].position[1] = 3.f;
    for (unsigned draw = 0; draw < 80; ++draw)
    {
        state.scissor[0] = draw % 64;
        state.scissor[1] = draw / 64;
        for (unsigned i = 0; i < 3; ++i)
        {
            for (unsigned c = 0; c < 3; ++c)
                vertices[i].color[c] = float(((draw + 1) >> c) & 1);
            vertices[i].color[3] = 1.f;
        }
        metalDraw(vertices.data(), static_cast<unsigned>(vertices.size()), state);
    }
    auto const image = ambientImage(64, 64);
    for (unsigned index = 0; index < 64 * 64; ++index)
        for (unsigned c = 0; c < 3; ++c)
        {
            unsigned const value = index < 80 ? index + 1 : 0;
            require(image[index * 4 + c] == (((value >> c) & 1) ? 255 : 0),
                    "all streamed packet pixels survive submission-budget rollover");
        }
    std::puts("PASS: streaming-buffer page and submission-budget rollover");
}

void lifecycle(SDL_Window *window)
{
    SDL_SetWindowSize(window, 96, 96);
    SDL_PumpEvents();
    require(metalResize(96, 96), "resize with previous submissions pending");
    NativeDrawState state = defaults(96, 96);
    triangle(state, 0.f, 1.f, 0.f);
    pixel("resized target", 0, 255, 0, 80, 80);
    require(metalPresent(), "present resized target");
    metalSetVsync(1);
    triangle(state, 0.f, 0.f, 1.f);
    require(metalPresent(), "present after changing vsync");
    require(metalResize(0, 0), "suspend a zero-sized render target");
    require(metalPresent(), "presentation tolerates a suspended render target");
    require(metalResize(96, 96), "restore the suspended render target");
    triangle(state, 0.f, 1.f, 1.f);
    pixel("restored target after suspension", 0, 255, 255, 80, 80);
    require(metalPresent(), "queue work before renderer teardown");
    metalDestroy();
    require(!metalActive(), "teardown releases native renderer ownership");
    require(metalCreate(window, 96, 96, 0), "recreate renderer after pending-frame teardown");
    triangle(state, 1.f, 0.f, 0.f);
    pixel("renderer recreation", 255, 0, 0);

    // A failed upload must not release window ownership or accidentally permit
    // the frontend to call an OpenGL driver on a Metal window.
    unsigned char const yellow[] = {255,255,0,255};
    std::puts("Checking runtime failure ownership (one invalid-subimage diagnostic is expected)");
    metalUploadTextureRegion(999, 0, 0, 0, 1, 1, yellow);
    require(metalActive(), "failed renderer retains its Metal window ownership");
    require(!metalPresent(), "runtime failure reaches presentation");
    metalDestroy();
    require(!metalActive(), "failed renderer teardown releases ownership");
    require(metalCreate(window, 96, 96, 0), "recreate renderer after a runtime failure");
    triangle(state, 0.f, 0.f, 1.f);
    pixel("recreation after runtime failure", 0, 0, 255);
    metalDestroy();
}

void ambientOcclusion(SDL_Window *window)
{
    SDL_SetWindowSize(window, 160, 120);
    SDL_PumpEvents();
    require(metalCreate(window, 160, 120, 0), "create AO renderer");
    ambientSceneChecks(160, 120);
    ambientFrame(160, 120, true, true, 1.f, true, true);
    auto const expected = ambientImage(160, 120);
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        ambientFrame(160, 120, true, true, 1.f, true, true);
        require(metalPresent(), "present AO frames without CPU readback or Finish");
    }
    require(ambientImage(160, 120) == expected, "AO remains exact across queued submissions");
    SDL_SetWindowSize(window, 240, 180);
    SDL_PumpEvents();
    require(metalResize(240, 180), "recreate AO targets after resize");
    ambientSceneChecks(240, 180);
    require(metalPresent(), "present resized AO targets");
    metalDestroy();
}

void realtimeLighting(SDL_Window *window)
{
    SDL_SetWindowSize(window, 160, 120);
    SDL_PumpEvents();
    require(metalCreate(window, 160, 120, 0), "create Metal lighting renderer");
    require(SDL_GL_GetCurrentContext() == nullptr, "Metal lighting uses no OpenGL context");
    lightingSceneChecks(160, 120);
    LightingScene scene;
    scene.hud = true;
    lightingFrame(160, 120, scene);
    auto const expected = lightingImage(160, 120);
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        // Alternate uniforms and shadow maps while previous frames are queued.
        // Only the final frame is read back, matching the original light.
        scene.movedLight = (frame & 1) == 0;
        lightingFrame(160, 120, scene);
        require(metalPresent(), "present lighting frames without readback or Finish");
    }
    require(lightingImage(160, 120) == expected, "Metal lighting remains exact across queued light changes");
    require(metalResize(0, 0), "suspend render target with lighting resources");
    require(metalPresent(), "present suspended Metal lighting renderer");
    require(metalResize(160, 120), "restore suspended Metal lighting target");
    scene.movedLight = false;
    lightingFrame(160, 120, scene);
    require(lightingImage(160, 120) == expected, "lighting survives suspension and restoration");
    SDL_SetWindowSize(window, 240, 180);
    SDL_PumpEvents();
    require(metalResize(240, 180), "resize Metal lighting targets");
    lightingSceneChecks(240, 180);
    lightingFrame(240, 180, scene);
    auto const resized = lightingImage(240, 180);
    require(metalPresent(), "present resized lighting targets");
    lightingFrame(240, 180, scene);
    require(metalPresent(), "queue lit frame before renderer teardown");
    metalDestroy();
    require(!metalActive(), "lighting teardown releases Metal renderer ownership");
    require(metalCreate(window, 240, 180, 0), "recreate Metal lighting renderer after teardown");
    lightingFrame(240, 180, scene);
    require(lightingImage(240, 180) == resized, "lighting resources recreate with identical pixels");
    metalDestroy();
    std::puts("PASS: native Metal point lighting, geometry shadows and lighting lifecycle");
}
}

int main(int argc, char **argv)
{
    bool requireDevice = false, lightingOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--require-device") == 0) requireDevice = true;
        else if (std::strcmp(argv[i], "--realtime-lighting-only") == 0) lightingOnly = true;
        else
        {
            std::fprintf(stderr, "usage: backend-test [--require-device] [--realtime-lighting-only]\n");
            return 2;
        }
    }
    if (!metalSupported())
    {
        std::fprintf(stderr, "%s: no native Metal device is available; rendering was not tested\n",
                     requireDevice ? "FAIL" : "SKIP");
        return requireDevice ? 1 : 77;
    }
    // With a Metal device present, an SDL/window/backend failure is a genuine
    // failure. It must never turn a broken implementation into a skipped test.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video initialization: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("NetBlood Metal contract checks", SDL_WINDOWPOS_UNDEFINED,
                                         SDL_WINDOWPOS_UNDEFINED, 64, 64,
                                         SDL_WINDOW_METAL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    require(window != nullptr, "create an actual SDL Metal window");
    require(SDL_GL_GetCurrentContext() == nullptr, "tests begin without an OpenGL context");
    if (lightingOnly)
    {
        realtimeLighting(window);
        require(SDL_GL_GetCurrentContext() == nullptr, "Metal lighting never created an OpenGL context");
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::puts("PASS: targeted native Metal point lighting and shadow checks");
        return 0;
    }
    require(metalCreate(window, 64, 64, 0), "create native Metal renderer");
    require(metalActive(), "native renderer owns the Metal window");
    std::printf("DEVICE: %s\n", metalDeviceName());
    require(metalMaxTextureSize() >= 1027, "native device can hold the NPOT upload fixture");
    renderingState();
    polygonPoints();
    reversedDepthRange();
    materialPrograms();
    ordering();
    pendingResources();
    streamRollover();
    lifecycle(window);
    ambientOcclusion(window);
    realtimeLighting(window);
    require(SDL_GL_GetCurrentContext() == nullptr, "native rendering never created an OpenGL context");
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("PASS: native Metal backend contract suite");
    return 0;
}
