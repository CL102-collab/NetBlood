#version 450
// SPDX-License-Identifier: GPL-2.0-only
layout(location = 0) in vec3 a_position;
layout(location = 0) out vec3 v_lightPosition;
layout(push_constant) uniform ShadowPush
{
    vec4 u_light;
    ivec4 u_face;
};

void main()
{
    vec3 p = a_position - u_light.xyz;
    v_lightPosition = p;
    vec3 face;
    if (u_face.x == 0) face = vec3(-p.z, -p.y, p.x);
    else if (u_face.x == 1) face = vec3(p.z, -p.y, -p.x);
    else if (u_face.x == 2) face = vec3(p.x, p.z, p.y);
    else if (u_face.x == 3) face = vec3(p.x, -p.z, -p.y);
    else if (u_face.x == 4) face = vec3(p.x, -p.y, p.z);
    else face = vec3(-p.x, -p.y, -p.z);
    float nearPlane = max(0.001, u_light.w / 4096.0);
    float farPlane = u_light.w;
    gl_Position = vec4(face.xy, farPlane * (face.z - nearPlane) / (farPlane - nearPlane), face.z);
}
