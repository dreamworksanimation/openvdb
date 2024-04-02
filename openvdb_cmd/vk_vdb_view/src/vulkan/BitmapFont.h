// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include "Utils.h"
#include "Resources.h"
#include <openvdb/math/Vec4.h>

/// Class providing Vulkan emulation of the OpenGL backend's `BitmapFont13` class.
///
/// While the bitmap font is the same and the feature sets mostly match, this class differs from `BitmapFont13`:
/// * This class uses Vulkan's framebuffer coordinate convention, with (0,0) at the top-left, as opposed to OpenGL's
///   starting at the bottom-left. Likewise, text boxes are positioned by their top-left corner. 
/// * Being Vulkan based, this class cannot immediately render text into a framebuffer. Instead it records rendering
///   commands into a command buffer supplied by the caller. It also assumes use of Vulkan dynamic rendering render passes. 
/// * This class supports arbitrarily scaling text, whereas the original cannot. It also provides more control
///   over color and alpha-blending with existing framebuffer contents. 
///
/// This class emulates `BitmapFont13` by rasterizing screen-space quads which cover the region filled by each line of text. 
/// Each line of text is drawn on its own instanced quad, where per-instance attribute data describe the text to render.
/// The vertex shader constructs each quad in the correct screen-space position, and outputs text-box coordinates which
/// the fragment treats as a virtual pixel grid, and uses to rasterize the text. For each render, all lines of text are
/// concatenated and uploaded to the GPU as a read-only storage buffer. This class uses the same `sCharacters` bitmap 
/// font from `BitmapFont13`, copied to the GPU as a uniform buffer. 
///
/// The usage pattern for a text rendering pass is:
/// ```c++
/// fontEngine.startFontRendering(myViewport); // Defines framebuffer coordinate space for text
/// // Add an arbitrary number of lines of ASCII text. Effectively unlimited.
/// fontEngine.addLine(xPos, yPos, myString, ...);
/// ...
/// // Record render pass into `myCommandBuffer`. 
/// font.recCommitFontRendering(myRenderingInfo, myCommandBuffer)
/// // At this point, the commands for rendering the text are recorded, but will of course not execute until the command
/// // buffer is submitted. 
/// ```
/// WARNING: Checks are not made to ensure correct ordering of method calls. Calling these functions out of order is 
///          undefined behavior. 
class VulkanBitmapFont13Engine
: virtual public vult::VulkanRuntimeScope::Child,
  public vult::VulkanRuntimeSingleton<VulkanBitmapFont13Engine>
{
public:
    using Color = openvdb::math::Vec4s;
    
    /// Initialize font rendering engine for given device. Resource allocation will use @a aAllocator and upload data using @a aTransferClosure.
    VulkanBitmapFont13Engine(vk::Device aDevice, VmaAllocator aAllocator, const vult::QueueClosure& aTransferClosure);

    /// Initialize font rendering engine for the given scope. Scope must provide a logical device, memory allocator, and transfer queue closure. 
    VulkanBitmapFont13Engine(const vult::VulkanRuntimeScope& aScope)
    : VulkanBitmapFont13Engine(aScope.getDevice(), aScope.getAllocator(), aScope.getTransferQueueClosure())
    {}

    ~VulkanBitmapFont13Engine() { cleanup(); } 
    virtual void cleanupVk(const vult::VulkanRuntimeScope& aScope) override { cleanup(); }

    /// @brief Start new font render pass, for which text position coordinates will lie within @a aViewport
    void startFontRendering(const vk::Viewport& aViewport);

    /// Add a new line of text to this font render pass. Each line added to the render pass gets 
    /// rasterized separately, and will draw over the top of anything previously drawn. Color blending
    /// is enabled so text will linearly blend with anything already in the framebuffer.
    /// @param px,py Framebuffer coordinate of the upper-left corner of the text
    /// @param line ASCII string to be rendered
    /// @param aPixSize [Default = 1.0] Pixel scaling of the rendered text. A value of 1.0 means each 
    ///                 framebuffer pixel maps 1-to-1 to a bit in the bitmap glyphs. Otherwise glyphs 
    ///                 are scaled by this value. Values < 1.0 are valid, but will generally be illegible.
    /// @param fontColor [Default = White] Color of the rendered text, including alpha value. 
    /// @param backgroundColor [Default = Transparent black] Color of text box behind rendered text, including alpha value.
    void addLine(
        uint32_t px, uint32_t py, const std::string& line, float aPixSize = 1.0f,
        const Color& fontColor = Color(1.0f), const Color& backgroundColor = Color(0.0f));

    /// @brief Record font rendering pass into @a aCmdBuffer. Render pass is started using @a aRenderInfo verbatim,
    ///        allowing use of render-pass suspension/resumption. 
    /// NOTE: Text is always rendered as RGBA color into the 0th color attachment. 
    /// NOTE: No depth testing is done during font rendering. Text always draws over existing framebuffer contents.
    void recCommitFontRendering(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer);

    /// Enable multisampling using the provided standard multisampling count. Sample shading is utilized, so glyph edges
    /// will be multi-sampled when appropriate. Multisampling is off by default, and setting this to `e1` disables multisampling. 
    ///
    /// NOTE: Multi-sampling count must match the multi-sampling count of the 0th color attachment of the rendering info struct
    ///       passed to `recCommitFontRendering()`.
    /// NOTE: Multisampling will have no effect when pixel size is an integer. 
    void setMultisamplingCount(vk::SampleCountFlagBits aSampleCount) { mSampleCount = aSampleCount; }

protected:
    void cleanup();

    /// POD struct describing each line of text. A list of these gets uploaded
    /// directly to the GPU to be used during rendering. 
    struct TextLine
    {
        uint32_t px, py, xSpan, ySpan;
        uint32_t textOffset = 0u, textLen = 0u;
        float pixelScale = 1.0f;
        Color fgColor = Color(1.0f);
        Color bgColor = Color(0.0f);
    };

    /// @brief Lines are stored here until `recCommitFontRendering()` is called.
    std::vector<TextLine> mLines;

    vk::Viewport mViewport;
    vk::SampleCountFlagBits mSampleCount = vk::SampleCountFlagBits::e1;

    // Vulkan shader internals. 
    vk::ShaderEXT mVertShader = VK_NULL_HANDLE;
    vk::ShaderEXT mFragShader = VK_NULL_HANDLE;
    vk::UniqueDescriptorSetLayout mDescriptorLayout;
    vk::UniquePipelineLayout mPipelineLayout;
    
    /// @brief Readonly storage buffer holding ASCII string contents.
    /// To keep things simple, buffer memory is host-writable.
    vult::MappableBuffer mTextBuffer;
    /// @brief Uniform buffer holding bitmap font characters
    vult::UploadStagedBuffer mGlyphBuffer;
    /// @brief Vertex buffer holding `TextLine` instances for per-instance attribute data.
    vult::UploadStagedBuffer mTextLineVertData; // Holds array of `TextLine` instances to be rendered

    vk::Device mDevice;
    VmaAllocator mAllocator; 
    vult::QueueClosure mTransferQueue;
};
