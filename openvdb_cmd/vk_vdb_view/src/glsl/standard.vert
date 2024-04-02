// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#version 460
#extension GL_EXT_debug_printf : enable
// Extremely bare-bones vertex shader doing standard model-view and projection transform,
// and outputting vertex colors and normals. Includes special-case branch for drawing the
// gnomon at the corner of the viewport.

layout(constant_id = 0) const bool scGnomonDraw = false;

layout(location = 0) in vec4 vertPosIn;
layout(location = 1) in vec3 vertNormalIn;
layout(location = 2) in vec4 vertColorIn;

layout(location = 0) out vec3 vertNormalOut;
layout(location = 1) out vec4 vertColorOut;

layout(binding = 0) uniform WorldInfo {
    mat4 MV; // Model-View matrix
    mat4 P; // Perspective projection matrix
} uWorld;

layout(push_constant) uniform pushConstants {
    layout(offset = 0) float pointSize;
};

vec4 computeGnomonGlPos(in vec4 p);

void main() {
    // Special case for drawing viewport gnomon
    if (scGnomonDraw) {
        gl_Position = computeGnomonGlPos(vertPosIn);
        vertColorOut = vertColorIn;
        return; 
    }

    // Typical use case
    gl_Position = uWorld.P * uWorld.MV * vertPosIn;
    vertColorOut = vertColorIn;
    vertNormalOut = mat3(uWorld.MV) * vertNormalIn;
    gl_PointSize = pointSize;
}

// Simple orthographic projection matrix used to project gnomon . 
const mat4 gnomonOrthoProj = mat4(
    1.0, 0.0, 0.0, 0.0,
    0.0, -1.0, 0.0, 0.0,
    0.0, 0.0, -0.1, 0.0,
    0.0, 0.0, 0.0, 1.0
);
vec4 computeGnomonGlPos(in vec4 p) {
    // Gnomon transform is MV with no translation, and position of -3 along the z-axis to keep it in view. 
    const mat4 gnomonMV = mat4(
        vec4(uWorld.MV[0].xyz, 0.0),
        vec4(uWorld.MV[1].xyz, 0.0),
        vec4(uWorld.MV[2].xyz, 0.0),
        vec4(0.0, 0.0, -3.0, 1.0)
    );

    return gnomonOrthoProj * gnomonMV * vertPosIn;
}