// Native OpenGL Polymost point lighting and world geometry shadow maps.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#include "polymost_lighting.h"

#ifdef USE_OPENGL
#include "build.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#ifndef EDUKE32_GLES
extern char const *ambient_occlusion;
extern char const *realtime_lighting;

namespace {
constexpr int kFaceSize = 256, kFaces = 6;
static_assert(kMaxRealtimeLights == 8, "Keep the GLSL 1.20 light arrays in sync");
struct LightingResources
{
    GLuint program = 0, maskProgram = 0, shadowProgram = 0;
    GLuint depthTexture = 0, depthFramebuffer = 0;
    GLuint shadowTexture = 0, shadowFramebuffer = 0, shadowBuffer = 0, vertexArray = 0;
    GLint projection = -1, depth = -1, viewport = -1, target = -1, eyeToWorld = -1;
    GLint params = -1, positions = -1, colors = -1, shadowLight = -1, shadowFace = -1;
    int width = 0, height = 0, depthBits = 0;
    bool atlasInitialized = false, failed = false;
} g;

// buildgl caches remain untouched: every raw GL mutation is restored to its
// original driver value, including bindings not covered by the attribute stack.
struct SavedState
{
    GLint program = 0, readFramebuffer = 0, drawFramebuffer = 0, activeTexture = 0;
    GLint arrayBuffer = 0, unpackBuffer = 0, vertexArray = 0, sampler[2] = {};
    bool samplers, vertexArrays;

    SavedState() : samplers(glinfo.samplerobjects && glBindSampler),
                   vertexArrays(glBindVertexArray && glGenVertexArrays && glDeleteVertexArrays)
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        if (vertexArrays) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        for (int unit = 0; unit < 2; ++unit)
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            if (samplers)
            {
                glGetIntegerv(GL_SAMPLER_BINDING, &sampler[unit]);
                glBindSampler(unit, 0);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    ~SavedState()
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glUseProgram(program);
        if (vertexArrays) glBindVertexArray(vertexArray);
        glPopClientAttrib();
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        for (int unit = 0; unit < 2; ++unit)
            if (samplers) glBindSampler(unit, sampler[unit]);
        glPopAttrib();
        glActiveTexture(activeTexture);
    }
};

void destroyResources()
{
    if (g.program) glDeleteProgram(g.program);
    if (g.maskProgram) glDeleteProgram(g.maskProgram);
    if (g.shadowProgram) glDeleteProgram(g.shadowProgram);
    if (g.depthTexture) glDeleteTextures(1, &g.depthTexture);
    if (g.shadowTexture) glDeleteTextures(1, &g.shadowTexture);
    if (g.depthFramebuffer) glDeleteFramebuffers(1, &g.depthFramebuffer);
    if (g.shadowFramebuffer) glDeleteFramebuffers(1, &g.shadowFramebuffer);
    if (g.shadowBuffer) glDeleteBuffers(1, &g.shadowBuffer);
    if (g.vertexArray) glDeleteVertexArrays(1, &g.vertexArray);
    g = LightingResources();
}

bool unavailable(char const *reason)
{
    if (!g.failed) std::fprintf(stderr, "Polymost: realtime lighting disabled; %s\n", reason);
    destroyResources();
    g.failed = true;
    return false;
}

GLuint compile(GLenum stage, char const *source)
{
    GLuint shader = glCreateShader(stage);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Polymost lighting shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint program(char const *vertexSource, char const *fragmentSource)
{
    GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    GLuint result = 0;
    if (vertex && fragment)
    {
        result = glCreateProgram();
        if (result)
        {
            glAttachShader(result, vertex);
            glAttachShader(result, fragment);
            glLinkProgram(result);
            GLint linked = GL_FALSE;
            glGetProgramiv(result, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                char log[2048] = {};
                glGetProgramInfoLog(result, sizeof(log), nullptr, log);
                std::fprintf(stderr, "Polymost lighting program: %s\n", log);
                glDeleteProgram(result);
                result = 0;
            }
        }
    }
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return result;
}

bool createPrograms()
{
    if (g.program && g.maskProgram && g.shadowProgram) return true;
    char const *fullscreen = "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n";
    std::string fragment =
        "#version 120\n"
        "uniform sampler2D u_depth, s_shadow;\n"
        "uniform vec4 u_aoProjection, u_aoDepth, u_aoViewport, u_aoTarget;\n"
        "uniform mat4 u_eyeToWorld;\n"
        "uniform vec4 u_lightParams, u_lightPositionRadius[8], u_lightColor[8];\n"
        "#define AO_DEPTH(uv) texture2D(u_depth, uv).r\n"
        "#define LIGHTING_SHADOW(uv) texture2D(s_shadow, uv).r\n"
        "#define LIGHT_POSITION(i) u_lightPositionRadius[i]\n"
        "#define LIGHT_COLOR(i) u_lightColor[i]\n";
    fragment += ambient_occlusion;
    fragment += '\n';
    fragment += realtime_lighting;
    fragment += "\nvoid main() { gl_FragColor = vec4(realtimeLighting(gl_FragCoord.xy), 0.0); }\n";
    g.program = program(fullscreen, fragment.c_str());
    g.maskProgram = program(fullscreen,
        "#version 120\nvoid main() { gl_FragColor = vec4(0.0); gl_FragDepth = 1.0; }\n");
    // GL's clip depth is [-w,w], while the Vulkan shadow vertex shader emits
    // [0,w]. Everything after this one remapping uses the same radial depth.
    g.shadowProgram = program(
        "#version 120\n"
        "uniform vec4 u_light; uniform int u_face; varying vec3 v_lightPosition;\n"
        "void main() {\n"
        " vec3 p = gl_Vertex.xyz - u_light.xyz; v_lightPosition = p; vec3 face;\n"
        " if (u_face == 0) face = vec3(-p.z, -p.y, p.x);\n"
        " else if (u_face == 1) face = vec3(p.z, -p.y, -p.x);\n"
        " else if (u_face == 2) face = vec3(p.x, p.z, p.y);\n"
        " else if (u_face == 3) face = vec3(p.x, -p.z, -p.y);\n"
        " else if (u_face == 4) face = vec3(p.x, -p.y, p.z);\n"
        " else face = vec3(-p.x, -p.y, -p.z);\n"
        " float nearPlane = max(0.001, u_light.w / 4096.0);\n"
        " float z = u_light.w * (face.z - nearPlane) / (u_light.w - nearPlane);\n"
        " gl_Position = vec4(face.xy, 2.0 * z - face.z, face.z);\n"
        "}\n",
        "#version 120\n"
        "uniform vec4 u_light; varying vec3 v_lightPosition;\n"
        "void main() { gl_FragDepth = min(length(v_lightPosition) / u_light.w, 1.0); }\n");
    if (!g.program || !g.maskProgram || !g.shadowProgram) return false;
    g.projection = glGetUniformLocation(g.program, "u_aoProjection");
    g.depth = glGetUniformLocation(g.program, "u_aoDepth");
    g.viewport = glGetUniformLocation(g.program, "u_aoViewport");
    g.target = glGetUniformLocation(g.program, "u_aoTarget");
    g.eyeToWorld = glGetUniformLocation(g.program, "u_eyeToWorld");
    g.params = glGetUniformLocation(g.program, "u_lightParams");
    g.positions = glGetUniformLocation(g.program, "u_lightPositionRadius[0]");
    g.colors = glGetUniformLocation(g.program, "u_lightColor[0]");
    g.shadowLight = glGetUniformLocation(g.shadowProgram, "u_light");
    g.shadowFace = glGetUniformLocation(g.shadowProgram, "u_face");
    glUseProgram(g.program);
    glUniform1i(glGetUniformLocation(g.program, "u_depth"), 0);
    glUniform1i(glGetUniformLocation(g.program, "s_shadow"), 1);
    return true;
}

void textureParameters()
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

bool textureSize(int width, int height)
{
    GLint actualWidth = 0, actualHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
    return actualWidth == width && actualHeight == height;
}

bool createTargets(int width, int height, int depthBits, bool vertexArrays)
{
    if (!g.depthTexture) glGenTextures(1, &g.depthTexture);
    if (!g.depthFramebuffer) glGenFramebuffers(1, &g.depthFramebuffer);
    glBindTexture(GL_TEXTURE_2D, g.depthTexture);
    if (g.width != width || g.height != height || g.depthBits != depthBits)
    {
        GLenum const format = depthBits == 32 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
        GLenum const type = depthBits == 32 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8;
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_DEPTH_STENCIL, type, nullptr);
        textureParameters();
        if (!textureSize(width, height)) return false;
        g.width = width; g.height = height; g.depthBits = depthBits;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g.depthFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g.depthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    if (!g.shadowTexture)
    {
        glGenTextures(1, &g.shadowTexture);
        glBindTexture(GL_TEXTURE_2D, g.shadowTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kFaceSize * kMaxRealtimeLights, kFaceSize * kFaces,
                     0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        textureParameters();
        if (!textureSize(kFaceSize * kMaxRealtimeLights, kFaceSize * kFaces)) return false;
        glGenFramebuffers(1, &g.shadowFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, g.shadowFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g.shadowTexture, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    }
    if (!g.shadowBuffer) glGenBuffers(1, &g.shadowBuffer);
    if (vertexArrays && !g.vertexArray) glGenVertexArrays(1, &g.vertexArray);
    return g.depthTexture && g.depthFramebuffer && g.shadowTexture && g.shadowFramebuffer && g.shadowBuffer &&
           (!vertexArrays || g.vertexArray);
}

void triangle()
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.f, -1.f, 1.f);
    glVertex3f(3.f, -1.f, 1.f);
    glVertex3f(-1.f, 3.f, 1.f);
    glEnd();
}
} // namespace
#endif

void polymostResetRealtimeLighting()
{
#ifndef EDUKE32_GLES
    destroyResources();
#endif
}

bool polymostRealtimeLightingAvailable()
{
#ifndef EDUKE32_GLES
    return !g.failed;
#else
    return false;
#endif
}

bool polymostRealtimeLighting(float const projection[16], float const eyeToWorld[16], int const viewport[4],
                              RealtimeLight const *lights, unsigned lightCount,
                              RealtimeShadowVertex const *shadowVertices, unsigned shadowVertexCount, bool shadows)
{
    if (!lightCount) return true;
#ifdef EDUKE32_GLES
    (void)projection; (void)eyeToWorld; (void)viewport; (void)lights;
    (void)shadowVertices; (void)shadowVertexCount; (void)shadows;
    return false;
#else
    if (g.failed) return false;
    if (!projection || !eyeToWorld || !viewport || !lights || lightCount > kMaxRealtimeLights) return false;
    if (shadows && (shadowVertexCount > INT_MAX ||
        size_t(shadowVertexCount) > size_t(std::numeric_limits<GLsizeiptr>::max()) / sizeof(RealtimeShadowVertex) ||
        shadowVertexCount % 3 || (shadowVertexCount && !shadowVertices))) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(projection[i]) || !std::isfinite(eyeToWorld[i])) return false;
    for (unsigned i = 0; i < lightCount; ++i)
    {
        for (unsigned j = 0; j < 4; ++j)
            if (!std::isfinite(lights[i].positionRadius[j]) || !std::isfinite(lights[i].color[j]) || lights[i].color[j] < 0.f) return false;
        if (lights[i].positionRadius[3] <= 0.001f) return false;
    }
    if (viewport[2] <= 0 || viewport[3] <= 0 || xdim <= 0 || ydim <= 0) return true;
    if (projection[0] <= 0.f || projection[5] <= 0.f ||
        std::fabs(projection[11] - 1.f) > 0.00001f || std::fabs(projection[15]) > 0.00001f) return false;
    if (!glGenFramebuffers || !glDeleteFramebuffers || !glBindFramebuffer || !glBlitFramebuffer ||
        !glFramebufferTexture2D || !glCheckFramebufferStatus || !glCreateShader || !glCreateProgram ||
        !glBindBuffer || !glBufferData || !glPushAttrib || !glPushClientAttrib)
        return unavailable("OpenGL framebuffer blitting and compatibility shaders are required");
    GLint depthBits = 0, stencilBits = 0, samples = 0, maxTextureSize = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if ((depthBits != 24 && depthBits != 32) || stencilBits < 8 || samples > 1 ||
        xdim > maxTextureSize || ydim > maxTextureSize || kFaceSize * int(kMaxRealtimeLights) > maxTextureSize)
        return unavailable("a single-sample packed depth/stencil target and 2048-pixel textures are required");
    int64_t const x0 = std::max<int64_t>(0, viewport[0]);
    int64_t const y0 = std::max<int64_t>(0, viewport[1]);
    int64_t const x1 = std::min<int64_t>(xdim, int64_t(viewport[0]) + viewport[2]);
    int64_t const y1 = std::min<int64_t>(ydim, int64_t(viewport[1]) + viewport[3]);
    if (x1 <= x0 || y1 <= y0) return true;

    SavedState const saved;
    if (!createPrograms() || !createTargets(xdim, ydim, depthBits, saved.vertexArrays))
        return unavailable("lighting shaders or depth framebuffers could not be created");
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DITHER);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_POLYGON_SMOOTH);
    glDisable(GL_MULTISAMPLE);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_ALPHA_TO_ONE);
    glDisable(GL_SAMPLE_COVERAGE);
    if (glinfo.depthclamp) glDisable(GL_DEPTH_CLAMP);
    for (unsigned i = 0; i < 6; ++i) glDisable(GL_CLIP_PLANE0 + i);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthRange(0., 1.);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, saved.drawFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.depthFramebuffer);
    glBlitFramebuffer(0, 0, xdim, ydim, 0, 0, xdim, ydim, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glViewport(0, 0, xdim, ydim);
    glUseProgram(g.maskProgram);
    glDepthFunc(GL_ALWAYS);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilFunc(GL_EQUAL, 0, 128);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    // Sanitize only the private copy. Skies, mirrors, portals and HUD pixels
    // neither receive lighting nor distort neighboring reconstructed normals.
    glBindTexture(GL_TEXTURE_2D, 0);
    triangle();

    glDisable(GL_STENCIL_TEST);
    glDepthFunc(GL_LEQUAL);
    if (shadows || !g.atlasInitialized)
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.shadowFramebuffer);
        glClearDepth(1.);
        glClear(GL_DEPTH_BUFFER_BIT);
        if (shadows && shadowVertexCount)
        {
            if (saved.vertexArrays)
            {
                glBindVertexArray(g.vertexArray);
                glBindBuffer(GL_ARRAY_BUFFER, g.shadowBuffer);
                // Orphaning lets the driver retain any mesh referenced by queued
                // draws. No CPU readback, glFinish, or explicit queue wait is used.
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(shadowVertexCount) * sizeof(RealtimeShadowVertex), shadowVertices, GL_STREAM_DRAW);
                glEnableClientState(GL_VERTEX_ARRAY);
                glVertexPointer(3, GL_FLOAT, sizeof(RealtimeShadowVertex), nullptr);
            }
            glUseProgram(g.shadowProgram);
            glEnable(GL_SCISSOR_TEST);
            for (unsigned light = 0; light < lightCount; ++light)
            {
                glUniform4fv(g.shadowLight, 1, lights[light].positionRadius);
                for (int face = 0; face < kFaces; ++face)
                {
                    glUniform1i(g.shadowFace, face);
                    glViewport(light * kFaceSize, face * kFaceSize, kFaceSize, kFaceSize);
                    glScissor(light * kFaceSize, face * kFaceSize, kFaceSize, kFaceSize);
                    if (saved.vertexArrays) glDrawArrays(GL_TRIANGLES, 0, shadowVertexCount);
                    else
                    {
                        // Older compatibility contexts without VAOs must not
                        // fetch unrelated caller arrays or modify generic
                        // attributes, which PushClientAttrib cannot preserve.
                        glBegin(GL_TRIANGLES);
                        for (unsigned i = 0; i < shadowVertexCount; ++i)
                            glVertex3fv(shadowVertices[i].position);
                        glEnd();
                    }
                }
            }
        }
        g.atlasInitialized = true;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved.drawFramebuffer);
    glViewport(0, 0, xdim, ydim);
    glUseProgram(g.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.depthTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g.shadowTexture);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_DST_COLOR, GL_ONE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(GLint(x0), GLint(y0), GLsizei(x1 - x0), GLsizei(y1 - y0));
    glUniform4f(g.projection, projection[0], projection[5], projection[8], projection[9]);
    glUniform4f(g.depth, projection[10], projection[14], 0.f, 0.f);
    glUniform4f(g.viewport, float(viewport[0]), float(viewport[1]), float(viewport[2]), float(viewport[3]));
    glUniform4f(g.target, 1.f / xdim, 1.f / ydim, 0.f, 0.f);
    glUniformMatrix4fv(g.eyeToWorld, 1, GL_FALSE, eyeToWorld);
    glUniform4f(g.params, float(lightCount), shadows ? 1.f : 0.f, float(kFaceSize), float(kMaxRealtimeLights));
    float positions[kMaxRealtimeLights][4], colors[kMaxRealtimeLights][4];
    for (unsigned i = 0; i < lightCount; ++i)
        for (unsigned j = 0; j < 4; ++j)
        {
            positions[i][j] = lights[i].positionRadius[j];
            colors[i][j] = lights[i].color[j];
        }
    glUniform4fv(g.positions, lightCount, positions[0]);
    glUniform4fv(g.colors, lightCount, colors[0]);
    triangle();
    return true;
#endif
}
#endif
