// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#version 460 

#extension GL_EXT_shader_8bit_storage: require
#extension GL_EXT_scalar_block_layout: require
#extension GL_EXT_shader_explicit_arithmetic_types_int8: require

// #define DEBUG_SHOW_GLYPH_CHECKERBOARD 1
// #define DEBUG_SHOW_BITMAP_CHECKERBOARD 1

layout(location = 0) noperspective sample in vec2 textBoxUV;
layout(location = 1) flat in uint textOffset;
layout(location = 2) flat in vec4 fontColor;
layout(location = 3) flat in vec4 bgColor;
layout(location = 4) flat in uint instIndex;

layout(location = 0) out vec4 fragColor;

// Uniform buffer of bitmap glyphs. Requires scalar layout and 8-bit access 
// extensions. Can be changed to an array of `uvec4`s to remove this requirement
// if needed for compatability. 
layout(scalar, binding = 0) restrict uniform GlyphBuffer {
    uint8_t uGlyphs[95][13];
};

// Storage buffer containing the text to be rendered. Requires 8-bit access.
// Can be replaced with wider integer type for if necessary. 
layout(std430, binding = 1) readonly restrict buffer TextBuffer {
    uint8_t uText[];
};

const vec2 cGlyphSize = vec2(10.0, 13.0);

void main() {
    // 2D index within the grid glyphs that make up the textbox being rendered.
    // Since only single lines of text are rendered, `glyphCoord.y` is != 0 only as a result of
    // multi-sampling and padding, and is treated an automatic miss. 
    const uvec2 glyphCoord = uvec2(textBoxUV.xy / cGlyphSize);
    const uint charIndex = textOffset + glyphCoord.x;
    if (glyphCoord.y > 0 || any(lessThan(textBoxUV, vec2(0.0)))) {
        fragColor = bgColor;
    } else {
        // Character UVs are textBoxUVs modulo glyph size.
        const vec2 charUV = textBoxUV - vec2(glyphCoord*cGlyphSize);
        // Char coordinates are flipped on the Y axis to unmirror glyphs. The flip is necessary since
        // Vulkan's framebuffer starts in the top-left rather than bottom-left.
        const uvec2 charCoord = uvec2(charUV.x, cGlyphSize.y - 1.0 - floor(charUV.y));
        const uint glyphIndex = uText[charIndex]-32;
        const float bitHit = float(glyphIndex < 95 && (uGlyphs[glyphIndex][charCoord.y] & (128u >> charCoord.x)) != 0);
        
        fragColor = mix(bgColor, fontColor, bitHit);

        #if DEBUG_SHOW_BITMAP_CHECKERBOARD
            const float gridChecker = float((charCoord.x & 1) ^ (charCoord.y & 1));
            fragColor += .05 + .1*gridChecker;
        #endif
    }

    #if DEBUG_SHOW_GLYPH_CHECKERBOARD
        const float glyphChecker = (glyphCoord.x & 1) ^ (glyphCoord.y & 1);
        fragColor = mix(fragColor, vec4(vec3(glyphChecker), 1.0), 0.1);
    #endif
}