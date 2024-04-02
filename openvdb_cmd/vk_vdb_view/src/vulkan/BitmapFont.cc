// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "BitmapFont.h"
#include "../Font.h"

#include <spv_shaders.h>

/// @brief Bit-twiddle returning the power of two >= v
constexpr static __always_inline uint32_t pow2ceil(uint32_t v){
    --v;
    for(uint32_t i = 1; i < 16u; i <<= 1u) { v |= v >> i; }
    return ++v;
}

VulkanBitmapFont13Engine::VulkanBitmapFont13Engine(vk::Device aDevice, VmaAllocator aAllocator, const vult::QueueClosure& aTransferClosure)
: mDevice(aDevice),
  mAllocator(aAllocator),
  mTransferQueue(aTransferClosure),
  mTextBuffer(aAllocator, 512, vk::BufferUsageFlagBits::eStorageBuffer),
  mGlyphBuffer(aAllocator, sizeof(openvdb_viewer::BitmapFont13::sCharacters), vk::BufferUsageFlagBits::eUniformBuffer),
  mTextLineVertData(aAllocator, sizeof(TextLine)*16u, vk::BufferUsageFlagBits::eVertexBuffer)
{
    // Setup resource and pipeline options needed for binding resources to shaders. 
    const std::array<vk::DescriptorSetLayoutBinding, 2> bufferBindings {
        vk::DescriptorSetLayoutBinding(0u, vk::DescriptorType::eUniformBuffer, 1u, vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(1u, vk::DescriptorType::eStorageBuffer, 1u, vk::ShaderStageFlagBits::eFragment)
    };

    const vk::DescriptorSetLayoutCreateInfo descSetLayoutInfo(vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, bufferBindings);
    mDescriptorLayout = mDevice.createDescriptorSetLayoutUnique(descSetLayoutInfo);

    // Push constants are used to supply the vertex shader with the framebuffer dimensions. 
    const vk::PushConstantRange pushConstantRange(vk::ShaderStageFlagBits::eVertex, 0u, 2*sizeof(float));
    mPipelineLayout = mDevice.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, mDescriptorLayout.get(), pushConstantRange));

    vk::ShaderCreateInfoEXT shaderCreateInfo(
        {}, vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eFragment,
        vk::ShaderCodeTypeEXT::eSpirv);

    shaderCreateInfo
        .setCode<uint32_t>(sBitmapVertexShader)
        .setPName("main")
        .setSetLayouts(mDescriptorLayout.get())
        .setPushConstantRanges(pushConstantRange);
    const auto [vsr, vsShaderObj] = aDevice.createShaderEXT(shaderCreateInfo);
    vk::resultCheck(vsr, "Shader compilation failed due to incompatible binary. "
                         "This should be impossible, and must be programmer or driver error!");
    mVertShader = vsShaderObj;
    
    shaderCreateInfo
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setNextStage(vk::ShaderStageFlagBits(0u))
        .setCode<uint32_t>(sBitmapFragmentShader);
    const auto [vsf, fsShaderObj] = aDevice.createShaderEXT(shaderCreateInfo);
    vk::resultCheck(vsf, "Shader compilation failed due to incompatible binary. "
                         "This should be impossible, and must be programmer or driver error!");
    mFragShader = fsShaderObj;

    // Upload bitmap font glyphs to GPU
    mGlyphBuffer.uploadNow(openvdb_viewer::BitmapFont13::sCharacters, mTransferQueue);
}

void VulkanBitmapFont13Engine::cleanup() {
    mTextBuffer.reset();
    mGlyphBuffer.reset();
    mTextLineVertData.reset();

    mPipelineLayout.reset();
    mDescriptorLayout.reset();

    if (mVertShader) mDevice.destroyShaderEXT(mVertShader);
    if (mFragShader) mDevice.destroyShaderEXT(mFragShader);
    mVertShader = mFragShader = VK_NULL_HANDLE;
}

void VulkanBitmapFont13Engine::startFontRendering(const vk::Viewport& aViewport) {
    mViewport = aViewport;
}

void VulkanBitmapFont13Engine::addLine(
        uint32_t px, uint32_t py, const std::string& line, float aPixSize,
        const Color& fontColor, const Color& backgroundColor)
{
    uint32_t charOffset = mLines.empty() ? 0u : (mLines.back().textOffset + mLines.back().textLen);
    if (line.size()*10 >= (UINT32_MAX-1) || charOffset + line.size() >= UINT32_MAX) {
        throw std::runtime_error("VulkanBitmap13Font: Failed trying to add line of " + std::to_string(line.size()) + " characters to render pass. "
                                 "The 32-bit limits of the class have been exceeded.");
    }

    // Each glyph is 10 bits wide and 13 bits tall. 
    mLines.emplace_back(TextLine {
        px, py, uint32_t(10*line.size()), 13, // Screen-space 2D bounding box of line
        charOffset, uint32_t(line.size()), // Offset of first character in text buffer, and number of chars in the line.
        aPixSize, fontColor, backgroundColor
    });

    // Grow the text buffer in necessary. Bumps size up to the next power of two. 
    // For vdb_view, this shouldn't ever need to happen, but it has been tested. 
    vk::DeviceSize currentBufferSize = charOffset + line.size();
    if (currentBufferSize > mTextBuffer.bufferSize()) {
        vult::MappableBuffer resizedBuffer(mAllocator, pow2ceil(currentBufferSize), vk::BufferUsageFlagBits::eStorageBuffer);
        std::copy_n(mTextBuffer.mapAs<char*>(), mTextBuffer.bufferSize(), resizedBuffer.mapAs<char*>());
        mTextBuffer = std::move(resizedBuffer);
    }

    #ifndef NDEBUG
        const size_t outOfRange = std::count_if(line.cbegin(), line.cend(), [](const uint8_t c) { return c - 32u >= 95u; });
        if (outOfRange > 0) {
            std::cerr << "VulkanBitmap13Font: Warning! Line added to font render pass contains " << outOfRange
                      << " characters outside the supported range (ASCII 32-126). These will not be rendered." << std::endl;
        }
    #endif

    // Concatenate string into text buffer on GPU
    std::copy_n(line.data(), line.size(), std::next(mTextBuffer.mapAs<char*>(), charOffset));
}

void VulkanBitmapFont13Engine::recCommitFontRendering(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) {
    assert(aRenderInfo.colorAttachmentCount > 0);

    // Record commands for upload each TextLine object to the GPU as vertex data. 
    const vk::DeviceSize textBoxBufferSize = mLines.size() * sizeof(TextLine);
    const vk::DeviceSize finalTextBufferSize = mLines.back().textOffset + mLines.back().textLen;
    if (textBoxBufferSize > mTextLineVertData.bufferSize()) {
        // Buffer is too small. Resize it. 
        mTextLineVertData = vult::UploadStagedBuffer(mAllocator, pow2ceil(textBoxBufferSize), vk::BufferUsageFlagBits::eVertexBuffer);
    }
    mTextLineVertData.stageData(mLines.data(), textBoxBufferSize);
    mTextLineVertData.recUpload(aCmdBuffer);
    mTextLineVertData.recUploadBarrier(aCmdBuffer,
        vk::PipelineStageFlagBits2::eVertexAttributeInput | vk::PipelineStageFlagBits2::eCopy,
        vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eTransferWrite);

    // Make sure text buffer is current on the GPU.
    mTextBuffer.flushAndInvalidatePages();
    vk::BufferMemoryBarrier2 textBarrier(
        vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
        {}, {}, mTextBuffer, 0u, finalTextBufferSize);
    aCmdBuffer.pipelineBarrier2({{}, {}, textBarrier});

    // Start of render pass
    aCmdBuffer.beginRendering(aRenderInfo);

    {// Record graphics pipeline state setting commands
        // Input assembly and vertex shading
        aCmdBuffer.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleStrip);
        aCmdBuffer.setPolygonModeEXT((vk::PolygonMode::eFill));
        aCmdBuffer.setRasterizerDiscardEnable(false);
        aCmdBuffer.setPrimitiveRestartEnable(false);
        aCmdBuffer.setCullMode(vk::CullModeFlagBits::eBack);
        aCmdBuffer.setFrontFace(vk::FrontFace::eClockwise);

        // Rasterization and fragment shading
        using ccf = vk::ColorComponentFlagBits;
        aCmdBuffer.setConservativeRasterizationModeEXT(vk::ConservativeRasterizationModeEXT::eDisabled);
        aCmdBuffer.setSampleLocationsEnableEXT(false);
        aCmdBuffer.setRasterizationSamplesEXT(mSampleCount);
        aCmdBuffer.setSampleMaskEXT(mSampleCount, std::array<vk::SampleMask, 2>{~0u, ~0u}.data());
        aCmdBuffer.setAlphaToCoverageEnableEXT(false);
        aCmdBuffer.setAlphaToOneEnableEXT(false);
        aCmdBuffer.setColorWriteMaskEXT(0u, ccf::eR | ccf::eG | ccf::eB | ccf::eA);
        aCmdBuffer.setDepthTestEnable(false);
        aCmdBuffer.setDepthWriteEnable(false);
        aCmdBuffer.setDepthBoundsTestEnable(false);
        aCmdBuffer.setDepthBiasEnable(false);
        aCmdBuffer.setDepthClampEnableEXT(false);
        aCmdBuffer.setStencilTestEnable(false);
        aCmdBuffer.setLogicOpEnableEXT(false);
        
        aCmdBuffer.setColorBlendEnableEXT(0u, true);
        aCmdBuffer.setColorBlendEquationEXT(0u, vk::ColorBlendEquationEXT(
            vk::BlendFactor::eSrcAlpha, vk::BlendFactor::eOneMinusSrcAlpha, vk::BlendOp::eAdd,
            vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendOp::eAdd
            )
        );
    }

    {// Push descriptor set for glyph and texel buffer.
        const std::array<vk::DescriptorBufferInfo, 2> bufferInfos {
            vk::DescriptorBufferInfo(mGlyphBuffer, 0u, mGlyphBuffer.bufferSize()),
            vk::DescriptorBufferInfo(mTextBuffer, 0u, mTextBuffer.bufferSize())
        };
        const std::array<vk::WriteDescriptorSet, 2> writeDescs {
            vk::WriteDescriptorSet(VK_NULL_HANDLE, 0u, 0u, vk::DescriptorType::eUniformBuffer, {}, bufferInfos[0]),
            vk::WriteDescriptorSet(VK_NULL_HANDLE, 1u, 0u, vk::DescriptorType::eStorageBuffer, {}, bufferInfos[1]),
        };
        aCmdBuffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, mPipelineLayout.get(), 0u, writeDescs);
    }

    // Send viewport (width, height) to vertex shader as a push constant. 
    const float viewportResolution[2] {mViewport.width, mViewport.height};
    aCmdBuffer.pushConstants<float>(mPipelineLayout.get(), vk::ShaderStageFlagBits::eVertex, 0u, viewportResolution);

    {// Configure vertex buffer attributes for the upcoming draw calls. There are no per-vertex attributes in the draw call,
     // just per-instance attributes defining each `TextLine`. 
        aCmdBuffer.setVertexInputEXT(vk::VertexInputBindingDescription2EXT(0u, sizeof(TextLine), vk::VertexInputRate::eInstance, 1u),
            {
                vk::VertexInputAttributeDescription2EXT(0u, 0u, vk::Format::eR32G32B32A32Uint, offsetof(TextLine, px)), // inTextBox = uvec4(px,py, xSpan, ySpan)
                vk::VertexInputAttributeDescription2EXT(1u, 0u, vk::Format::eR32Uint, offsetof(TextLine, textOffset)), // inTextOffset
                vk::VertexInputAttributeDescription2EXT(2u, 0u, vk::Format::eR32Sfloat, offsetof(TextLine, pixelScale)), // inPixelScale
                vk::VertexInputAttributeDescription2EXT(3u, 0u, vk::Format::eR32G32B32A32Sfloat, offsetof(TextLine, fgColor)), // inFontColor
                vk::VertexInputAttributeDescription2EXT(4u, 0u, vk::Format::eR32G32B32A32Sfloat, offsetof(TextLine, bgColor)), // inBgColor
            }
        );
    }

    // Bind shaders and vertex buffer, then draw. 
    aCmdBuffer.bindShadersEXT(vk::ShaderStageFlagBits::eVertex, mVertShader);
    aCmdBuffer.bindShadersEXT(vk::ShaderStageFlagBits::eFragment, mFragShader);

    aCmdBuffer.bindVertexBuffers(0u, mTextLineVertData.buffer(), {0u});
    // Render text. Each text box is an instanced quad. 
    aCmdBuffer.draw(4u, uint32_t(mLines.size()), 0u, 0u);

    aCmdBuffer.endRendering();
    // end render pass

    mLines.clear();
}
