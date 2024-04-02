// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#version 460

#define JHAT vec3(0.0, 1.0, 0.0)
#define SKY_COLOR vec3(0.9, 0.9, 1.0)
#define GROUND_COLOR vec3(0.3, 0.3, 0.2)

#define SHADE_MODE_UNLIT_COLOR 0
#define SHADE_MODE_DIFFUSE_COLOR_SURF 1
#define SHADE_MODE_DIFFUSE_ISOSURF 2

layout(location = 0) in vec3 vertNormalIn;
layout(location = 1) in vec4 vertColorIn;

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform pushConstants {
    layout(offset = 4) uint shadingMode;
};

void main() {
    const bool is_diffuse = shadingMode >= SHADE_MODE_DIFFUSE_COLOR_SURF;

    if (shadingMode == SHADE_MODE_UNLIT_COLOR) {
        // Unlit
        fragColor = vertColorIn;
    } else if (is_diffuse) {
        // Diffuse
        const vec3 normal = normalize(vertNormalIn);
        const float w = 0.5 * (1.0 + dot(normal, JHAT));

        if (shadingMode == SHADE_MODE_DIFFUSE_COLOR_SURF) {
            // Mesh shading
            fragColor = mix(vertColorIn, 0.3*vertColorIn, w);
        } else if (shadingMode == SHADE_MODE_DIFFUSE_ISOSURF) {
            // Iso-surface shading
            fragColor = vec4(mix(SKY_COLOR, GROUND_COLOR, w), 1.0);
        }
    } else {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}