// OpenGL point lighting/shadow regressions without game assets.
// SPDX-License-Identifier: GPL-2.0-only
#include "build.h"
#include "baselayer.h"
#include "polymost_lighting.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

glinfo_t glinfo = {};
int32_t xdim = 0, ydim = 0;
void *_xmalloc(bsize_t size) { return malloc(size); }
void *_xrealloc(void *ptr, bsize_t size) { return realloc(ptr, size); }
void _xfree(void *ptr) { free(ptr); }

namespace {
GLuint target = 0, colorTarget = 0, depthTarget = 0;
GLuint sentinelTexture = 0, sentinelSampler = 0, sentinelBuffer = 0, sentinelVao = 0, sentinelProgram = 0;

void require(bool condition, char const *message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void cleanErrors(char const *message)
{
    GLenum const error = glGetError();
    if (error != GL_NO_ERROR) std::fprintf(stderr, "OpenGL error 0x%x: %s\n", error, message);
    require(error == GL_NO_ERROR, message);
}

void targetSize(int width, int height)
{
    xdim = width; ydim = height;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (target) glDeleteFramebuffers(1, &target);
    if (colorTarget) glDeleteTextures(1, &colorTarget);
    if (depthTarget) glDeleteRenderbuffers(1, &depthTarget);
    glGenFramebuffers(1, &target);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glGenTextures(1, &colorTarget);
    glBindTexture(GL_TEXTURE_2D, colorTarget);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTarget, 0);
    glGenRenderbuffers(1, &depthTarget);
    glBindRenderbuffer(GL_RENDERBUFFER, depthTarget);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthTarget);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "create test color/depth/stencil framebuffer");
    glBindTexture(GL_TEXTURE_2D, 0);
    cleanErrors("resize test framebuffer");
}

struct Snapshot
{
    std::vector<GLint> integers;
    std::vector<GLfloat> floats;
    void integer(GLenum name, unsigned count = 1)
    {
        GLint values[16] = {};
        glGetIntegerv(name, values);
        integers.insert(integers.end(), values, values + count);
    }
    void real(GLenum name, unsigned count)
    {
        GLfloat values[16] = {};
        glGetFloatv(name, values);
        floats.insert(floats.end(), values, values + count);
    }
    Snapshot()
    {
        for (GLenum name : {GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE, GL_DRAW_FRAMEBUFFER_BINDING,
             GL_READ_FRAMEBUFFER_BINDING, GL_RENDERBUFFER_BINDING, GL_DRAW_BUFFER, GL_READ_BUFFER,
             GL_ARRAY_BUFFER_BINDING, GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_PIXEL_PACK_BUFFER_BINDING,
             GL_PIXEL_UNPACK_BUFFER_BINDING, GL_VERTEX_ARRAY_BINDING, GL_DEPTH_FUNC, GL_DEPTH_WRITEMASK,
             GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA,
             GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_STENCIL_FUNC, GL_STENCIL_REF,
             GL_STENCIL_VALUE_MASK, GL_STENCIL_WRITEMASK, GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL,
             GL_STENCIL_PASS_DEPTH_PASS, GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK,
             GL_STENCIL_BACK_WRITEMASK, GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL,
             GL_STENCIL_BACK_PASS_DEPTH_PASS, GL_CULL_FACE_MODE, GL_FRONT_FACE, GL_MATRIX_MODE,
             GL_ALPHA_TEST_FUNC, GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT}) integer(name);
        integer(GL_VIEWPORT, 4); integer(GL_SCISSOR_BOX, 4); integer(GL_COLOR_WRITEMASK, 4);
        integer(GL_POLYGON_MODE, 2);
        for (GLenum cap : {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE,
                           GL_ALPHA_TEST, GL_POLYGON_OFFSET_FILL}) integers.push_back(glIsEnabled(cap));
        real(GL_DEPTH_RANGE, 2); real(GL_BLEND_COLOR, 4); real(GL_ALPHA_TEST_REF, 1);
        real(GL_PROJECTION_MATRIX, 16); real(GL_MODELVIEW_MATRIX, 16);
        GLint active;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (unsigned unit = 0; unit < 5; ++unit)
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            integer(GL_TEXTURE_BINDING_2D);
            integer(GL_SAMPLER_BINDING);
            integers.push_back(glIsEnabled(GL_TEXTURE_2D));
        }
        glActiveTexture(active);
        cleanErrors("capture OpenGL state");
    }
    bool operator==(Snapshot const &other) const { return integers == other.integers && floats == other.floats; }
};

void unusualState()
{
    glUseProgram(sentinelProgram);
    glViewport(3, 5, 25, 31);
    glEnable(GL_SCISSOR_TEST); glScissor(2, 4, 30, 35);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GREATER); glDepthMask(GL_FALSE); glDepthRange(0.2, 0.8);
    glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); glFrontFace(GL_CW);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.9f);
    glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
    glBlendColor(0.1f, 0.2f, 0.3f, 0.4f);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilFuncSeparate(GL_FRONT, GL_GREATER, 5, 0x7f);
    glStencilFuncSeparate(GL_BACK, GL_LESS, 7, 0x3f);
    glStencilMaskSeparate(GL_FRONT, 0x33); glStencilMaskSeparate(GL_BACK, 0x55);
    glStencilOpSeparate(GL_FRONT, GL_INCR, GL_KEEP, GL_DECR);
    glStencilOpSeparate(GL_BACK, GL_REPLACE, GL_ZERO, GL_KEEP);
    glBindVertexArray(sentinelVao);
    glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sentinelBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sentinelBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sentinelBuffer);
    for (unsigned unit = 0; unit < 5; ++unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, sentinelTexture);
        glBindSampler(unit, sentinelSampler);
    }
    glActiveTexture(GL_TEXTURE3);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glTranslatef(0.1f, 0.2f, 0.3f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glScalef(2.f, 3.f, 4.f);
    glMatrixMode(GL_TEXTURE);
    cleanErrors("set caller OpenGL state");
}

void defaultState()
{
    glUseProgram(0);
    glBindVertexArray(0);
    for (GLenum buffer : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER})
        glBindBuffer(buffer, 0);
    for (unsigned unit = 0; unit < 5; ++unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0); glBindSampler(unit, 0); glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, xdim, ydim);
    for (GLenum cap : {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE,
                       GL_ALPHA_TEST, GL_POLYGON_OFFSET_FILL}) glDisable(cap);
    glDepthFunc(GL_LESS); glDepthMask(GL_TRUE); glDepthRange(0.0, 1.0);
    glStencilMask(0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    cleanErrors("restore test draw state");
}

struct Scene
{
    bool apply = true, shadows = true, offscreen = false, moved = false;
    bool eligible = true, hud = false, inset = false, emptyMesh = false, zeroLights = false, checkState = false;
};

void frame(Scene const &scene)
{
    defaultState();
    glClearColor(0.12f, 0.23f, 0.34f, 0.7f); glClearDepth(1.0); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_STENCIL_TEST);
    glStencilMask(0x80); glStencilFunc(GL_ALWAYS, scene.eligible ? 0x80 : 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    int viewport[4] = {0, 0, xdim, ydim};
    if (scene.inset)
    {
        viewport[0] = xdim / 8; viewport[1] = ydim / 8;
        viewport[2] = xdim * 3 / 4; viewport[3] = ydim * 3 / 4;
    }
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    float projection[16] = {};
    projection[0] = projection[5] = 1.f;
    projection[10] = 17.f / 15.f; projection[11] = 1.f; projection[14] = -32.f / 15.f;
    float eyeToWorld[16] = {};
    for (unsigned i = 0; i < 4; ++i) eyeToWorld[i * 5] = 1.f;
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(projection);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    std::vector<RealtimeShadowVertex> mesh;
    auto quad = [&mesh](float left, float right, float bottom, float top, float z)
    {
        float positions[4][3] = {{left,bottom,z}, {right,bottom,z}, {right,top,z}, {left,top,z}};
        glColor4f(0.35f, 0.45f, 0.55f, 0.7f);
        glBegin(GL_TRIANGLES);
        for (unsigned index : {0u, 2u, 1u, 0u, 3u, 2u})
        {
            glVertex3fv(positions[index]);
            RealtimeShadowVertex vertex;
            std::copy(positions[index], positions[index] + 3, vertex.position);
            mesh.push_back(vertex);
        }
        glEnd();
    };
    quad(-4.f, 4.f, -4.f, 4.f, 4.f);
    if (scene.offscreen) quad(-4.1f, -3.1f, -0.4f, 0.4f, 1.5f);
    else quad(-0.75f, -0.25f, -0.5f, 0.5f, 2.f);
    RealtimeLight light = {{scene.offscreen ? -6.f : (scene.moved ? 1.f : -1.f), 0.f,
                            scene.offscreen ? 0.f : 1.f, scene.offscreen ? 16.f : 8.f}, {0.8f, 0.5f, 0.2f, 1.f}};
    if (scene.apply)
    {
        if (scene.checkState) unusualState();
        Snapshot const before;
        require(polymostRealtimeLighting(projection, eyeToWorld, viewport,
                    scene.zeroLights ? nullptr : &light, scene.zeroLights ? 0 : 1,
                    scene.emptyMesh ? nullptr : mesh.data(), scene.emptyMesh ? 0 : unsigned(mesh.size()), scene.shadows),
                "apply OpenGL lighting and shadows");
        cleanErrors("OpenGL lighting pass");
        require(Snapshot() == before, "lighting restores all caller OpenGL state");
    }
    defaultState();
    if (scene.hud)
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(xdim / 4, ydim / 8, xdim / 2, ydim / 8);
        glClearColor(1.f, 1.f, 1.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
        glScissor(xdim / 4, (ydim / 8) * 2, xdim / 2, ydim / 8);
        glClearColor(1.f, 0.f, 1.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

std::vector<unsigned char> image()
{
    std::vector<unsigned char> pixels(size_t(xdim) * ydim * 4);
    glReadPixels(0, 0, xdim, ydim, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    cleanErrors("read lighting evidence");
    return pixels;
}

void evidence(char const *label, std::vector<unsigned char> const &pixels)
{
    char const *directory = std::getenv("NBLOOD_LIGHTING_TEST_OUTPUT");
    if (!directory || !*directory || xdim != 160) return;
    std::string const path = std::string(directory) + "/opengl-" + label + ".ppm";
    FILE *file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "open OpenGL evidence image");
    std::fprintf(file, "P6\n%d %d\n255\n", xdim, ydim);
    for (int y = ydim - 1; y >= 0; --y)
        for (int x = 0; x < xdim; ++x)
            require(std::fwrite(&pixels[(size_t(y) * xdim + x) * 4], 1, 3, file) == 3, "write OpenGL evidence");
    require(std::fclose(file) == 0, "close OpenGL evidence");
}

void sceneChecks()
{
    auto capture = [](Scene const &scene) { frame(scene); return image(); };
    Scene scene;
    scene.apply = false;
    auto const unlit = capture(scene);
    scene.apply = true; scene.shadows = false;
    auto const lit = capture(scene);
    scene.shadows = true;
    auto const shadowed = capture(scene);
    unsigned brightened = 0, blocked = 0, unaffected = 0;
    for (size_t i = 0; i < unlit.size(); i += 4)
    {
        require(lit[i + 3] == unlit[i + 3] && shadowed[i + 3] == unlit[i + 3], "lighting preserves alpha");
        for (unsigned c = 0; c < 3; ++c)
        {
            require(lit[i + c] + 1 >= unlit[i + c], "point lighting preserves base material brightness");
            require(shadowed[i + c] <= lit[i + c] + 1, "shadows only reduce added light");
            require(shadowed[i + c] + 1 >= unlit[i + c], "shadows preserve base material lighting");
        }
        if (lit[i] >= unlit[i] + 3) ++brightened;
        if (shadowed[i] + 3 <= lit[i]) ++blocked;
        if (lit[i] >= unlit[i] + 3 && std::abs(int(lit[i]) - int(shadowed[i])) <= 1) ++unaffected;
    }
    require(brightened > unsigned(xdim * ydim / 20), "point light visibly illuminates the receiver");
    require(blocked > unsigned(xdim * ydim / 400), "geometry shadows visible receiver pixels");
    require(unaffected > unsigned(xdim * ydim / 20), "unblocked receiver remains illuminated");
    evidence("unlit", unlit); evidence("lit", lit); evidence("shadowed", shadowed);
    scene.checkState = true;
    require(capture(scene) == shadowed, "caller OpenGL state does not change lighting output");
    scene = Scene(); scene.moved = true;
    auto const moved = capture(scene);
    unsigned changed = 0;
    for (size_t i = 0; i < moved.size(); i += 4)
        if (std::abs(int(moved[i]) - int(shadowed[i])) >= 3) ++changed;
    require(changed > unsigned(xdim * ydim / 20), "moving light updates illumination and shadows");
    scene = Scene(); scene.shadows = false;
    require(capture(scene) == lit, "disabling shadows restores exact unshadowed light");
    scene.shadows = true; scene.emptyMesh = true;
    require(capture(scene) == lit, "empty mesh does not reuse stale shadows");
    scene = Scene(); scene.apply = false;
    require(capture(scene) == unlit, "disabling lighting restores original pixels exactly");
    scene.apply = true; scene.zeroLights = true;
    require(capture(scene) == unlit, "zero lights leaves original pixels unchanged");
    scene = Scene(); scene.eligible = false;
    require(capture(scene) == unlit, "stencil-excluded geometry remains unlit");
    scene = Scene(); scene.offscreen = true; scene.shadows = false;
    auto const offscreenLit = capture(scene);
    scene.shadows = true;
    auto const offscreenShadowed = capture(scene);
    changed = 0;
    for (size_t i = 0; i < offscreenLit.size(); i += 4)
        if (offscreenShadowed[i] + 2 <= offscreenLit[i]) ++changed;
    require(changed > unsigned(xdim * ydim / 200), "caster outside camera frustum casts visible shadows");
    evidence("offscreen-lit", offscreenLit); evidence("offscreen-shadowed", offscreenShadowed);
    scene = Scene(); scene.hud = true; scene.apply = false;
    auto const hudOff = capture(scene);
    scene.apply = true;
    auto const hudOn = capture(scene);
    for (int y = ydim / 8; y < (ydim / 8) * 3; ++y)
        for (int x = xdim / 4; x < xdim * 3 / 4; ++x)
            for (unsigned c = 0; c < 4; ++c)
                require(hudOn[(size_t(y) * xdim + x) * 4 + c] == hudOff[(size_t(y) * xdim + x) * 4 + c],
                        "post-pass HUD remains unchanged");
    scene = Scene(); scene.inset = true; scene.apply = false;
    auto const insetOff = capture(scene);
    scene.apply = true;
    auto const insetOn = capture(scene);
    changed = 0;
    for (int y = 0; y < ydim; ++y)
        for (int x = 0; x < xdim; ++x)
        {
            size_t const offset = (size_t(y) * xdim + x) * 4;
            bool const outside = x < xdim / 8 || x >= xdim / 8 + xdim * 3 / 4 ||
                                 y < ydim / 8 || y >= ydim / 8 + ydim * 3 / 4;
            if (outside)
                for (unsigned c = 0; c < 4; ++c)
                    require(insetOn[offset + c] == insetOff[offset + c], "lighting respects viewport boundaries");
            else if (insetOn[offset] >= insetOff[offset] + 3) ++changed;
        }
    require(changed > unsigned(xdim * ydim / 40), "inset viewport receives lighting");
    scene = Scene();
    for (unsigned i = 0; i < 6; ++i) frame(scene);
    require(image() == shadowed, "repeated lighting frames remain identical");
    polymostResetRealtimeLighting();
    require(capture(scene) == shadowed, "lighting resources recreate correctly after reset");
    // Compatibility drivers can expose framebuffer objects without VAOs. Keep
    // the real caller VAO/buffer state, but remove VAO allocation from the
    // backend's capabilities so it exercises immediate shadow submission.
    auto const generateVertexArrays = glad_glGenVertexArrays;
    polymostResetRealtimeLighting();
    glad_glGenVertexArrays = nullptr;
    scene.checkState = true;
    require(capture(scene) == shadowed, "no-VAO shadow fallback preserves pixels and caller arrays");
    polymostResetRealtimeLighting();
    glad_glGenVertexArrays = generateVertexArrays;
    std::printf("PASS: OpenGL lighting %dx%d: %u lit, %u blocked, %u unblocked; state, bypass, offscreen, HUD, reset\n",
                xdim, ydim, brightened, blocked, unaffected);
}

void sentinels()
{
    glGenTextures(1, &sentinelTexture); glBindTexture(GL_TEXTURE_2D, sentinelTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenSamplers(1, &sentinelSampler);
    glSamplerParameteri(sentinelSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenBuffers(1, &sentinelBuffer); glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffer);
    glBufferData(GL_ARRAY_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
    glGenVertexArrays(1, &sentinelVao);
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER), fragment = glCreateShader(GL_FRAGMENT_SHADER);
    char const *vs = "#version 120\nvoid main(){gl_Position=gl_Vertex;}\n";
    char const *fs = "#version 120\nvoid main(){gl_FragColor=vec4(1.0);}\n";
    glShaderSource(vertex, 1, &vs, nullptr); glCompileShader(vertex);
    glShaderSource(fragment, 1, &fs, nullptr); glCompileShader(fragment);
    sentinelProgram = glCreateProgram(); glAttachShader(sentinelProgram, vertex); glAttachShader(sentinelProgram, fragment);
    glLinkProgram(sentinelProgram);
    GLint linked = GL_FALSE; glGetProgramiv(sentinelProgram, GL_LINK_STATUS, &linked);
    require(linked == GL_TRUE, "create caller sentinel shader");
    glDeleteShader(vertex); glDeleteShader(fragment);
    cleanErrors("create caller sentinel resources");
}
} // namespace

int main()
{
    require(SDL_Init(SDL_INIT_VIDEO) == 0, "initialize SDL video");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window *window = SDL_CreateWindow("OpenGL lighting tests", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          160, 120, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    require(window != nullptr, "create SDL OpenGL window");
    SDL_GLContext context = SDL_GL_CreateContext(window);
    require(context != nullptr, "create compatibility OpenGL context");
    require(gladLoadGLLoader(SDL_GL_GetProcAddress) != 0, "load real OpenGL entry points");
    std::printf("OpenGL test renderer: %s; %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glinfo.samplerobjects = 1; glinfo.fbos = 1; glinfo.glsl = 1; glinfo.depthtex = 1;
    sentinels();
    targetSize(160, 120); sceneChecks();
    targetSize(240, 180); sceneChecks();
    // An unsupported source target must disable the optional pass cleanly,
    // then permit recovery when a supported context/target is restored.
    defaultState();
    glBindRenderbuffer(GL_RENDERBUFFER, depthTarget);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, xdim, ydim);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "create unsupported depth-only test target");
    float projection[16] = {}, eyeToWorld[16] = {};
    projection[0] = projection[5] = 1.f; projection[10] = 17.f / 15.f;
    projection[11] = 1.f; projection[14] = -32.f / 15.f;
    for (unsigned i = 0; i < 4; ++i) eyeToWorld[i * 5] = 1.f;
    int viewport[4] = {0, 0, xdim, ydim};
    RealtimeLight light = {{0.f, 0.f, 1.f, 8.f}, {0.8f, 0.5f, 0.2f, 1.f}};
    require(!polymostRealtimeLighting(projection, eyeToWorld, viewport, &light, 1, nullptr, 0, false),
            "unsupported depth target bypasses optional lighting");
    require(!polymostRealtimeLightingAvailable(), "unsupported target reports lighting unavailable");
    polymostResetRealtimeLighting();
    require(polymostRealtimeLightingAvailable(), "reset permits lighting to recover on a supported target");
    targetSize(160, 120);
    Scene recovered;
    frame(recovered);
    require(polymostRealtimeLightingAvailable(), "lighting recovers after unsupported depth target reset");
    polymostResetRealtimeLighting();
    defaultState();
    glDeleteFramebuffers(1, &target); glDeleteTextures(1, &colorTarget); glDeleteRenderbuffers(1, &depthTarget);
    glDeleteTextures(1, &sentinelTexture); glDeleteSamplers(1, &sentinelSampler);
    glDeleteBuffers(1, &sentinelBuffer); glDeleteVertexArrays(1, &sentinelVao); glDeleteProgram(sentinelProgram);
    cleanErrors("destroy OpenGL lighting resources");
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    std::puts("PASS: all OpenGL point-light and geometry shadow checks");
    return 0;
}
