// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#ifndef OPENVDB_VIEWER_RENDERMODULES_HAS_BEEN_INCLUDED
#define OPENVDB_VIEWER_RENDERMODULES_HAS_BEEN_INCLUDED

#include "vulkan/ClassicRaster.h"
#include <cstdint>
#include <openvdb/openvdb.h>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(MACOSX)
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#elif defined(_WIN32)
#include <GL/glew.h>
#else
#include <GL/gl.h>
#include <GL/glu.h>
#endif

namespace openvdb_viewer {

// OpenGL helper objects

class OglBufferObject
{
public:
    OglBufferObject();
    ~OglBufferObject();

    void render() const;

    /// @note accepted @c primType: GL_POINTS, GL_LINE_STRIP, GL_LINE_LOOP,
    /// GL_LINES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES,
    /// GL_QUAD_STRIP, GL_QUADS and GL_POLYGON
    void genIndexBuffer(const std::vector<GLuint>&, GLenum primType);

    void genVertexBuffer(const std::vector<GLfloat>&);
    void genNormalBuffer(const std::vector<GLfloat>&);
    void genColorBuffer(const std::vector<GLfloat>&);

    void clear();

private:
    GLuint mVertexBuffer, mNormalBuffer, mIndexBuffer, mColorBuffer;
    GLenum mPrimType;
    GLsizei mPrimNum;
};


class OglShaderProgram
{
public:
    OglShaderProgram();
    ~OglShaderProgram();

    void setVertShader(const std::string&);
    void setFragShader(const std::string&);

    void build();
    void build(const std::vector<GLchar*>& attributes);

    void startShading() const;
    void stopShading() const;

    void clear();

private:
    GLuint mProgram, mVertShader, mFragShader;
};


////////////////////////////////////////


/// @brief interface class
class RenderModule
{
public:
    virtual ~RenderModule() {}

    /// @brief OpenGL render
    virtual void render() = 0;
    /// @brief Vulkan render commands recording
    virtual void recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) = 0;

    bool visible() { return mIsVisible; }
    void setVisible(bool b) { mIsVisible = b; }

protected:
    RenderModule(): mIsVisible(true) {}

    bool mIsVisible;
};

////////////////////////////////////////


/// @brief Basic render module, axis gnomon and ground plane.
class ViewportModule : virtual public RenderModule
{
public:
    ViewportModule();
    virtual ~ViewportModule() override;

    void render() override;
    void recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) override;

    /// @brief Specify viewport dimensions. Necessary to draw gnomon correctly.
    void setViewport(const vk::Viewport& aViewport) { mViewport = aViewport; }

private:
    void initVulkanData(); 

    constexpr static uint32_t ceNumVerts = 4 * 17 + 6;
    const static vk::DeviceSize csVertexBufferSize;

    float mAxisGnomonScale, mGroundPlaneScale;

    vk::Viewport mViewport;
    vk::ShaderEXT mGnomonVS = VK_NULL_HANDLE;
    VulkanClassicRasterGeo mViewportGeo;
    bool mVulkanDidInit = false;
};


////////////////////////////////////////


/// @brief Tree topology render module
class TreeTopologyModule: public RenderModule
{
public:
    TreeTopologyModule(const openvdb::GridBase::ConstPtr&, bool aVulkanMode = false);
    ~TreeTopologyModule() override = default;

    virtual void render() override;
    virtual void recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) override;

private:
    void init();

    const openvdb::GridBase::ConstPtr& mGrid;
    OglBufferObject mOglBufferObject;
    bool mIsInitialized, mVulkanMode = false;
    OglShaderProgram mShader;

    VulkanClassicRasterGeo mTopoGeo;
};


////////////////////////////////////////


/// @brief Module to render active voxels as points
class VoxelModule: public RenderModule
{
public:
    VoxelModule(const openvdb::GridBase::ConstPtr&, bool aVulkanMode = false);
    ~VoxelModule() override = default;

    void render() override;
    virtual void recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) override;

private:
    void init();

    const openvdb::GridBase::ConstPtr& mGrid;
    OglBufferObject mInteriorBuffer, mSurfaceBuffer, mVectorBuffer;
    bool mIsInitialized, mVulkanMode = false;
    OglShaderProgram mFlatShader, mSurfaceShader;
    bool mDrawingPointGrid;

    VulkanClassicRasterGeo mUnifiedGeo;
};


////////////////////////////////////////


/// @brief Surfacing render module
class MeshModule: public RenderModule
{
public:
    MeshModule(const openvdb::GridBase::ConstPtr&, bool aVulkanMode = false);
    ~MeshModule() override = default;

    void render() override;
    virtual void recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) override;

private:
    void init();

    const openvdb::GridBase::ConstPtr& mGrid;
    OglBufferObject mOglBufferObject;
    bool mIsInitialized, mVulkanMode = false;
    OglShaderProgram mShader;

    VulkanClassicRasterGeo mMeshGeo;
};

} // namespace openvdb_viewer

#endif // OPENVDB_VIEWER_RENDERMODULES_HAS_BEEN_INCLUDED
