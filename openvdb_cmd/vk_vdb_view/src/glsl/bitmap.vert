// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#version 460
// #extension GL_EXT_debug_printf : enable

layout(location = 0) in uvec4 inTextBox;
layout(location = 1) in uint inTextOffset;
layout(location = 2) in float inPixelScale;
layout(location = 3) in vec4 inFontColor;
layout(location = 4) in vec4 inBgColor;

layout(location = 0) noperspective sample out vec2 outTextBoxUV;
layout(location = 1) flat out uint outTextOffset;
layout(location = 2) flat out vec4 outFontColor;
layout(location = 3) flat out vec4 outBgColor;
layout(location = 4) flat out uint outInstIndex;


layout(push_constant) uniform PushConstants {
    vec2 uViewportRes;
};

void main() {
    // Manually form unit quad ([0, 0] -> [1, 1])
    const vec2 quadUV = vec2(gl_VertexIndex & 0x1, (gl_VertexIndex & 0x2) >> 1u); // Forms [0,0] -> [1, 1] quad

    // Adds +1 pixel of padding above glyphs to make background visible
    const uvec2 boxPadding = uvec2(0, 1);
    
    // Transform quad to cover text box in clip space.
    const vec2 clipSpaceSpan = (2.0 * inPixelScale * (inTextBox.zw + boxPadding) / uViewportRes.xy);
    const vec2 clipSpaceOffset = (2.0 * (inTextBox.xy - boxPadding*inPixelScale) / uViewportRes.xy) - 1.0;
    gl_Position = vec4((quadUV * clipSpaceSpan) + clipSpaceOffset, 0.0, 1.0);

    // Write text box pixel UV coordinates, which will be interpolated for fragment shading. These coordinates
    // are used for glyph rendering, enabling optional scaling and multi-sampling. 
    outTextBoxUV = (quadUV * vec2(inTextBox.zw + boxPadding)) - vec2(boxPadding);
    
    // Pass along without interpolating. 
    outTextOffset = inTextOffset;
    outFontColor = inFontColor;
    outBgColor = inBgColor;
    outInstIndex = gl_InstanceIndex;
}