// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
#pragma once 

#include "Utils.h"
#include "Resources.h"

#include <openvdb/math/Vec3.h>
#include <openvdb/math/Vec4.h>
#include <openvdb/math/Mat4.h>

/// Helper struct representing some geometry which is renderable by the `VulkanClassicRasterEngine`.
/// Not required, here for convenience. 
///
/// At a minimum, constains a render ready device-local vertex buffer. It may also contain a device-local
/// index buffer, in which case the instance is an indexed geometry. 
///
/// Geometries may optionally be split into parts. In this case the buffers are still contiguous,
/// but the `mPartSpans` member provides a list of <offset, count> pairs indicating the buffer 
/// offsets and counts for rendering separate parts. This provides a mechanism for splitting a geometry
/// among separate draw calls with different primitive topology, shading, ect...
/// If the geometry uses an index buffer ALL parts are interpreted as sub-ranges of the index buffer, otherwise
/// they are interpreted as sub-ranges of the vertex buffer for un-indexed draw calls.
class VulkanClassicRasterGeo
{
public:
    VulkanClassicRasterGeo() = default;
    ~VulkanClassicRasterGeo() = default;

    VulkanClassicRasterGeo(VulkanClassicRasterGeo& aOther) = delete;
    VulkanClassicRasterGeo& operator=(VulkanClassicRasterGeo& aOther) = delete;

    VulkanClassicRasterGeo(VulkanClassicRasterGeo&& aOther) = default;
    VulkanClassicRasterGeo& operator=(VulkanClassicRasterGeo&& aOther) = default;

    struct Vertex
    {
        alignas(16) openvdb::math::Vec3s position = openvdb::math::Vec3s::zero();
        alignas(16) openvdb::math::Vec3s normal = openvdb::math::Vec3s::zero();
        alignas(16) openvdb::math::Vec4s color = openvdb::math::Vec4s::zero();
    };

    bool isValid() const { return mVertexBuffer.isValid(); }
    bool isEmpty() const { return !isValid(); }
    bool isIndexed() const { return isValid() && mIndexBuffer.isValid(); }
    void reset();

    uint32_t numVerts() const { return mVertexBuffer.bufferSize() / sizeof(Vertex); }
    uint32_t numIndices() const { return mIndexBuffer.bufferSize() / sizeof(uint32_t); }
    /// @brief Number of parts this geometry is split into. Returns 1 if the part list is emtpy, because 
    ///        an empty part list implies a single piece spanning the full vertex and index buffer.
    uint32_t numParts() const { return std::max(uint32_t(mPartSpans.size()), 1u); }

    vult::UploadStagedBuffer mVertexBuffer;
    vult::UploadStagedBuffer mIndexBuffer;
    std::vector<std::pair<uint32_t, uint32_t>> mPartSpans;
};

/// @brief Helper class for creating `VulkanClassicRasterGeo`. Thin wrapper around some
///        std::vectors. 
class VulkanClassicRasterGeoBuilder {
public:
    using Vertex = VulkanClassicRasterGeo::Vertex;

    VulkanClassicRasterGeoBuilder() = default;
    VulkanClassicRasterGeoBuilder(size_t n) { mVerts.reserve(n); mIndices.reserve(n); }

    Vertex& addVert(const Vertex& aVert);
    void addIndex(const uint32_t aIndex) { mIndices.push_back(aIndex); size32Check(); }
    void startNewPart();

    uint32_t numVerts() const { size32Check(); return uint32_t(mVerts.size()); }
    uint32_t numIndices() const { size32Check(); return uint32_t(mIndices.size()); }
    uint32_t numParts() const { return mPartOffsets.size(); }

    /// Build renderable geometry. Memory allocator and transfer queue must be available through vult::GlobalVulkanScope.
    VulkanClassicRasterGeo build() const {
        if (!vult::GlobalVulkanRuntimeScope::hasInstance())
            throw std::runtime_error("VulkanClassicRasterGeoBuilder: build() called without a Vulkan scope, but global scope is uninitialized.");
        return build(*vult::GlobalVulkanRuntimeScope::getInstance());
    }
    /// @brief Build renderable geometry, using allocator and transfer queue from the given scope.
    VulkanClassicRasterGeo build(const vult::VulkanRuntimeScope& aScope) const;
    operator VulkanClassicRasterGeo() const { return build(); }

    std::vector<Vertex> mVerts;
    std::vector<uint32_t> mIndices;
    std::vector<uint32_t> mPartOffsets;
protected:
    /// @brief Internal check that geometry does not exceed 32-bit limitations of single draw calls. 
    void size32Check() const;

    bool mPartsAreIndexed = false;
};

class VulkanClassicRasterEngine
: virtual public vult::VulkanRuntimeScope::Child,
  public vult::VulkanRuntimeSingleton<VulkanClassicRasterEngine>
{
public:
    using Vertex = VulkanClassicRasterGeo::Vertex;

    enum DrawMode {
        DRAW_MODE_TRIANGLE_STRIP_FILL,
        DRAW_MODE_TRIANGLE_LIST_WIRE,
        DRAW_MODE_LINE_LIST,
        DRAW_MODE_POINT_LIST
    };

    enum ShadeMode : uint32_t {
        SHADE_MODE_UNLIT_COLOR = 0,
        SHADE_MODE_DIFFUSE_COLOR_SURF = 1,
        SHADE_MODE_DIFFUSE_ISOSURF = 2
    };

    struct UniformData
    {
        alignas(16) openvdb::math::Mat4s modelView;
        alignas(16) openvdb::math::Mat4s perspective;
    };

    VulkanClassicRasterEngine(vk::Device aDevice, VmaAllocator aAllocator);
    VulkanClassicRasterEngine(const vult::VulkanRuntimeScope& aScope) : VulkanClassicRasterEngine(aScope.getDevice(), aScope.getAllocator()) {}
    ~VulkanClassicRasterEngine() { cleanup(); };

    void cleanup();
    virtual void cleanupVk(const vult::VulkanRuntimeScope& aScope) override { cleanup(); } 

    /// @brief Write model-view and projection matrices into uniform buffer. 
    ///
    /// WARNING: Uniform buffer values are read from memory when command buffers are executed and shaders run,
    ///          this means that changes through this function in-between command recordings will not affect
    ///          the actual draw results. Only the state set prior to command buffer execution will be respected. 
    void setUniforms(const openvdb::math::Mat4s aModelView, const openvdb::math::Mat4s aProjection);

    /// Sets the point size. Takes effect upon next call to `recConfigureShading()`, and remains in effect
    /// until `setPointSize()` is called again. Only applies to point topology. 
    void setPointSize(float aPointSize) { mPointSize = aPointSize; }

    /// @brief Provides read-only access to currently set uniform buffer. 
    const UniformData& getUniforms() const { return *mUniformBuffer.getMappedPtrAs<UniformData>(); }

    /// Record a pipeline barrier to ensure that the uniform buffer is up-to-date
    /// and valid before shaders access it. This must be included when setting
    /// up command buffers, or uniform data will not work. Additionally, this 
    /// call **cannot** be made during a dynamic rendering pass, and must be done
    /// beforehand instead. Don't ask me why, I don't get it either. 
    void recUniformBufferHostBarrier(vk::CommandBuffer aCmdBuffer);

    /// Enable multisampling using the provided standard multisampling count. Multisampling is off by default,
    /// and setting this to `e1` disables multisampling. 
    ///
    /// This only changes the behavior of `recStandardState()`. Making it correctly set dynamic state so that 
    /// rendering is configured for multisampling rendering with the given sample count. 
    void setMultisamplingCount(vk::SampleCountFlagBits aSampleCount) { mSampleCount = aSampleCount; }
    
    // Lower level calls for hacking in more custom behavior.
    void recStandardState(vk::CommandBuffer aCmdBuffer);
    void recPushDescriptorSet(vk::CommandBuffer aCmdBuffer);
    void recSetVertexInputs(vk::CommandBuffer aCmdBuffer);
    void recConfigureShading(vk::CommandBuffer aCmdBuffer, const DrawMode aMode, const ShadeMode aShading);

    /// Record necessary setup and state commands prior to drawing. 
    ///
    /// NOTE: Equivalent to calling the following lower level calls in order:
    ///       * `recStandardState(aCmdBuffer)`
    ///       * `recPushDescriptorSet(aCmdBuffer)`
    ///       * `recSetVertexInputs(aCmdBuffer)`
    void recPreDraw(vk::CommandBuffer aCmdBuffer);

    /// @brief Draw the given geometry object using the given mode and shading.
    /// @param aCmdBuffer Command buffer to record setup and draw commands into
    /// @param aGeo Geometry object to draw
    /// @param aMode 
    /// @param aShading
    /// @param aPart [Optional] List of part indices within `aGeo.mPartSpans` which should.
    ///              be drawn. If not specified, all parts are drawn. 
    void recDrawGeo(
        vk::CommandBuffer aCmdBuffer,
        const VulkanClassicRasterGeo& aGeo,
        const DrawMode aMode,
        const ShadeMode aShading,
        const vk::ArrayProxy<uint32_t> aParts = {});

    /// @brief Record commands for an unindexed draw of the provided vertex buffer.
    void recDraw(
        vk::CommandBuffer aCmdBuffer,
        uint32_t aFirstVertex,
        uint32_t aVertexCount,
        const vk::Buffer& aVertexBuffer,
        const DrawMode aMode,
        const ShadeMode aShading);

    /// @brief Record commands for an indexed draw call with the provided vertex and index buffers.
    void recIndexedDraw(
        vk::CommandBuffer aCmdBuffer,
        uint32_t aFirstIndex,
        uint32_t aIndexCount,
        const vk::Buffer& aVertexBuffer,
        const vk::Buffer& aIndexBuffer,
        const DrawMode aMode,
        const ShadeMode aShading);

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return mDescriptorLayout.get(); }
    const std::vector<vk::PushConstantRange>& getPushConstantRanges() const { return mPushConstantRanges; }
    vk::PipelineLayout getPipelineLayout() const { return mPipelineLayout.get(); }

protected:
    vk::ShaderEXT mVertShader;
    vk::ShaderEXT mFragShader;

    vk::SampleCountFlagBits mSampleCount = vk::SampleCountFlagBits::e1;

    vk::UniqueDescriptorSetLayout mDescriptorLayout;
    std::vector<vk::PushConstantRange> mPushConstantRanges;
    vk::UniquePipelineLayout mPipelineLayout;

    vult::MappableBuffer mUniformBuffer;
    float mPointSize = 1.0f;

    vk::Device mDevice;
    VmaAllocator mAllocator = VK_NULL_HANDLE;
};
