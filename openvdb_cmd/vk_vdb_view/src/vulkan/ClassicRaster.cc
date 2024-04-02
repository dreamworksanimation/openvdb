// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "ClassicRaster.h"
#include <spv_shaders.h>

using namespace openvdb::math;

void VulkanClassicRasterGeo::reset() {
    mVertexBuffer.reset();
    mIndexBuffer.reset();
    mPartSpans.clear();
}

VulkanClassicRasterGeoBuilder::Vertex& VulkanClassicRasterGeoBuilder::addVert(const Vertex& aVert) { 
    Vertex& ref = mVerts.emplace_back(aVert);
    size32Check(); 
    return ref;
}

void VulkanClassicRasterGeoBuilder::startNewPart() {
    if (!mIndices.empty()) {
        if (!mPartOffsets.empty() && !mPartsAreIndexed) {
            throw std::logic_error(
                "VulkanClassicRasterGeoBuilder: Parts indexing was started with an empty index list,"
                "but is now being continued with a non-empty index list. The first call to startNewPart() "
                "must be made after indices have been added, or called on an unindexed geometry.");
        }
        mPartOffsets.push_back(mIndices.size());
        mPartsAreIndexed = true;
    } else if (!mVerts.empty()) {
        assert(!mPartsAreIndexed);
        mPartOffsets.push_back(mVerts.size());
    }
}

VulkanClassicRasterGeo VulkanClassicRasterGeoBuilder::build(const vult::VulkanRuntimeScope& aScope) const {
    size32Check();

    // Refuse to create emtpy vertex buffer, return invalid 'empty' geometry
    if (mVerts.empty()) return VulkanClassicRasterGeo {};

    // Catch case where parts are made for unindexed geometry, but then indices are added later.
    if (!mPartOffsets.empty() && !mPartsAreIndexed && !mIndices.empty()) {
        throw std::logic_error(
            "VulkanClassicRasterGeoBuilder: Parts list was created for an unindexed geometry, "
            "but the geometry is now being built as indexed. If the first call to startNewPart() "
            "is made while the builder has no indices, then indices must not be added later.");
    }

    vk::DeviceSize vertBufferSize = mVerts.size() * sizeof(Vertex);
    vk::DeviceSize indexBufferSize = mIndices.size() * sizeof(uint32_t);

    // Create and allocate GPU buffers
    VulkanClassicRasterGeo geo;
    geo.mVertexBuffer = vult::UploadStagedBuffer(
        aScope.getAllocator(), vertBufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer);
    if (!mIndices.empty()) {
        geo.mIndexBuffer = vult::UploadStagedBuffer(
            aScope.getAllocator(), indexBufferSize,
            vk::BufferUsageFlagBits::eIndexBuffer);
    }

    // Upload vertex and index data to the GPU
    vult::QueueClosure closure = aScope.getTransferQueueClosure();
    vk::CommandBuffer cmdBuffer = closure.beginSingleSubmitCommands();
    geo.mVertexBuffer.stageData(mVerts.data());
    if (geo.mIndexBuffer) geo.mIndexBuffer.stageData(mIndices.data());
    geo.mVertexBuffer.recUpload(cmdBuffer);
    if (geo.mIndexBuffer) geo.mIndexBuffer.recUpload(cmdBuffer);
    closure.endSingleSubmitCommandsAndFlush(cmdBuffer);
    
    // Construct parts list. 
    if (!mPartOffsets.empty()) {
        geo.mPartSpans.reserve(mPartOffsets.size() + 1);
        uint32_t lastOffset = 0;
        for (const uint32_t offset : mPartOffsets) {
            geo.mPartSpans.emplace_back(lastOffset, offset-lastOffset);
            lastOffset = offset;
        }
        const size_t endSize = mPartsAreIndexed ? mIndices.size() : mVerts.size();
        geo.mPartSpans.emplace_back(lastOffset, endSize - lastOffset);
    }

    // Return with move to avoid destroying buffers. 
    return std::move(geo);
}

// Unlikely 32-bit limitation check.
void VulkanClassicRasterGeoBuilder::size32Check() const {
    if (mVerts.size() >= UINT32_MAX-1)
        throw std::runtime_error("ClassicRasterGeoBuilder: Vertex list has hit the 32-bit limit!");
    if (mIndices.size() >= UINT32_MAX-1)
        throw std::runtime_error("ClassicRasterGeoBuilder: Index list has hit the 32-bit limit!");
}

VulkanClassicRasterEngine::VulkanClassicRasterEngine(vk::Device aDevice, VmaAllocator aAllocator)
: mAllocator(aAllocator),
  mDevice(aDevice),
  mUniformBuffer(aAllocator, sizeof(UniformData), vk::BufferUsageFlagBits::eUniformBuffer)
{
    const vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eUniformBuffer, 1u, vk::ShaderStageFlagBits::eVertex);
    vk::DescriptorSetLayoutCreateInfo createInfo(vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, binding);
    mDescriptorLayout = mDevice.createDescriptorSetLayoutUnique(createInfo);

    mPushConstantRanges = {
        vk::PushConstantRange(vk::ShaderStageFlagBits::eVertex, 0u, sizeof(float)),
        vk::PushConstantRange(vk::ShaderStageFlagBits::eFragment, sizeof(float), sizeof(ShadeMode))
    };
    mPipelineLayout = mDevice.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, mDescriptorLayout.get(), mPushConstantRanges));

    vk::ShaderCreateInfoEXT shaderCreateInfo(
        {}, vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eFragment,
        vk::ShaderCodeTypeEXT::eSpirv);

    shaderCreateInfo
        .setCode<uint32_t>(sStandardVertexShader)
        .setPName("main")
        .setSetLayouts(mDescriptorLayout.get())
        .setPushConstantRanges(mPushConstantRanges);
    const auto [vsr, vsShaderObj] = aDevice.createShaderEXT(shaderCreateInfo);
    vk::resultCheck(vsr, "Shader compilation failed due to incompatible binary. "
                         "This should be impossible, and must be programmer or driver error!");
    mVertShader = vsShaderObj;
    
    shaderCreateInfo
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setNextStage(vk::ShaderStageFlagBits(0u))
        .setCode<uint32_t>(sStandardFragmentShader);
    const auto [vsf, fsShaderObj] = aDevice.createShaderEXT(shaderCreateInfo);
    vk::resultCheck(vsf, "Shader compilation failed due to incompatible binary. "
                         "This should be impossible, and must be programmer or driver error!");
    mFragShader = fsShaderObj;
}

void VulkanClassicRasterEngine::cleanup() {
    mUniformBuffer.reset();

    mPipelineLayout.reset();
    mDescriptorLayout.reset();

    if (mVertShader) mDevice.destroyShaderEXT(mVertShader);
    if (mFragShader) mDevice.destroyShaderEXT(mFragShader);
    mVertShader = mFragShader = VK_NULL_HANDLE;
}

void VulkanClassicRasterEngine::setUniforms(const Mat4s aModelView, const Mat4s aProjection) {
    *mUniformBuffer.getMappedPtrAs<UniformData>() = UniformData{aModelView, aProjection};
}

void VulkanClassicRasterEngine::recStandardState(vk::CommandBuffer aCmdBuffer) {
        // Input assembly and vertex shading
        aCmdBuffer.setRasterizerDiscardEnable(false);
        aCmdBuffer.setPrimitiveRestartEnable(true);
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
        aCmdBuffer.setColorBlendEnableEXT(0u, false);
        aCmdBuffer.setColorBlendEquationEXT(0u, vk::ColorBlendEquationEXT(
            vk::BlendFactor::eSrcAlpha, vk::BlendFactor::eOneMinusSrcAlpha, vk::BlendOp::eAdd,
            vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendOp::eAdd));
        aCmdBuffer.setColorWriteMaskEXT(0u, ccf::eR | ccf::eG | ccf::eB | ccf::eA);
        aCmdBuffer.setDepthTestEnable(true);
        aCmdBuffer.setDepthWriteEnable(true);
        aCmdBuffer.setDepthCompareOp(vk::CompareOp::eLess);
        aCmdBuffer.setDepthBoundsTestEnable(false);
        aCmdBuffer.setDepthBiasEnable(false);
        aCmdBuffer.setDepthClampEnableEXT(false);
        aCmdBuffer.setStencilTestEnable(false);
        aCmdBuffer.setLogicOpEnableEXT(false);
}

void VulkanClassicRasterEngine::recPushDescriptorSet(vk::CommandBuffer aCmdBuffer) {
    vk::DescriptorBufferInfo bufferInfo(mUniformBuffer.buffer(), 0u, sizeof(UniformData));
    vk::WriteDescriptorSet writeDesc(VK_NULL_HANDLE, 0u, 0u, vk::DescriptorType::eUniformBuffer, {}, bufferInfo, {});
    aCmdBuffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, mPipelineLayout.get(), 0u, writeDesc);
}

void VulkanClassicRasterEngine::recUniformBufferHostBarrier(vk::CommandBuffer aCmdBuffer) {
    vk::BufferMemoryBarrier2 uniformBarrier(
        vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eVertexShader, vk::AccessFlagBits2::eShaderRead);
    uniformBarrier.setBuffer(mUniformBuffer.buffer());
    uniformBarrier.setSize(sizeof(UniformData));

    aCmdBuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, uniformBarrier));
}

void VulkanClassicRasterEngine::recSetVertexInputs(vk::CommandBuffer aCmdBuffer) {
    aCmdBuffer.setVertexInputEXT(vk::VertexInputBindingDescription2EXT(0u, sizeof(Vertex), vk::VertexInputRate::eVertex, 1u),
            {
                vk::VertexInputAttributeDescription2EXT(0u, 0u, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position)), // Position
                vk::VertexInputAttributeDescription2EXT(1u, 0u, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal)), // Normal
                vk::VertexInputAttributeDescription2EXT(2u, 0u, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, color)) // Color
            }
        );
}

void VulkanClassicRasterEngine::recConfigureShading(vk::CommandBuffer aCmdBuffer, const DrawMode aMode, const ShadeMode aShading) {
    aCmdBuffer.pushConstants<float>(mPipelineLayout.get(), vk::ShaderStageFlagBits::eVertex, 0, mPointSize);
    aCmdBuffer.pushConstants<ShadeMode>(mPipelineLayout.get(), vk::ShaderStageFlagBits::eFragment, sizeof(float), aShading);

    switch (aMode) {
        case DRAW_MODE_TRIANGLE_STRIP_FILL:
            aCmdBuffer.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleStrip);
            aCmdBuffer.setPolygonModeEXT(vk::PolygonMode::eFill);
            break;
        case DRAW_MODE_TRIANGLE_LIST_WIRE:
            aCmdBuffer.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
            aCmdBuffer.setPolygonModeEXT(vk::PolygonMode::eLine);
            break;
        case DRAW_MODE_LINE_LIST:
            aCmdBuffer.setPrimitiveTopology(vk::PrimitiveTopology::eLineList);
            aCmdBuffer.setPolygonModeEXT(vk::PolygonMode::eLine);
            break;
        case DRAW_MODE_POINT_LIST:
            aCmdBuffer.setPrimitiveTopology(vk::PrimitiveTopology::ePointList);
            aCmdBuffer.setPolygonModeEXT(vk::PolygonMode::ePoint);
            break;
        default: break;
    }
}

void VulkanClassicRasterEngine::recPreDraw(vk::CommandBuffer aCmdBuffer) {
    recStandardState(aCmdBuffer);
    recPushDescriptorSet(aCmdBuffer);
    recSetVertexInputs(aCmdBuffer);
    aCmdBuffer.bindShadersEXT(vk::ShaderStageFlagBits::eVertex, mVertShader);
    aCmdBuffer.bindShadersEXT(vk::ShaderStageFlagBits::eFragment, mFragShader);
}

void VulkanClassicRasterEngine::recDrawGeo(
    vk::CommandBuffer aCmdBuffer,
    const VulkanClassicRasterGeo& aGeo,
    const DrawMode aMode,
    const ShadeMode aShading,
    const vk::ArrayProxy<uint32_t> aParts)
{
    if (aGeo.isEmpty()) {
        std::cerr << "WARNING: VulkanClassicRasterEngine was passed empty geometry. Skipping draw..." << std::endl;
        return;
    }

    aCmdBuffer.bindVertexBuffers(0, aGeo.mVertexBuffer.buffer(), {0u});
    recConfigureShading(aCmdBuffer, aMode, aShading);

    if (!aGeo.isIndexed()) {
        if (!aParts.empty()) {
            for (uint32_t part : aParts) {
                const auto [first, count] = aGeo.mPartSpans.at(part);
                aCmdBuffer.draw(count, 1u, first, 0u);
            }
        } else {
            aCmdBuffer.draw(aGeo.numVerts(), 1u, 0u, 0u);
        }
    } else {
        aCmdBuffer.bindIndexBuffer(aGeo.mIndexBuffer.buffer(), 0u, vk::IndexType::eUint32);
        if (!aParts.empty()) {
            for (uint32_t part : aParts) {
                const auto [first, count] = aGeo.mPartSpans.at(part);
                aCmdBuffer.drawIndexed(count, 1u, first, 0u, 0u);
            }
        } else {
            aCmdBuffer.drawIndexed(aGeo.numIndices(), 1u, 0u, 0u, 0u);
        }
    }
}

void VulkanClassicRasterEngine::recDraw(
    vk::CommandBuffer aCmdBuffer,
    uint32_t aFirstVertex,
    uint32_t aVertexCount,
    const vk::Buffer& aVertexBuffer,
    const DrawMode aMode,
    const ShadeMode aShading)
{
    recConfigureShading(aCmdBuffer, aMode, aShading);
    aCmdBuffer.bindVertexBuffers(0, aVertexBuffer, {0u});
    aCmdBuffer.draw(aVertexCount, 1u, aFirstVertex, 0u);
}

void VulkanClassicRasterEngine::recIndexedDraw(
    vk::CommandBuffer aCmdBuffer,
    uint32_t aFirstIndex,
    uint32_t aIndexCount,
    const vk::Buffer& aVertexBuffer,
    const vk::Buffer& aIndexBuffer,
    const DrawMode aMode,
    const ShadeMode aShading)
{
    recConfigureShading(aCmdBuffer, aMode, aShading);
    aCmdBuffer.bindVertexBuffers(0, aVertexBuffer, {0u});
    aCmdBuffer.bindIndexBuffer(aIndexBuffer, 0u, vk::IndexType::eUint32);
    aCmdBuffer.drawIndexed(aIndexCount, 1u, aFirstIndex, 0u, 0u);
}
