// Native Vulkan command adapter for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
// This file implements the engine's legacy graphics vocabulary, not an OpenGL
// context. Every submitted primitive and image is owned by the Vulkan backend.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "glbuild.h"
#include "vulkan_renderer.h"

#ifdef USE_VULKAN
namespace
{
using Matrix = std::array<float, 16>;

[[noreturn]] void fail(const char *operation)
{
    LOG_F(ERROR, "Vulkan frontend: unsupported or invalid %s", operation);
    Bexit(1);
}

Matrix identity()
{
    Matrix m = {};
    m[0] = m[5] = m[10] = m[15] = 1.f;
    return m;
}

Matrix multiply(const Matrix &a, const float *b)
{
    Matrix out = {};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                out[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
    return out;
}

struct Array
{
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    const void *pointer = nullptr;
    GLuint buffer = 0;
    bool enabled = false, normalized = false;
};
struct Buffer { std::vector<uint8_t> bytes; bool bound = false; };
struct Image { int width = 0, height = 0; std::vector<uint8_t> rgba; };
struct Texture
{
    std::map<int, Image> levels;
    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR;
    GLenum wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    float anisotropy = 1.f;
    int baseLevel = 0, maxLevel = 1000;
    bool bound = false;
};
struct Shader { GLenum type = 0; std::string source; bool compiled = false; };
struct Program
{
    std::vector<GLuint> shaders;
    VulkanUniforms uniforms = {};
    int kind = 0;
    bool linked = false;
    int samplerUnits[5] = {0, 1, 2, 3, 4};
};
struct PixelStore { int alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0; };
struct Frontend
{
    VulkanDrawState draw = {};
    unsigned activeTexture = 0, clientTexture = 0;
    GLenum matrixMode = GL_MODELVIEW;
    std::vector<Matrix> modelView{identity()}, projection{identity()};
    std::array<std::vector<Matrix>, 16> textureMatrices;
    std::array<GLuint, 16> textures = {};
    std::array<bool, 16> textureEnabled = {};
    std::array<GLenum, 16> texEnvMode;
    std::array<float, 16> texEnvScale;
    std::array<std::array<float, 4>, 16> texcoords;
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    Array vertex, colorArray;
    std::array<Array, 16> texcoordArrays, attributes;
    GLuint arrayBuffer = 0, elementBuffer = 0, program = 0;
    GLenum alphaFunc = GL_ALWAYS, fogMode = GL_EXP;
    bool alphaTest = false, fogEnabled = false;
    float alphaRef = 0.f, fogStart = 0.f, fogEnd = 1.f, fogDensity = 1.f;
    float fogColor[4] = {};
    float clearColor[4] = {}, clearAccum[4] = {};
    float clearDepth = 1.f;
    uint32_t clearStencil = 0;
    PixelStore pack, unpack;
    std::set<GLenum> enables;
    GLenum immediateMode = 0;
    bool immediate = false;
    std::vector<VulkanVertex> immediateVertices;
    int width = 0, height = 0;
    Frontend()
    {
        for (auto &m : textureMatrices) m.push_back(identity());
        for (auto &t : texcoords) t = {{0.f, 0.f, 0.f, 1.f}};
        texEnvMode.fill(GL_MODULATE);
        texEnvScale.fill(1.f);
        draw.depthFunc = GL_LESS;
        draw.depthWrite = true;
        draw.blendSrc = GL_ONE; draw.blendDst = GL_ZERO; draw.blendEquation = GL_FUNC_ADD;
        draw.cullFace = GL_BACK; draw.frontFace = GL_CCW;
        draw.polygonMode = GL_FILL;
        draw.stencilFunc = GL_ALWAYS;
        draw.stencilFail = draw.stencilDepthFail = draw.stencilPass = GL_KEEP;
        draw.stencilCompareMask = draw.stencilWriteMask = ~0u;
        draw.depthRange[1] = 1.f;
        draw.lineWidth = 1.f;
        for (bool &v : draw.colorWrite) v = true;
    }
};
Frontend state;
struct Attributes
{
    GLbitfield mask;
    Frontend state;
    // Texture image storage is not an attribute. Save parameters only, avoiding
    // copies of large ART atlases when GL_TEXTURE_BIT is requested.
    std::map<GLuint, Texture> textureParameters;
};
std::vector<Attributes> attributeStack;
std::map<GLuint, Buffer> buffers;
std::map<GLuint, Texture> textures;
std::map<GLuint, Shader> shaders;
std::map<GLuint, Program> programs;
std::set<GLsync> fences;
GLuint nextBuffer = 1, nextTexture = 1, nextObject = 1;
bool installed = false;
std::vector<float> accumulation;
GLuint accumulationTexture = 0;

void materialDefaults(VulkanUniforms &u)
{
    u = {};
    auto const one = identity();
    memcpy(u.rotation, one.data(), sizeof(u.rotation));
    u.texturePosSize[2] = u.texturePosSize[3] = 1.f;
    u.palswapSizeClamp[0] = u.palswapSizeClamp[1] = 1.f;
    u.shadeNumVis[1] = 32.f; u.shadeNumVis[2] = 1.f / 32.f;
    u.fog[0] = 1.f;
    u.npot[1] = u.npot[3] = 1.f;
    u.correction[0] = u.correction[1] = u.correction[2] = 1.f;
    u.misc[0] = 1.f;
}

VulkanDrawState snapshot()
{
    VulkanDrawState out = state.draw;
    materialDefaults(out.uniforms);
    int kind = 0;
    if (state.program)
    {
        auto it = programs.find(state.program);
        if (it == programs.end() || !it->second.linked) fail("shader program binding");
        out.uniforms = it->second.uniforms;
        kind = it->second.kind;
    }
    auto &u = out.uniforms;
    memcpy(u.modelView, state.modelView.back().data(), sizeof(u.modelView));
    // Polymost's compatibility shader consumes gl_ModelViewProjectionMatrix,
    // which OpenGL precomposes on the CPU. Multiplying M*v and then P*result on
    // the GPU changes depth rounding at model/floor intersections.
    auto const projection = kind == 1 ? multiply(state.projection.back(), state.modelView.back().data()) : state.projection.back();
    memcpy(u.projection, projection.data(), sizeof(u.projection));
    const int units[] = {0, 3, 4};
    for (int i = 0; i < 3; ++i)
        memcpy(u.textureMatrix[i], state.textureMatrices[units[i]].back().data(), sizeof(u.textureMatrix[i]));
    u.fog[1] = state.fogEnd;
    // Fixed-function equality uses distance-end, equivalent to scale -1 in
    // the shader's (end-distance)*scale form. Ordinary ranges are signed.
    u.fog[2] = state.fogEnd == state.fogStart ? -1.f : 1.f / (state.fogEnd - state.fogStart);
    if (kind == 1)
    {
        // The GLSL compatibility builtin gl_Fog.scale is a positive finite
        // reciprocal, including reversed/equal ranges. Polymost uses a large
        // negative end value for zero visibility, so a signed reciprocal here
        // would visibly disagree with its OpenGL shader. Fixed-function fog
        // retains its separate signed-range behavior.
        float const minimum = std::numeric_limits<float>::min() * 0.5f;
        float const maximum = std::numeric_limits<float>::max() * 0.5f;
        float const scale = state.fogEnd == state.fogStart ? maximum : u.fog[2];
        u.fog[2] = std::max(minimum, std::min(maximum, scale));
    }
    u.fog[3] = state.alphaRef;
    memcpy(u.fogColor, state.fogColor, sizeof(u.fogColor));
    u.misc[2] = state.alphaTest ? 1.f : 0.f;
    u.misc[3] = float(kind);
    u.fixed[0] = state.textureEnabled[0] ? (state.texEnvMode[0] == GL_REPLACE ? 2.f : 1.f) : 0.f;
    u.fixed[1] = state.fogEnabled ? 1.f : 0.f;
    u.fixed[2] = float(state.alphaFunc);
    u.fixed[3] = float(state.fogMode);
    if (kind == 0) u.fog[0] = state.fogDensity;
    for (int i = 0; i < 5; ++i)
    {
        int unit = state.program ? programs[state.program].samplerUnits[i] : i;
        if (unit < 0 || unit >= 16) fail("sampler texture unit");
        auto &sampler = out.samplers[i];
        sampler.texture = state.textures[unit];
        auto const it = textures.find(sampler.texture);
        if (it != textures.end())
        {
            auto const &t = it->second;
            sampler.minFilter = t.minFilter; sampler.magFilter = t.magFilter;
            sampler.wrapS = t.wrapS; sampler.wrapT = t.wrapT;
            sampler.anisotropy = t.anisotropy;
            sampler.minLod = float(t.baseLevel); sampler.maxLod = float(t.maxLevel);
        }
        else
        {
            sampler.minFilter = sampler.magFilter = GL_NEAREST;
            sampler.wrapS = sampler.wrapT = GL_REPEAT;
            sampler.anisotropy = 1.f; sampler.minLod = sampler.maxLod = 0.f;
        }
    }
    return out;
}

std::vector<Matrix> &matrixStack()
{
    if (state.matrixMode == GL_MODELVIEW) return state.modelView;
    if (state.matrixMode == GL_PROJECTION) return state.projection;
    if (state.matrixMode == GL_TEXTURE) return state.textureMatrices[state.activeTexture];
    fail("matrix mode");
}
void submit(GLenum mode, const std::vector<VulkanVertex> &input)
{
    if (input.empty()) return;
    auto draw = snapshot();
    std::vector<VulkanVertex> vertices;
    auto triangle = [&](size_t a, size_t b, size_t c) { vertices.push_back(input[a]); vertices.push_back(input[b]); vertices.push_back(input[c]); };
    switch (mode)
    {
        case GL_TRIANGLES: case GL_LINES: case GL_POINTS: vertices = input; draw.topology = mode; break;
        case GL_TRIANGLE_FAN: case GL_POLYGON:
            for (size_t i = 2; i < input.size(); ++i) triangle(0, i - 1, i);
            draw.topology = GL_TRIANGLES; break;
        case GL_TRIANGLE_STRIP:
            for (size_t i = 2; i < input.size(); ++i) triangle(i - 2 + (i & 1), i - 1 - (i & 1), i);
            draw.topology = GL_TRIANGLES; break;
        case GL_QUADS:
            for (size_t i = 0; i + 3 < input.size(); i += 4) { triangle(i, i + 1, i + 2); triangle(i, i + 2, i + 3); }
            draw.topology = GL_TRIANGLES; break;
        case GL_QUAD_STRIP:
            for (size_t i = 0; i + 3 < input.size(); i += 2) { triangle(i, i + 1, i + 3); triangle(i, i + 3, i + 2); }
            draw.topology = GL_TRIANGLES; break;
        case GL_LINE_STRIP: case GL_LINE_LOOP:
            for (size_t i = 1; i < input.size(); ++i) { vertices.push_back(input[i - 1]); vertices.push_back(input[i]); }
            if (mode == GL_LINE_LOOP && input.size() > 1) { vertices.push_back(input.back()); vertices.push_back(input.front()); }
            draw.topology = GL_LINES; break;
        default: fail("primitive topology");
    }
    if (!vertices.empty()) vulkanDraw(vertices.data(), unsigned(vertices.size()), draw);
}

size_t scalarSize(GLenum type)
{
    switch (type)
    {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
        case GL_DOUBLE: return 8;
        default: fail("vertex component type");
    }
}
template<typename T> T readScalar(const uint8_t *p) { T value; memcpy(&value, p, sizeof(value)); return value; }
float component(const uint8_t *p, GLenum type, bool normalized)
{
    switch (type)
    {
        case GL_FLOAT: return readScalar<float>(p);
        case GL_DOUBLE: return float(readScalar<double>(p));
        case GL_UNSIGNED_BYTE: return *p * (normalized ? 1.f / 255.f : 1.f);
        case GL_BYTE: return normalized ? std::max(-1.f, readScalar<int8_t>(p) / 127.f) : readScalar<int8_t>(p);
        case GL_UNSIGNED_SHORT: return readScalar<uint16_t>(p) * (normalized ? 1.f / 65535.f : 1.f);
        case GL_SHORT: return normalized ? std::max(-1.f, readScalar<int16_t>(p) / 32767.f) : readScalar<int16_t>(p);
        case GL_UNSIGNED_INT: return float(readScalar<uint32_t>(p) * (normalized ? 1. / 4294967295. : 1.));
        case GL_INT: return normalized ? float(std::max(-1., readScalar<int32_t>(p) / 2147483647.)) : float(readScalar<int32_t>(p));
        default: fail("vertex component");
    }
}
void readArray(const Array &a, unsigned index, float out[4])
{
    if (a.size < 1 || a.size > 4 || a.stride < 0) fail("vertex array layout");
    size_t size = scalarSize(a.type), stride = a.stride ? size_t(a.stride) : size * a.size;
    size_t offset = size_t(index) * stride;
    const uint8_t *p = static_cast<const uint8_t *>(a.pointer);
    if (a.buffer)
    {
        auto it = buffers.find(a.buffer);
        if (it == buffers.end()) fail("deleted vertex buffer");
        size_t const base = reinterpret_cast<uintptr_t>(a.pointer);
        if (base > it->second.bytes.size() || offset > it->second.bytes.size() - base || size * a.size > it->second.bytes.size() - base - offset)
            fail("vertex buffer bounds");
        p = it->second.bytes.data() + base + offset;
    }
    else
    {
        if (!p) fail("null client vertex array");
        p += offset;
    }
    out[0] = out[1] = out[2] = 0.f; out[3] = 1.f;
    for (int j = 0; j < a.size; ++j) out[j] = component(p + j * size, a.type, a.normalized);
}
VulkanVertex arrayVertex(unsigned index)
{
    VulkanVertex v = {};
    memcpy(v.color, state.color, sizeof(v.color));
    readArray(state.attributes[0].enabled ? state.attributes[0] : state.vertex, index, v.position);
    if (state.colorArray.enabled) readArray(state.colorArray, index, v.color);
    const int units[] = {0, 3, 4};
    for (int i = 0; i < 3; ++i)
    {
        auto const &a = i == 0 && state.attributes[1].enabled ? state.attributes[1] : state.texcoordArrays[units[i]];
        if (a.enabled) readArray(a, index, v.texcoord[i]);
        else memcpy(v.texcoord[i], state.texcoords[units[i]].data(), sizeof(v.texcoord[i]));
    }
    return v;
}

void APIENTRY vkfBegin(GLenum mode) { if (state.immediate) fail("nested Begin"); state.immediate = true; state.immediateMode = mode; state.immediateVertices.clear(); }
void APIENTRY vkfEnd() { if (!state.immediate) fail("End without Begin"); state.immediate = false; submit(state.immediateMode, state.immediateVertices); }
void APIENTRY vkfVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (!state.immediate) fail("immediate vertex outside Begin");
    VulkanVertex v = {{x, y, z, w}, {}, {}};
    memcpy(v.color, state.color, sizeof(v.color));
    const int units[] = {0, 3, 4};
    for (int i = 0; i < 3; ++i) memcpy(v.texcoord[i], state.texcoords[units[i]].data(), sizeof(v.texcoord[i]));
    state.immediateVertices.push_back(v);
}
void APIENTRY vkfVertex3f(GLfloat x, GLfloat y, GLfloat z) { vkfVertex4f(x, y, z, 1.f); }
void APIENTRY vkfVertex2f(GLfloat x, GLfloat y) { vkfVertex4f(x, y, 0.f, 1.f); }
void APIENTRY vkfVertex2i(GLint x, GLint y) { vkfVertex2f(float(x), float(y)); }
void APIENTRY vkfVertex3fv(const GLfloat *v) { vkfVertex3f(v[0], v[1], v[2]); }
void APIENTRY vkfVertex2fv(const GLfloat *v) { vkfVertex2f(v[0], v[1]); }
void APIENTRY vkfColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { state.color[0] = r; state.color[1] = g; state.color[2] = b; state.color[3] = a; }
void APIENTRY vkfColor3f(GLfloat r, GLfloat g, GLfloat b) { vkfColor4f(r, g, b, 1.f); }
void APIENTRY vkfColor4fv(const GLfloat *v) { vkfColor4f(v[0], v[1], v[2], v[3]); }
void APIENTRY vkfColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { vkfColor4f(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
void APIENTRY vkfTexCoord2f(GLfloat s, GLfloat t) { state.texcoords[0] = {{s, t, 0.f, 1.f}}; }
void APIENTRY vkfMultiTexCoord2f(GLenum unit, GLfloat s, GLfloat t) { if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 16) fail("multitexture coordinate"); state.texcoords[unit - GL_TEXTURE0] = {{s, t, 0.f, 1.f}}; }
void APIENTRY vkfDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (first < 0 || count < 0) fail("DrawArrays range");
    std::vector<VulkanVertex> vertices; vertices.reserve(count);
    for (int i = 0; i < count; ++i) vertices.push_back(arrayVertex(unsigned(first + i)));
    submit(mode, vertices);
}
void APIENTRY vkfDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (count < 0 || (type != GL_UNSIGNED_INT && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_BYTE)) fail("DrawElements format");
    size_t bytes = scalarSize(type);
    auto p = static_cast<const uint8_t *>(indices);
    if (state.elementBuffer)
    {
        auto const &b = buffers[state.elementBuffer].bytes;
        size_t off = reinterpret_cast<uintptr_t>(indices);
        if (off > b.size() || size_t(count) * bytes > b.size() - off) fail("index buffer bounds");
        p = b.data() + off;
    }
    if (!p && count) fail("null index buffer");
    std::vector<VulkanVertex> vertices; vertices.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        unsigned index = type == GL_UNSIGNED_INT ? readScalar<uint32_t>(p + i * bytes) : type == GL_UNSIGNED_SHORT ? readScalar<uint16_t>(p + i * bytes) : p[i];
        vertices.push_back(arrayVertex(index));
    }
    submit(mode, vertices);
}
void arrayPointer(Array &a, GLint size, GLenum type, GLsizei stride, const void *pointer, bool normalized = false)
{
    a.size = size; a.type = type; a.stride = stride; a.pointer = pointer; a.buffer = state.arrayBuffer; a.normalized = normalized;
}
void APIENTRY vkfVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) { arrayPointer(state.vertex, size, type, stride, pointer); }
void APIENTRY vkfColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) { arrayPointer(state.colorArray, size, type, stride, pointer, true); }
void APIENTRY vkfTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) { arrayPointer(state.texcoordArrays[state.clientTexture], size, type, stride, pointer); }
void APIENTRY vkfVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) { if (index >= 16) fail("vertex attribute index"); arrayPointer(state.attributes[index], size, type, stride, pointer, normalized != 0); }
void clientState(GLenum which, bool enabled)
{
    if (which == GL_VERTEX_ARRAY) state.vertex.enabled = enabled;
    else if (which == GL_TEXTURE_COORD_ARRAY) state.texcoordArrays[state.clientTexture].enabled = enabled;
    else if (which == GL_COLOR_ARRAY) state.colorArray.enabled = enabled;
    else fail("client array kind");
}
void APIENTRY vkfEnableClientState(GLenum which) { clientState(which, true); }
void APIENTRY vkfDisableClientState(GLenum which) { clientState(which, false); }
void APIENTRY vkfEnableVertexAttribArray(GLuint index) { if (index >= 16) fail("attribute index"); state.attributes[index].enabled = true; }
void APIENTRY vkfDisableVertexAttribArray(GLuint index) { if (index >= 16) fail("attribute index"); state.attributes[index].enabled = false; }
void APIENTRY vkfActiveTexture(GLenum unit) { if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 16) fail("active texture unit"); state.activeTexture = unit - GL_TEXTURE0; }
void APIENTRY vkfClientActiveTexture(GLenum unit) { if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 16) fail("client texture unit"); state.clientTexture = unit - GL_TEXTURE0; }

void APIENTRY vkfMatrixMode(GLenum mode) { state.matrixMode = mode; (void)matrixStack(); }
void APIENTRY vkfLoadIdentity() { matrixStack().back() = identity(); }
void APIENTRY vkfLoadMatrixf(const GLfloat *m) { memcpy(matrixStack().back().data(), m, 16 * sizeof(float)); }
void APIENTRY vkfMultMatrixf(const GLfloat *m) { matrixStack().back() = multiply(matrixStack().back(), m); }
void APIENTRY vkfLoadMatrixd(const GLdouble *m) { for (int i = 0; i < 16; ++i) matrixStack().back()[i] = float(m[i]); }
void APIENTRY vkfMultMatrixd(const GLdouble *m) { Matrix f; for (int i = 0; i < 16; ++i) f[i] = float(m[i]); vkfMultMatrixf(f.data()); }
void APIENTRY vkfPushMatrix() { auto &s = matrixStack(); s.push_back(s.back()); }
void APIENTRY vkfPopMatrix() { auto &s = matrixStack(); if (s.size() < 2) fail("matrix stack underflow"); s.pop_back(); }
void APIENTRY vkfTranslatef(GLfloat x, GLfloat y, GLfloat z) { auto m = identity(); m[12] = x; m[13] = y; m[14] = z; vkfMultMatrixf(m.data()); }
void APIENTRY vkfScalef(GLfloat x, GLfloat y, GLfloat z) { auto m = identity(); m[0] = x; m[5] = y; m[10] = z; vkfMultMatrixf(m.data()); }
void APIENTRY vkfRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    float length = std::sqrt(x * x + y * y + z * z);
    if (!length) return;
    x /= length; y /= length; z /= length;
    float a = angle * 0.017453292519943295f, c = std::cos(a), s = std::sin(a), t = 1.f - c;
    Matrix m = {{t*x*x+c, t*x*y+s*z, t*x*z-s*y, 0, t*x*y-s*z, t*y*y+c, t*y*z+s*x, 0, t*x*z+s*y, t*y*z-s*x, t*z*z+c, 0, 0,0,0,1}};
    vkfMultMatrixf(m.data());
}
void APIENTRY vkfOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    if (l == r || b == t || n == f) fail("orthographic projection");
    auto m = identity(); m[0] = float(2/(r-l)); m[5] = float(2/(t-b)); m[10] = float(-2/(f-n));
    m[12] = float(-(r+l)/(r-l)); m[13] = float(-(t+b)/(t-b)); m[14] = float(-(f+n)/(f-n));
    vkfMultMatrixf(m.data());
}
void APIENTRY vkfFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    if (l == r || b == t || n <= 0 || f <= n) fail("perspective projection");
    Matrix m = {}; m[0] = float(2*n/(r-l)); m[5] = float(2*n/(t-b)); m[8] = float((r+l)/(r-l));
    m[9] = float((t+b)/(t-b)); m[10] = float(-(f+n)/(f-n)); m[11] = -1; m[14] = float(-2*f*n/(f-n));
    vkfMultMatrixf(m.data());
}
void APIENTRY vkfViewport(GLint x, GLint y, GLsizei w, GLsizei h) { if (w < 0 || h < 0) fail("viewport"); state.draw.viewport[0] = x; state.draw.viewport[1] = y; state.draw.viewport[2] = w; state.draw.viewport[3] = h; }
void APIENTRY vkfScissor(GLint x, GLint y, GLsizei w, GLsizei h) { if (w < 0 || h < 0) fail("scissor"); state.draw.scissor[0] = x; state.draw.scissor[1] = y; state.draw.scissor[2] = w; state.draw.scissor[3] = h; }
void enable(GLenum cap, bool on)
{
    if (on) state.enables.insert(cap); else state.enables.erase(cap);
    switch (cap)
    {
        case GL_DEPTH_TEST: state.draw.depthTest = on; break;
        case GL_BLEND: state.draw.blend = on; break;
        case GL_CULL_FACE: state.draw.cull = on; break;
        case GL_STENCIL_TEST: state.draw.stencilTest = on; break;
        case GL_SCISSOR_TEST: state.draw.scissorTest = on; break;
        case GL_ALPHA_TEST: state.alphaTest = on; break;
        case GL_FOG: state.fogEnabled = on; break;
        case GL_TEXTURE_2D: state.textureEnabled[state.activeTexture] = on; break;
        case GL_POLYGON_OFFSET_FILL: state.draw.depthBias = on; break;
        // Quality hints do not change the frontend's geometry or materials.
        case GL_LINE_SMOOTH: case GL_POINT_SMOOTH: case GL_DITHER: case GL_MULTISAMPLE: break;
        case GL_DEPTH_CLAMP: state.draw.depthClamp = on; break;
        default: LOG_F(ERROR, "Vulkan frontend capability: 0x%x", cap); fail("render capability");
    }
}
void APIENTRY vkfEnable(GLenum cap) { enable(cap, true); }
void APIENTRY vkfDisable(GLenum cap) { enable(cap, false); }
GLboolean APIENTRY vkfIsEnabled(GLenum cap) { return cap == GL_TEXTURE_2D ? state.textureEnabled[state.activeTexture] : state.enables.count(cap) != 0; }
void APIENTRY vkfDepthFunc(GLenum function) { state.draw.depthFunc = function; }
void APIENTRY vkfDepthMask(GLboolean enabled) { state.draw.depthWrite = enabled != 0; }
void APIENTRY vkfDepthRange(GLdouble nearValue, GLdouble farValue) { state.draw.depthRange[0] = float(std::max(0., std::min(1., nearValue))); state.draw.depthRange[1] = float(std::max(0., std::min(1., farValue))); }
void APIENTRY vkfBlendFunc(GLenum source, GLenum destination) { state.draw.blendSrc = source; state.draw.blendDst = destination; }
void APIENTRY vkfBlendEquation(GLenum equation)
{
    if (equation != GL_FUNC_ADD && equation != GL_FUNC_SUBTRACT && equation != GL_FUNC_REVERSE_SUBTRACT && equation != GL_MIN && equation != GL_MAX) fail("blend equation");
    state.draw.blendEquation = equation;
}
void APIENTRY vkfAlphaFunc(GLenum function, GLclampf reference) { state.alphaFunc = function; state.alphaRef = std::max(0.f, std::min(1.f, reference)); }
void APIENTRY vkfCullFace(GLenum face) { state.draw.cullFace = face; }
void APIENTRY vkfFrontFace(GLenum face) { state.draw.frontFace = face; }
void APIENTRY vkfColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { state.draw.colorWrite[0] = r; state.draw.colorWrite[1] = g; state.draw.colorWrite[2] = b; state.draw.colorWrite[3] = a; }
void APIENTRY vkfPolygonMode(GLenum face, GLenum mode) { if (face != GL_FRONT_AND_BACK) fail("separate polygon modes"); state.draw.polygonMode = mode; }
void APIENTRY vkfPolygonOffset(GLfloat factor, GLfloat units) { state.draw.depthBiasFactor = factor; state.draw.depthBiasUnits = units; }
void APIENTRY vkfLineWidth(GLfloat width) { state.draw.lineWidth = width; }
void APIENTRY vkfStencilFunc(GLenum function, GLint reference, GLuint mask) { state.draw.stencilFunc = function; state.draw.stencilReference = reference; state.draw.stencilCompareMask = mask; }
void APIENTRY vkfStencilOp(GLenum failOp, GLenum depthFail, GLenum pass) { state.draw.stencilFail = failOp; state.draw.stencilDepthFail = depthFail; state.draw.stencilPass = pass; }
void APIENTRY vkfStencilMask(GLuint mask) { state.draw.stencilWriteMask = mask; }
void APIENTRY vkfFogf(GLenum parameter, GLfloat value)
{
    switch (parameter)
    {
        case GL_FOG_START: state.fogStart = value; break;
        case GL_FOG_END: state.fogEnd = value; break;
        case GL_FOG_DENSITY: state.fogDensity = value; break;
        case GL_FOG_MODE: state.fogMode = GLenum(value); break;
        default: fail("fog parameter");
    }
}
void APIENTRY vkfFogi(GLenum p, GLint value) { vkfFogf(p, float(value)); }
void APIENTRY vkfFogfv(GLenum p, const GLfloat *value) { if (p == GL_FOG_COLOR) memcpy(state.fogColor, value, sizeof(state.fogColor)); else vkfFogf(p, *value); }
void copyTextureParameters(Texture &to, const Texture &from)
{
    to.minFilter = from.minFilter; to.magFilter = from.magFilter;
    to.wrapS = from.wrapS; to.wrapT = from.wrapT;
    to.anisotropy = from.anisotropy;
    to.baseLevel = from.baseLevel; to.maxLevel = from.maxLevel;
}
void APIENTRY vkfPushAttrib(GLbitfield mask)
{
    if (mask & ~GL_ALL_ATTRIB_BITS) fail("attribute mask");
    Attributes saved{mask, state, {}};
    if (mask & GL_TEXTURE_BIT)
        for (auto id : state.textures)
        {
            auto it = textures.find(id);
            if (it != textures.end()) copyTextureParameters(saved.textureParameters[id], it->second);
        }
    attributeStack.push_back(std::move(saved));
}
void APIENTRY vkfPopAttrib()
{
    if (attributeStack.empty()) fail("attribute stack underflow");
    auto attributes = std::move(attributeStack.back()); attributeStack.pop_back();
    auto const &saved = attributes.state;
    GLbitfield const mask = attributes.mask;
    auto const &s = saved.draw;
    auto &d = state.draw;
    auto restoreEnable = [&](GLenum cap) { enable(cap, saved.enables.count(cap) != 0); };
    if (mask & GL_CURRENT_BIT)
    {
        memcpy(state.color, saved.color, sizeof(state.color));
        state.texcoords = saved.texcoords;
    }
    if (mask & GL_COLOR_BUFFER_BIT)
    {
        restoreEnable(GL_ALPHA_TEST); restoreEnable(GL_BLEND); restoreEnable(GL_DITHER);
        state.alphaFunc = saved.alphaFunc; state.alphaRef = saved.alphaRef;
        d.blendSrc = s.blendSrc; d.blendDst = s.blendDst; d.blendEquation = s.blendEquation;
        memcpy(d.colorWrite, s.colorWrite, sizeof(d.colorWrite));
        memcpy(state.clearColor, saved.clearColor, sizeof(state.clearColor));
    }
    if (mask & GL_DEPTH_BUFFER_BIT)
    {
        restoreEnable(GL_DEPTH_TEST);
        d.depthFunc = s.depthFunc; d.depthWrite = s.depthWrite; state.clearDepth = saved.clearDepth;
    }
    if (mask & GL_STENCIL_BUFFER_BIT)
    {
        restoreEnable(GL_STENCIL_TEST);
        d.stencilFunc = s.stencilFunc; d.stencilReference = s.stencilReference;
        d.stencilCompareMask = s.stencilCompareMask; d.stencilWriteMask = s.stencilWriteMask;
        d.stencilFail = s.stencilFail; d.stencilDepthFail = s.stencilDepthFail; d.stencilPass = s.stencilPass;
        state.clearStencil = saved.clearStencil;
    }
    if (mask & GL_FOG_BIT)
    {
        restoreEnable(GL_FOG);
        state.fogMode = saved.fogMode; state.fogStart = saved.fogStart;
        state.fogEnd = saved.fogEnd; state.fogDensity = saved.fogDensity;
        memcpy(state.fogColor, saved.fogColor, sizeof(state.fogColor));
    }
    if (mask & GL_LINE_BIT) { restoreEnable(GL_LINE_SMOOTH); d.lineWidth = s.lineWidth; }
    if (mask & GL_POINT_BIT) restoreEnable(GL_POINT_SMOOTH);
    if (mask & GL_POLYGON_BIT)
    {
        restoreEnable(GL_CULL_FACE); restoreEnable(GL_POLYGON_OFFSET_FILL);
        d.cullFace = s.cullFace; d.frontFace = s.frontFace; d.polygonMode = s.polygonMode;
        d.depthBiasFactor = s.depthBiasFactor; d.depthBiasUnits = s.depthBiasUnits;
    }
    if (mask & GL_SCISSOR_BIT)
    {
        restoreEnable(GL_SCISSOR_TEST); memcpy(d.scissor, s.scissor, sizeof(d.scissor));
    }
    if (mask & GL_VIEWPORT_BIT)
    {
        memcpy(d.viewport, s.viewport, sizeof(d.viewport)); memcpy(d.depthRange, s.depthRange, sizeof(d.depthRange));
    }
    if (mask & GL_TRANSFORM_BIT)
    {
        state.matrixMode = saved.matrixMode; restoreEnable(GL_DEPTH_CLAMP);
    }
    if (mask & GL_ACCUM_BUFFER_BIT) memcpy(state.clearAccum, saved.clearAccum, sizeof(state.clearAccum));
    if (mask & GL_TEXTURE_BIT)
    {
        state.activeTexture = saved.activeTexture; state.textures = saved.textures;
        state.textureEnabled = saved.textureEnabled;
        state.texEnvMode = saved.texEnvMode; state.texEnvScale = saved.texEnvScale;
        for (auto const &entry : attributes.textureParameters)
            if (textures.count(entry.first)) copyTextureParameters(textures[entry.first], entry.second);
    }
    if (mask & GL_ENABLE_BIT)
    {
        auto capabilities = state.enables;
        capabilities.insert(saved.enables.begin(), saved.enables.end());
        for (auto cap : capabilities) if (cap != GL_TEXTURE_2D) restoreEnable(cap);
        state.textureEnabled = saved.textureEnabled;
    }
    // Client pixel storage/arrays/active texture, matrix stacks, shader bindings
    // and uniforms are deliberately untouched by server attribute restoration.
    // The shared frontend caches wrapper calls; invalidate their hash entries so
    // a later call cannot skip restoring state on the basis of a pre-pop value.
    for (auto &table : gl.state)
    {
        inthash_free(&table); table.count = 64; inthash_init(&table);
    }
    gl.currentActiveTexture = GL_TEXTURE0 + state.activeTexture;
    gl.x = d.viewport[0]; gl.y = d.viewport[1]; gl.width = d.viewport[2]; gl.height = d.viewport[3];
    gl.fullReset = 1;
}
void APIENTRY vkfHint(GLenum, GLenum) {} // Hints are explicitly advisory.
void APIENTRY vkfShadeModel(GLenum model) { if (model != GL_SMOOTH) fail("flat vertex-color shading"); }
void APIENTRY vkfClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { state.clearColor[0] = r; state.clearColor[1] = g; state.clearColor[2] = b; state.clearColor[3] = a; }
void APIENTRY vkfClearDepth(GLclampd depth) { state.clearDepth = float(depth); }
void APIENTRY vkfClearStencil(GLint stencil) { state.clearStencil = uint32_t(stencil); }
void APIENTRY vkfClearAccum(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { state.clearAccum[0] = r; state.clearAccum[1] = g; state.clearAccum[2] = b; state.clearAccum[3] = a; }
template<typename Function> void accumulationComponents(Function function)
{
    int left = 0, bottom = 0, right = state.width, top = state.height;
    if (state.draw.scissorTest)
    {
        left = std::max(left, state.draw.scissor[0]); bottom = std::max(bottom, state.draw.scissor[1]);
        right = std::min(right, state.draw.scissor[0] + state.draw.scissor[2]);
        top = std::min(top, state.draw.scissor[1] + state.draw.scissor[3]);
    }
    for (int y = bottom; y < top; ++y)
        for (int x = left; x < right; ++x)
            for (int c = 0; c < 4; ++c) function((size_t(y) * state.width + x) * 4 + c);
}
void APIENTRY vkfClear(GLbitfield mask)
{
    if (mask & GL_ACCUM_BUFFER_BIT)
    {
        accumulation.resize(size_t(state.width) * state.height * 4);
        accumulationComponents([](size_t i) { accumulation[i] = state.clearAccum[i & 3]; });
        mask &= ~GL_ACCUM_BUFFER_BIT;
    }
    if (mask) vulkanClear(mask, state.clearColor, state.clearDepth, state.clearStencil, snapshot());
}
void APIENTRY vkfFinish() { vulkanFinish(); }
void APIENTRY vkfFlush() { vulkanFlush(); }

GLuint &boundBuffer(GLenum target)
{
    if (target == GL_ARRAY_BUFFER) return state.arrayBuffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return state.elementBuffer;
    fail("buffer target");
}
void APIENTRY vkfGenBuffers(GLsizei count, GLuint *out) { if (count < 0) fail("GenBuffers count"); for (int i = 0; i < count; ++i) { out[i] = nextBuffer++; buffers.emplace(out[i], Buffer{}); } }
void APIENTRY vkfBindBuffer(GLenum target, GLuint id) { boundBuffer(target) = id; if (id) buffers[id].bound = true; }
GLboolean APIENTRY vkfIsBuffer(GLuint id) { auto it = buffers.find(id); return it != buffers.end() && it->second.bound; }
void APIENTRY vkfDeleteBuffers(GLsizei count, const GLuint *ids)
{
    for (int i = 0; i < count; ++i)
    {
        buffers.erase(ids[i]);
        if (state.arrayBuffer == ids[i]) state.arrayBuffer = 0;
        if (state.elementBuffer == ids[i]) state.elementBuffer = 0;
    }
}
void APIENTRY vkfBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum)
{
    if (size < 0 || !boundBuffer(target)) fail("BufferData allocation");
    auto &b = buffers[boundBuffer(target)].bytes; b.resize(size_t(size));
    if (data && size) memcpy(b.data(), data, size_t(size));
}
void APIENTRY vkfBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    if (!boundBuffer(target) || offset < 0 || size < 0 || (!data && size)) fail("BufferSubData arguments");
    auto &b = buffers[boundBuffer(target)].bytes;
    if (size_t(offset) > b.size() || size_t(size) > b.size() - size_t(offset)) fail("BufferSubData bounds");
    if (size) memcpy(b.data() + offset, data, size_t(size));
}
void APIENTRY vkfBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield) { vkfBufferData(target, size, data, GL_STATIC_DRAW); }
void *APIENTRY vkfMapBuffer(GLenum target, GLenum) { if (!boundBuffer(target)) fail("MapBuffer binding"); return buffers[boundBuffer(target)].bytes.data(); }
void *APIENTRY vkfMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield)
{
    if (!boundBuffer(target) || offset < 0 || length < 0) fail("MapBufferRange arguments");
    auto &b = buffers[boundBuffer(target)].bytes;
    if (size_t(offset) > b.size() || size_t(length) > b.size() - size_t(offset)) fail("MapBufferRange bounds");
    return b.data() + offset;
}
GLboolean APIENTRY vkfUnmapBuffer(GLenum target) { if (!boundBuffer(target)) fail("UnmapBuffer binding"); return GL_TRUE; }
GLsync APIENTRY vkfFenceSync(GLenum, GLbitfield) { auto fence = reinterpret_cast<GLsync>(new uint8_t); fences.insert(fence); return fence; }
void APIENTRY vkfDeleteSync(GLsync fence) { if (fences.erase(fence)) delete reinterpret_cast<uint8_t *>(fence); }
GLboolean APIENTRY vkfIsSync(GLsync fence) { return fences.count(fence) != 0; }
GLenum APIENTRY vkfClientWaitSync(GLsync fence, GLbitfield, GLuint64) { if (!fences.count(fence)) fail("sync object"); vulkanFinish(); return GL_ALREADY_SIGNALED; }

size_t pixelComponents(GLenum format)
{
    switch (format)
    {
        case GL_RED: case GL_ALPHA: case GL_LUMINANCE: return 1;
        case GL_RG: case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: case GL_BGR: return 3;
        case GL_RGBA: case GL_BGRA: return 4;
        default: fail("pixel format");
    }
}
size_t pixelBytes(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_BYTE) return pixelComponents(format);
    if (type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT_8_8_8_8_REV) return 4;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_5_6_5_REV
        || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_4_4_4_4_REV
        || type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_1_5_5_5_REV) return 2;
    fail("pixel element type");
}
size_t pixelStride(const PixelStore &store, int width, size_t bytes)
{
    size_t row = size_t(store.rowLength ? store.rowLength : width) * bytes;
    return (row + store.alignment - 1) / store.alignment * store.alignment;
}
void decodePixel(const uint8_t *in, GLenum format, GLenum type, uint8_t *out)
{
    uint8_t c[4] = {0, 0, 0, 255};
    size_t const count = pixelComponents(format);
    if (type == GL_UNSIGNED_BYTE) memcpy(c, in, count);
    else if (type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT_8_8_8_8_REV)
    {
        uint32_t value = readScalar<uint32_t>(in);
        for (unsigned i = 0; i < 4; ++i) c[i] = uint8_t(value >> (8 * (type == GL_UNSIGNED_INT_8_8_8_8_REV ? i : 3 - i)));
    }
    else
    {
        uint16_t value = readScalar<uint16_t>(in);
        int widths[4] = {5, 6, 5, 0};
        bool reverse = type == GL_UNSIGNED_SHORT_5_6_5_REV || type == GL_UNSIGNED_SHORT_4_4_4_4_REV || type == GL_UNSIGNED_SHORT_1_5_5_5_REV;
        if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_4_4_4_4_REV) for (int &w : widths) w = 4;
        else if (type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_1_5_5_5_REV) { widths[0] = widths[1] = widths[2] = 5; widths[3] = 1; }
        int position = reverse ? 0 : 16;
        for (size_t i = 0; i < count; ++i)
        {
            if (!widths[i]) continue;
            if (!reverse) position -= widths[i];
            unsigned mask = (1u << widths[i]) - 1;
            c[i] = uint8_t((((value >> position) & mask) * 255u + mask / 2) / mask);
            if (reverse) position += widths[i];
        }
    }
    if (format == GL_BGR || format == GL_BGRA) std::swap(c[0], c[2]);
    if (format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA)
    {
        uint8_t alpha = format == GL_LUMINANCE_ALPHA ? c[1] : 255;
        c[1] = c[2] = c[0]; c[3] = alpha;
    }
    else if (format == GL_ALPHA) { c[3] = c[0]; c[0] = c[1] = c[2] = 255; }
    memcpy(out, c, 4);
}
std::vector<uint8_t> decodePixels(int width, int height, GLenum format, GLenum type, const void *data)
{
    if (width < 0 || height < 0) fail("texture dimensions");
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 0);
    if (!data) return pixels;
    size_t bytes = pixelBytes(format, type), stride = pixelStride(state.unpack, width, bytes);
    auto p = static_cast<const uint8_t *>(data) + state.unpack.skipRows * stride + state.unpack.skipPixels * bytes;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            decodePixel(p + y * stride + x * bytes, format, type, pixels.data() + (size_t(y) * width + x) * 4);
    return pixels;
}
Texture &boundTexture(GLenum target)
{
    if (target != GL_TEXTURE_2D) fail("texture target");
    GLuint id = state.textures[state.activeTexture];
    return textures[id]; // Name zero has default-object parameter state, as in the shared frontend.
}
void APIENTRY vkfGenTextures(GLsizei count, GLuint *out) { if (count < 0) fail("GenTextures count"); for (int i = 0; i < count; ++i) { out[i] = nextTexture++; textures.emplace(out[i], Texture{}); } }
void APIENTRY vkfBindTexture(GLenum target, GLuint id) { if (target != GL_TEXTURE_2D) fail("texture binding target"); state.textures[state.activeTexture] = id; if (id) textures[id].bound = true; }
GLboolean APIENTRY vkfIsTexture(GLuint id) { auto it = textures.find(id); return it != textures.end() && it->second.bound; }
void APIENTRY vkfDeleteTextures(GLsizei count, const GLuint *ids)
{
    for (int i = 0; i < count; ++i)
    {
        if (!ids[i]) continue;
        if (textures.erase(ids[i])) vulkanDeleteTexture(ids[i]);
        for (auto &bound : state.textures) if (bound == ids[i]) bound = 0;
    }
}
void APIENTRY vkfTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data)
{
    if (level < 0 || level > 30 || border != 0 || width <= 0 || height <= 0) fail("TexImage2D dimensions");
    auto &t = boundTexture(target);
    auto &image = t.levels[level]; image.width = width; image.height = height;
    image.rgba = decodePixels(width, height, format, type, data);
    if (internalFormat == GL_RGB || internalFormat == GL_RGB8 || internalFormat == 3)
        for (size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    vulkanUploadTexture(state.textures[state.activeTexture], level, width, height, image.rgba.data());
}
void APIENTRY vkfTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *data)
{
    auto &t = boundTexture(target);
    auto it = t.levels.find(level);
    if (it == t.levels.end() || x < 0 || y < 0 || width < 0 || height < 0 || x > it->second.width - width || y > it->second.height - height)
    {
        LOG_F(ERROR, "Vulkan texture %u, unit %u, mip %d: subimage (%d,%d) %dx%d exceeds image %dx%d (format 0x%x, type 0x%x)",
              state.textures[state.activeTexture], state.activeTexture, level, x, y, width, height,
              it == t.levels.end() ? 0 : it->second.width, it == t.levels.end() ? 0 : it->second.height, format, type);
        fail("TexSubImage2D bounds");
    }
    auto pixels = decodePixels(width, height, format, type, data);
    auto &im = it->second;
    for (int row = 0; row < height; ++row)
        memcpy(im.rgba.data() + (size_t(y + row) * im.width + x) * 4, pixels.data() + size_t(row) * width * 4, size_t(width) * 4);
    if (width && height) vulkanUploadTextureRegion(state.textures[state.activeTexture], level, x, y, width, height, pixels.data());
}
void APIENTRY vkfTexParameterf(GLenum target, GLenum parameter, GLfloat value)
{
    auto &t = boundTexture(target);
    switch (parameter)
    {
        case GL_TEXTURE_MIN_FILTER: t.minFilter = GLenum(value); break;
        case GL_TEXTURE_MAG_FILTER: t.magFilter = GLenum(value); break;
        case GL_TEXTURE_WRAP_S: t.wrapS = GLenum(value); break;
        case GL_TEXTURE_WRAP_T: t.wrapT = GLenum(value); break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT: t.anisotropy = std::max(1.f, std::min(value, vulkanMaxAnisotropy())); break;
        case GL_TEXTURE_BASE_LEVEL: t.baseLevel = int(value); break;
        case GL_TEXTURE_MAX_LEVEL: t.maxLevel = int(value); break;
        case GL_TEXTURE_PRIORITY: break; // Advisory residency hint.
        default: LOG_F(ERROR, "Vulkan frontend texture parameter: 0x%x", parameter); fail("texture parameter");
    }
}
void APIENTRY vkfTexParameteri(GLenum target, GLenum parameter, GLint value) { vkfTexParameterf(target, parameter, float(value)); }
void APIENTRY vkfTexEnvf(GLenum target, GLenum parameter, GLfloat value)
{
    if (target != GL_TEXTURE_ENV) fail("texture environment target");
    if (parameter == GL_TEXTURE_ENV_MODE)
    {
        if (GLenum(value) != GL_MODULATE && GLenum(value) != GL_REPLACE) fail("texture environment mode");
        state.texEnvMode[state.activeTexture] = GLenum(value);
    }
    else if (parameter == GL_RGB_SCALE)
    {
        // Shared Polymost uses shader uniforms for detail-map modulation and
        // resets this legacy state to one after the model/detail paths.
        if (value != 1.f) fail("fixed-function texture RGB scale");
        state.texEnvScale[state.activeTexture] = value;
    }
    else fail("texture environment parameter");
}
void APIENTRY vkfTexEnvi(GLenum target, GLenum parameter, GLint value) { vkfTexEnvf(target, parameter, float(value)); }
void APIENTRY vkfPixelStorei(GLenum parameter, GLint value)
{
    if (value < 0) fail("negative pixel storage parameter");
    switch (parameter)
    {
        case GL_PACK_ALIGNMENT: case GL_UNPACK_ALIGNMENT:
            if (value != 1 && value != 2 && value != 4 && value != 8) fail("pixel alignment");
            (parameter == GL_PACK_ALIGNMENT ? state.pack : state.unpack).alignment = value; break;
        case GL_PACK_ROW_LENGTH: state.pack.rowLength = value; break;
        case GL_UNPACK_ROW_LENGTH: state.unpack.rowLength = value; break;
        case GL_PACK_SKIP_ROWS: state.pack.skipRows = value; break;
        case GL_UNPACK_SKIP_ROWS: state.unpack.skipRows = value; break;
        case GL_PACK_SKIP_PIXELS: state.pack.skipPixels = value; break;
        case GL_UNPACK_SKIP_PIXELS: state.unpack.skipPixels = value; break;
        case GL_PACK_SWAP_BYTES: case GL_UNPACK_SWAP_BYTES: case GL_PACK_LSB_FIRST: case GL_UNPACK_LSB_FIRST:
            if (value) fail("byte-swapped pixel storage");
            break;
        default: fail("pixel storage parameter");
    }
}
void APIENTRY vkfGenerateMipmap(GLenum target)
{
    auto &texture = boundTexture(target);
    int level = texture.baseLevel;
    auto it = texture.levels.find(level);
    if (it == texture.levels.end()) fail("mipmap base image");
    Image previous = it->second;
    while ((previous.width > 1 || previous.height > 1) && level < texture.maxLevel)
    {
        Image next; next.width = std::max(1, previous.width / 2); next.height = std::max(1, previous.height / 2);
        next.rgba.resize(size_t(next.width) * next.height * 4);
        for (int y = 0; y < next.height; ++y) for (int x = 0; x < next.width; ++x) for (int c = 0; c < 4; ++c)
        {
            unsigned value = 0, count = 0;
            for (int dy = 0; dy < 2 && y*2+dy < previous.height; ++dy)
                for (int dx = 0; dx < 2 && x*2+dx < previous.width; ++dx) { value += previous.rgba[(size_t(y*2+dy)*previous.width+x*2+dx)*4+c]; ++count; }
            next.rgba[(size_t(y)*next.width+x)*4+c] = uint8_t((value+count/2)/count);
        }
        ++level;
        vulkanUploadTexture(state.textures[state.activeTexture], level, next.width, next.height, next.rgba.data());
        texture.levels[level] = next; previous = std::move(next);
    }
}

const char *const uniformNames[] = {
    "s_texture", "s_palswap", "s_palette", "s_detail", "s_glow",
    "u_texturePosSize", "u_halfTexelSize", "u_palswapPos", "u_palswapSize", "u_clamp",
    "u_shade", "u_numShades", "u_visFactor", "u_fogEnabled", "u_useColorOnly", "u_usePalette",
    "u_useDetailMapping", "u_useGlowMapping", "u_npotEmulation", "u_brightness", "u_rotMatrix",
    "u_shadeInterpolate", "u_colorCorrection", "tex"
};
GLuint APIENTRY vkfCreateShader(GLenum type) { GLuint id = nextObject++; shaders[id].type = type; return id; }
void APIENTRY vkfShaderSource(GLuint id, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    auto it = shaders.find(id); if (it == shaders.end() || count < 0) fail("ShaderSource object");
    auto &s = it->second.source; s.clear();
    for (int i = 0; i < count; ++i) if (strings[i]) s.append(strings[i], lengths && lengths[i] >= 0 ? size_t(lengths[i]) : strlen(strings[i]));
}
void APIENTRY vkfCompileShader(GLuint id) { auto it = shaders.find(id); if (it == shaders.end()) fail("CompileShader object"); it->second.compiled = !it->second.source.empty(); }
void APIENTRY vkfGetShaderiv(GLuint id, GLenum parameter, GLint *out)
{
    auto it = shaders.find(id); if (it == shaders.end()) fail("shader query object");
    switch (parameter)
    {
        case GL_COMPILE_STATUS: *out = it->second.compiled; break;
        case GL_SHADER_TYPE: *out = it->second.type; break;
        case GL_INFO_LOG_LENGTH: *out = 1; break;
        case GL_SHADER_SOURCE_LENGTH: *out = GLint(it->second.source.size() + 1); break;
        case GL_DELETE_STATUS: *out = GL_FALSE; break;
        default: fail("shader query parameter");
    }
}
void APIENTRY vkfGetShaderInfoLog(GLuint, GLsizei size, GLsizei *length, GLchar *out) { if (length) *length = 0; if (out && size > 0) *out = 0; }
void APIENTRY vkfDeleteShader(GLuint id) { shaders.erase(id); }
GLuint APIENTRY vkfCreateProgram() { GLuint id = nextObject++; auto &p = programs[id]; materialDefaults(p.uniforms); return id; }
void APIENTRY vkfAttachShader(GLuint program, GLuint shader) { if (!programs.count(program) || !shaders.count(shader)) fail("shader attachment"); programs[program].shaders.push_back(shader); }
void APIENTRY vkfDetachShader(GLuint program, GLuint shader)
{
    auto it = programs.find(program); if (it == programs.end()) fail("shader detachment");
    auto &s = it->second.shaders; s.erase(std::remove(s.begin(), s.end(), shader), s.end());
}
void APIENTRY vkfLinkProgram(GLuint id)
{
    auto it = programs.find(id); if (it == programs.end()) fail("LinkProgram object");
    std::string fragment;
    for (auto shader : it->second.shaders)
    {
        auto s = shaders.find(shader); if (s == shaders.end() || !s->second.compiled) fail("uncompiled shader");
        if (s->second.type == GL_FRAGMENT_SHADER) fragment += s->second.source;
    }
    if (fragment.find("s_palswap") != std::string::npos && fragment.find("u_npotEmulation") != std::string::npos) it->second.kind = 1;
    else if (fragment.find("1.1643") != std::string::npos && fragment.find("2.017") != std::string::npos) it->second.kind = 2;
    else if (fragment.find("c_paletteScale") != std::string::npos && fragment.find("s_palette") != std::string::npos) it->second.kind = 3;
    else fail("shader program (only the engine's Polymost, paletted-surface and VPX shaders are supported)");
    it->second.linked = true;
}
void APIENTRY vkfUseProgram(GLuint id) { if (id && (!programs.count(id) || !programs[id].linked)) fail("UseProgram object"); state.program = id; }
GLboolean APIENTRY vkfIsProgram(GLuint id) { return programs.count(id) != 0; }
void APIENTRY vkfDeleteProgram(GLuint id) { programs.erase(id); if (state.program == id) state.program = 0; }
void APIENTRY vkfGetProgramiv(GLuint id, GLenum parameter, GLint *out)
{
    auto it = programs.find(id); if (it == programs.end()) fail("program query object");
    switch (parameter)
    {
        case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *out = it->second.linked; break;
        case GL_INFO_LOG_LENGTH: *out = 1; break;
        case GL_ATTACHED_SHADERS: *out = GLint(it->second.shaders.size()); break;
        case GL_DELETE_STATUS: *out = GL_FALSE; break;
        default: fail("program query parameter");
    }
}
void APIENTRY vkfGetProgramInfoLog(GLuint id, GLsizei size, GLsizei *length, GLchar *out) { vkfGetShaderInfoLog(id, size, length, out); }
void APIENTRY vkfBindAttribLocation(GLuint, GLuint index, const GLchar *name)
{
    if ((!strcmp(name, "i_vertPos") && index == 0) || (!strcmp(name, "i_texCoord") && index == 1)) return;
    fail("shader attribute binding");
}
GLint APIENTRY vkfGetAttribLocation(GLuint, const GLchar *name) { if (!strcmp(name, "i_vertPos")) return 0; if (!strcmp(name, "i_texCoord")) return 1; return -1; }
GLint APIENTRY vkfGetUniformLocation(GLuint id, const GLchar *name)
{
    if (!programs.count(id)) fail("uniform program object");
    for (unsigned i = 0; i < sizeof(uniformNames) / sizeof(uniformNames[0]); ++i) if (!strcmp(name, uniformNames[i])) return GLint(i);
    return -1; // A queried uniform absent from a recognized built-in program.
}
void uniform(GLint location, unsigned count, const float *values)
{
    if (location == -1) return;
    if (!state.program || !programs.count(state.program) || location < 0 || location >= int(sizeof(uniformNames)/sizeof(uniformNames[0]))) fail("uniform update");
    auto &p = programs[state.program]; auto &u = p.uniforms;
    if (location < 5 || location == 23)
    {
        if (count != 1) fail("sampler uniform size");
        p.samplerUnits[location == 23 ? 0 : location] = int(values[0]); return;
    }
    float *out = nullptr; unsigned expected = 0;
    switch (location)
    {
        case 5: out = u.texturePosSize; expected = 4; break;
        case 6: out = u.halfTexelPalswapPos; expected = 2; break;
        case 7: out = u.halfTexelPalswapPos + 2; expected = 2; break;
        case 8: out = u.palswapSizeClamp; expected = 2; break;
        case 9: out = u.palswapSizeClamp + 2; expected = 2; break;
        case 10: out = u.shadeNumVis; expected = 1; break;
        case 11: out = u.shadeNumVis + 1; expected = 2; break;
        case 12: out = u.shadeNumVis + 3; expected = 1; break;
        case 13: out = u.fog; expected = 1; break;
        case 14: out = u.effects; expected = 1; break;
        case 15: out = u.effects + 1; expected = 1; break;
        case 16: out = u.effects + 2; expected = 1; break;
        case 17: out = u.effects + 3; expected = 1; break;
        case 18: out = u.npot; expected = 4; break;
        case 19: out = u.misc; expected = 1; break;
        case 20: out = u.rotation; expected = 16; break;
        case 21: out = u.misc + 1; expected = 1; break;
        case 22: out = u.correction; expected = 4; break;
        default: fail("uniform location");
    }
    if (count != expected) fail("uniform size");
    memcpy(out, values, count * sizeof(float));
}
void APIENTRY vkfUniform1f(GLint loc, GLfloat x) { uniform(loc, 1, &x); }
void APIENTRY vkfUniform1i(GLint loc, GLint x) { float v = float(x); uniform(loc, 1, &v); }
void APIENTRY vkfUniform2f(GLint loc, GLfloat x, GLfloat y) { float v[] = {x,y}; uniform(loc, 2, v); }
void APIENTRY vkfUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { float v[] = {x,y,z,w}; uniform(loc, 4, v); }
void APIENTRY vkfUniform2fv(GLint loc, GLsizei count, const GLfloat *v) { if (count != 1) fail("uniform vec2 array"); uniform(loc, 2, v); }
void APIENTRY vkfUniform4fv(GLint loc, GLsizei count, const GLfloat *v) { if (count != 1) fail("uniform vec4 array"); uniform(loc, 4, v); }
void APIENTRY vkfUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v)
{
    if (count != 1) fail("uniform matrix array");
    if (!transpose) uniform(loc, 16, v);
    else { Matrix m; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) m[i*4+j] = v[j*4+i]; uniform(loc, 16, m.data()); }
}

void APIENTRY vkfReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *out)
{
    if (width < 0 || height < 0 || type != GL_UNSIGNED_BYTE || !out) fail("ReadPixels arguments");
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    if (!vulkanReadPixels(x, y, width, height, rgba.data())) fail("framebuffer readback");
    size_t bytes = pixelComponents(format), stride = pixelStride(state.pack, width, bytes);
    auto *dest = static_cast<uint8_t *>(out) + state.pack.skipRows * stride + state.pack.skipPixels * bytes;
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col)
    {
        auto const *p = rgba.data() + (size_t(row) * width + col) * 4;
        uint8_t value[4] = {p[0],p[1],p[2],p[3]};
        if (format == GL_BGRA || format == GL_BGR) std::swap(value[0], value[2]);
        if (format == GL_ALPHA) value[0] = p[3];
        if (format == GL_LUMINANCE_ALPHA) value[1] = p[3];
        memcpy(dest + row * stride + col * bytes, value, bytes);
    }
}
void APIENTRY vkfAccum(GLenum operation, GLfloat value)
{
    size_t const size = size_t(state.width) * state.height * 4;
    if (accumulation.size() != size) accumulation.assign(size, 0.f);
    if (operation == GL_LOAD || operation == GL_ACCUM)
    {
        std::vector<uint8_t> rgba(size);
        if (!vulkanReadPixels(0, 0, state.width, state.height, rgba.data())) fail("accumulation readback");
        accumulationComponents([&](size_t i)
        {
            float f = rgba[i] * (value / 255.f);
            accumulation[i] = operation == GL_LOAD ? f : accumulation[i] + f;
        });
    }
    else if (operation == GL_MULT || operation == GL_ADD)
        accumulationComponents([&](size_t i) { accumulation[i] = operation == GL_MULT ? accumulation[i] * value : accumulation[i] + value; });
    else if (operation == GL_RETURN)
    {
        std::vector<uint8_t> rgba(size);
        for (size_t i = 0; i < size; ++i) rgba[i] = uint8_t(std::max(0.f, std::min(255.f, accumulation[i] * value * 255.f)) + 0.5f);
        if (!accumulationTexture) accumulationTexture = nextTexture++;
        vulkanUploadTexture(accumulationTexture, 0, state.width, state.height, rgba.data());
        auto draw = snapshot();
        materialDefaults(draw.uniforms);
        auto m = identity(); memcpy(draw.uniforms.modelView, m.data(), sizeof(draw.uniforms.modelView)); memcpy(draw.uniforms.projection, m.data(), sizeof(draw.uniforms.projection));
        for (auto &matrix : draw.uniforms.textureMatrix) memcpy(matrix, m.data(), sizeof(matrix));
        draw.uniforms.fixed[0] = 1.f;
        draw.depthTest = draw.depthWrite = draw.blend = draw.cull = draw.stencilTest = draw.depthBias = draw.depthClamp = false;
        draw.topology = GL_TRIANGLES; draw.polygonMode = GL_FILL;
        draw.viewport[0] = draw.viewport[1] = 0; draw.viewport[2] = state.width; draw.viewport[3] = state.height;
        draw.samplers[0].texture = accumulationTexture;
        draw.samplers[0].minFilter = draw.samplers[0].magFilter = GL_NEAREST;
        draw.samplers[0].wrapS = draw.samplers[0].wrapT = GL_CLAMP_TO_EDGE;
        draw.samplers[0].anisotropy = 1.f;
        draw.samplers[0].minLod = draw.samplers[0].maxLod = 0.f;
        VulkanVertex corners[4] = {};
        float const positions[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
        for (int i = 0; i < 4; ++i)
        {
            corners[i].position[0] = positions[i][0]; corners[i].position[1] = positions[i][1]; corners[i].position[3] = 1.f;
            for (float &c : corners[i].color) c = 1.f;
            corners[i].texcoord[0][0] = (positions[i][0]+1.f)*0.5f; corners[i].texcoord[0][1] = (positions[i][1]+1.f)*0.5f; corners[i].texcoord[0][3] = 1.f;
        }
        VulkanVertex vertices[] = {corners[0],corners[1],corners[2],corners[0],corners[2],corners[3]};
        vulkanDraw(vertices, 6, draw);
    }
    else fail("accumulation operation");
}

GLenum APIENTRY vkfGetError() { return GL_NO_ERROR; } // Invalid commands fail with their operation name.
const GLubyte *APIENTRY vkfGetString(GLenum which)
{
    switch (which)
    {
        case GL_VENDOR: return reinterpret_cast<const GLubyte *>("NBlood native Vulkan");
        case GL_RENDERER: return reinterpret_cast<const GLubyte *>(vulkanDeviceName());
        case GL_VERSION: return reinterpret_cast<const GLubyte *>("2.1 NBlood native Vulkan frontend");
        case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte *>("1.20 built-in shaders translated to SPIR-V");
        case GL_EXTENSIONS: return reinterpret_cast<const GLubyte *>("GL_ARB_multitexture GL_ARB_vertex_buffer_object GL_ARB_texture_non_power_of_two GL_EXT_texture_filter_anisotropic");
        default: fail("string query");
    }
}
void APIENTRY vkfGetIntegerv(GLenum which, GLint *out)
{
    switch (which)
    {
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *out = vulkanMaxTextureSize(); break;
        case GL_MAX_TEXTURE_UNITS: case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_ATTRIBS: *out = 16; break;
        case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: *out = 1; break;
        case GL_ACTIVE_TEXTURE: *out = GL_TEXTURE0 + state.activeTexture; break;
        case GL_CLIENT_ACTIVE_TEXTURE: *out = GL_TEXTURE0 + state.clientTexture; break;
        case GL_ARRAY_BUFFER_BINDING: *out = state.arrayBuffer; break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: *out = state.elementBuffer; break;
        case GL_TEXTURE_BINDING_2D: *out = state.textures[state.activeTexture]; break;
        case GL_CURRENT_PROGRAM: *out = state.program; break;
        case GL_MATRIX_MODE: *out = state.matrixMode; break;
        case GL_VIEWPORT: memcpy(out, state.draw.viewport, sizeof(state.draw.viewport)); break;
        case GL_SCISSOR_BOX: memcpy(out, state.draw.scissor, sizeof(state.draw.scissor)); break;
        case GL_MAJOR_VERSION: *out = 2; break;
        case GL_MINOR_VERSION: *out = 1; break;
        case GL_NUM_EXTENSIONS: case GL_NUM_COMPRESSED_TEXTURE_FORMATS: *out = 0; break;
        case 0x9126: *out = 2; break; // GL_CONTEXT_PROFILE_MASK / compatibility bit.
        case GL_BLEND_SRC: case GL_BLEND_SRC_RGB: case GL_BLEND_SRC_ALPHA: *out = state.draw.blendSrc; break;
        case GL_BLEND_DST: case GL_BLEND_DST_RGB: case GL_BLEND_DST_ALPHA: *out = state.draw.blendDst; break;
        case GL_DEPTH_FUNC: *out = state.draw.depthFunc; break;
        case GL_FRONT_FACE: *out = state.draw.frontFace; break;
        case GL_CULL_FACE_MODE: *out = state.draw.cullFace; break;
        case GL_POLYGON_MODE: out[0] = out[1] = state.draw.polygonMode; break;
        case GL_PACK_ALIGNMENT: *out = state.pack.alignment; break;
        case GL_UNPACK_ALIGNMENT: *out = state.unpack.alignment; break;
        case GL_PACK_ROW_LENGTH: *out = state.pack.rowLength; break;
        case GL_UNPACK_ROW_LENGTH: *out = state.unpack.rowLength; break;
        case GL_DEPTH_BITS: *out = 24; break;
        case GL_STENCIL_BITS: *out = 8; break;
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: *out = 8; break;
        default: LOG_F(ERROR, "Vulkan frontend: query enum 0x%x", which); fail("integer state query");
    }
}
void APIENTRY vkfGetFloatv(GLenum which, GLfloat *out)
{
    switch (which)
    {
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: *out = vulkanMaxAnisotropy(); break;
        case GL_MODELVIEW_MATRIX: memcpy(out, state.modelView.back().data(), sizeof(Matrix)); break;
        case GL_PROJECTION_MATRIX: memcpy(out, state.projection.back().data(), sizeof(Matrix)); break;
        case GL_TEXTURE_MATRIX: memcpy(out, state.textureMatrices[state.activeTexture].back().data(), sizeof(Matrix)); break;
        case GL_CURRENT_COLOR: memcpy(out, state.color, sizeof(state.color)); break;
        case GL_VIEWPORT: for (int i = 0; i < 4; ++i) out[i] = float(state.draw.viewport[i]); break;
        case GL_SCISSOR_BOX: for (int i = 0; i < 4; ++i) out[i] = float(state.draw.scissor[i]); break;
        case GL_DEPTH_RANGE: memcpy(out, state.draw.depthRange, sizeof(state.draw.depthRange)); break;
        case GL_FOG_COLOR: memcpy(out, state.fogColor, sizeof(state.fogColor)); break;
        default: { GLint value = 0; vkfGetIntegerv(which, &value); *out = float(value); break; }
    }
}
void APIENTRY vkfGetBooleanv(GLenum which, GLboolean *out)
{
    if (which == GL_COLOR_WRITEMASK) { for (int i = 0; i < 4; ++i) out[i] = state.draw.colorWrite[i]; }
    else if (which == GL_DEPTH_WRITEMASK) *out = state.draw.depthWrite;
    else *out = vkfIsEnabled(which);
}
void APIENTRY vkfGetTexLevelParameteriv(GLenum target, GLint level, GLenum which, GLint *out)
{
    auto &t = boundTexture(target); auto it = t.levels.find(level);
    if (it == t.levels.end()) { *out = 0; return; }
    switch (which)
    {
        case GL_TEXTURE_WIDTH: *out = it->second.width; break;
        case GL_TEXTURE_HEIGHT: *out = it->second.height; break;
        case GL_TEXTURE_INTERNAL_FORMAT: *out = GL_RGBA8; break;
        case GL_TEXTURE_COMPRESSED: case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: *out = 0; break;
        default: fail("texture level query");
    }
}
void APIENTRY vkfCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *) { fail("compressed texture upload while compression is disabled"); }
void APIENTRY vkfGetCompressedTexImage(GLenum, GLint, void *) { fail("compressed texture readback while compression is disabled"); }
void APIENTRY vkfDeleteSamplers(GLsizei, const GLuint *) {} // No native sampler objects are exposed.
void APIENTRY vkfGenSamplers(GLsizei, GLuint *) { fail("sampler objects while capability is disabled"); }
void APIENTRY vkfBindSampler(GLuint, GLuint id) { if (id) fail("sampler object binding while capability is disabled"); }
GLboolean APIENTRY vkfIsSampler(GLuint) { return GL_FALSE; }
void APIENTRY vkfSamplerParameterf(GLuint, GLenum, GLfloat) { fail("sampler object parameter while capability is disabled"); }
void APIENTRY vkfSamplerParameteri(GLuint, GLenum, GLint) { fail("sampler object parameter while capability is disabled"); }

// Keep the host OpenGL dispatch intact for later renderer switches.
#define FRONTEND_DISPATCH(X) \
    X(Begin) \
    X(End) \
    X(Vertex4f) \
    X(Vertex3f) \
    X(Vertex2f) \
    X(Vertex2i) \
    X(Vertex3fv) \
    X(Vertex2fv) \
    X(Color4f) \
    X(Color3f) \
    X(Color4fv) \
    X(Color4ub) \
    X(TexCoord2f) \
    X(MultiTexCoord2f) \
    X(DrawArrays) \
    X(DrawElements) \
    X(VertexPointer) \
    X(ColorPointer) \
    X(TexCoordPointer) \
    X(VertexAttribPointer) \
    X(EnableClientState) \
    X(DisableClientState) \
    X(EnableVertexAttribArray) \
    X(DisableVertexAttribArray) \
    X(ActiveTexture) \
    X(ClientActiveTexture) \
    X(MatrixMode) \
    X(LoadIdentity) \
    X(LoadMatrixf) \
    X(MultMatrixf) \
    X(LoadMatrixd) \
    X(MultMatrixd) \
    X(PushMatrix) \
    X(PopMatrix) \
    X(Translatef) \
    X(Scalef) \
    X(Rotatef) \
    X(Ortho) \
    X(Frustum) \
    X(Viewport) \
    X(Scissor) \
    X(Enable) \
    X(Disable) \
    X(IsEnabled) \
    X(DepthFunc) \
    X(DepthMask) \
    X(DepthRange) \
    X(BlendFunc) \
    X(BlendEquation) \
    X(AlphaFunc) \
    X(CullFace) \
    X(FrontFace) \
    X(ColorMask) \
    X(PolygonMode) \
    X(PolygonOffset) \
    X(LineWidth) \
    X(StencilFunc) \
    X(StencilOp) \
    X(StencilMask) \
    X(Fogf) \
    X(Fogi) \
    X(Fogfv) \
    X(PushAttrib) \
    X(PopAttrib) \
    X(Hint) \
    X(ShadeModel) \
    X(ClearColor) \
    X(ClearDepth) \
    X(ClearStencil) \
    X(ClearAccum) \
    X(Clear) \
    X(Finish) \
    X(Flush) \
    X(GenBuffers) \
    X(BindBuffer) \
    X(IsBuffer) \
    X(DeleteBuffers) \
    X(BufferData) \
    X(BufferSubData) \
    X(BufferStorage) \
    X(MapBuffer) \
    X(MapBufferRange) \
    X(UnmapBuffer) \
    X(FenceSync) \
    X(DeleteSync) \
    X(IsSync) \
    X(ClientWaitSync) \
    X(GenTextures) \
    X(BindTexture) \
    X(IsTexture) \
    X(DeleteTextures) \
    X(TexImage2D) \
    X(TexSubImage2D) \
    X(TexParameterf) \
    X(TexParameteri) \
    X(TexEnvf) \
    X(TexEnvi) \
    X(PixelStorei) \
    X(GenerateMipmap) \
    X(CreateShader) \
    X(ShaderSource) \
    X(CompileShader) \
    X(GetShaderiv) \
    X(GetShaderInfoLog) \
    X(DeleteShader) \
    X(CreateProgram) \
    X(AttachShader) \
    X(DetachShader) \
    X(LinkProgram) \
    X(UseProgram) \
    X(IsProgram) \
    X(DeleteProgram) \
    X(GetProgramiv) \
    X(GetProgramInfoLog) \
    X(BindAttribLocation) \
    X(GetAttribLocation) \
    X(GetUniformLocation) \
    X(Uniform1f) \
    X(Uniform1i) \
    X(Uniform2f) \
    X(Uniform4f) \
    X(Uniform2fv) \
    X(Uniform4fv) \
    X(UniformMatrix4fv) \
    X(ReadPixels) \
    X(Accum) \
    X(GetError) \
    X(GetString) \
    X(GetIntegerv) \
    X(GetFloatv) \
    X(GetBooleanv) \
    X(GetTexLevelParameteriv) \
    X(CompressedTexImage2D) \
    X(GetCompressedTexImage) \
    X(DeleteSamplers) \
    X(GenSamplers) \
    X(BindSampler) \
    X(IsSampler) \
    X(SamplerParameterf) \
    X(SamplerParameteri)

#define DECLARE_SAVED(name) decltype(glad_gl##name) saved##name = nullptr;
FRONTEND_DISPATCH(DECLARE_SAVED)
#undef DECLARE_SAVED
} // namespace

void vulkanFrontendInstall(int width, int height)
{
    if (installed) fail("duplicate frontend installation");
    state = Frontend{};
    state.width = width; state.height = height;
    vkfViewport(0, 0, width, height); vkfScissor(0, 0, width, height);
    buffers.clear(); textures.clear(); shaders.clear(); programs.clear(); attributeStack.clear();
    nextBuffer = nextTexture = nextObject = 1;
    accumulation.clear(); accumulationTexture = 0;
#define INSTALL(name) saved##name = glad_gl##name; glad_gl##name = vkf##name;
    FRONTEND_DISPATCH(INSTALL)
#undef INSTALL
    installed = true;
}

void vulkanFrontendUninstall()
{
    if (!installed) return;
    vulkanFinish();
    for (auto const &t : textures) vulkanDeleteTexture(t.first);
    if (accumulationTexture) vulkanDeleteTexture(accumulationTexture);
    for (auto fence : fences) delete reinterpret_cast<uint8_t *>(fence);
    fences.clear(); buffers.clear(); textures.clear(); shaders.clear(); programs.clear();
    attributeStack.clear(); accumulation.clear(); accumulationTexture = 0;
#define RESTORE(name) glad_gl##name = saved##name;
    FRONTEND_DISPATCH(RESTORE)
#undef RESTORE
    installed = false;
}

void vulkanFrontendResize(int width, int height)
{
    state.width = width; state.height = height;
    vkfViewport(0, 0, width, height); vkfScissor(0, 0, width, height);
    accumulation.clear();
}
#undef FRONTEND_DISPATCH
#endif // USE_VULKAN
