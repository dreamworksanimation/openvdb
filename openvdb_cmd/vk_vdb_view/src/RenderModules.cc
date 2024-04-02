// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "RenderModules.h"
#include "vulkan/ClassicRaster.h"
#include <spv_shaders.h>

#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointCount.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tools/Prune.h>
#include <openvdb/tree/LeafManager.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/tools/PointScatter.h>
#include <openvdb/tree/LeafManager.h>
#include <openvdb/math/Operators.h>
#include <openvdb/util/logging.h>

#include <algorithm> // for std::min()
#include <cmath> // for std::abs(), std::fabs(), std::floor()
#include <limits>
#include <stdexcept>
#include <type_traits> // for std::is_const

using GVS = vult::GlobalVulkanRuntimeScope;

namespace openvdb_viewer {

namespace util {

/// Helper class used internally by processTypedGrid()
template<typename GridType, typename OpType, bool IsConst/*=false*/>
struct GridProcessor {
    static inline void call(OpType& op, openvdb::GridBase::Ptr grid) {
        op.template operator()<GridType>(openvdb::gridPtrCast<GridType>(grid));
    }
};

/// Helper class used internally by processTypedGrid()
template<typename GridType, typename OpType>
struct GridProcessor<GridType, OpType, /*IsConst=*/true> {
    static inline void call(OpType& op, openvdb::GridBase::ConstPtr grid) {
        op.template operator()<GridType>(openvdb::gridConstPtrCast<GridType>(grid));
    }
};


/// Helper function used internally by processTypedGrid()
template<typename GridType, typename OpType, typename GridPtrType>
inline void
doProcessTypedGrid(GridPtrType grid, OpType& op)
{
    GridProcessor<GridType, OpType,
        std::is_const<typename GridPtrType::element_type>::value>::call(op, grid);
}


////////////////////////////////////////


/// @brief Utility function that, given a generic grid pointer,
/// calls a functor on the fully-resolved grid
///
/// Usage:
/// @code
/// struct PruneOp {
///     template<typename GridT>
///     void operator()(typename GridT::Ptr grid) const { grid->tree()->prune(); }
/// };
///
/// processTypedGrid(myGridPtr, PruneOp());
/// @endcode
///
/// @return @c false if the grid type is unknown or unhandled.
template<typename GridPtrType, typename OpType>
bool
processTypedGrid(GridPtrType grid, OpType& op)
{
    using namespace openvdb;
    if (grid->template isType<BoolGrid>())        doProcessTypedGrid<BoolGrid>(grid, op);
    else if (grid->template isType<FloatGrid>())  doProcessTypedGrid<FloatGrid>(grid, op);
    else if (grid->template isType<DoubleGrid>()) doProcessTypedGrid<DoubleGrid>(grid, op);
    else if (grid->template isType<Int32Grid>())  doProcessTypedGrid<Int32Grid>(grid, op);
    else if (grid->template isType<Int64Grid>())  doProcessTypedGrid<Int64Grid>(grid, op);
    else if (grid->template isType<Vec3IGrid>())  doProcessTypedGrid<Vec3IGrid>(grid, op);
    else if (grid->template isType<Vec3SGrid>())  doProcessTypedGrid<Vec3SGrid>(grid, op);
    else if (grid->template isType<Vec3DGrid>())  doProcessTypedGrid<Vec3DGrid>(grid, op);
    else if (grid->template isType<points::PointDataGrid>()) {
        doProcessTypedGrid<points::PointDataGrid>(grid, op);
    }
    else return false;
    return true;
}


/// @brief Utility function that, given a generic grid pointer, calls
/// a functor on the fully-resolved grid, provided that the grid's
/// voxel values are scalars
template<typename GridPtrType, typename OpType>
bool
processTypedScalarGrid(GridPtrType grid, OpType& op)
{
    using namespace openvdb;
    if (grid->template isType<FloatGrid>())       doProcessTypedGrid<FloatGrid>(grid, op);
    else if (grid->template isType<DoubleGrid>()) doProcessTypedGrid<DoubleGrid>(grid, op);
    else if (grid->template isType<Int32Grid>())  doProcessTypedGrid<Int32Grid>(grid, op);
    else if (grid->template isType<Int64Grid>())  doProcessTypedGrid<Int64Grid>(grid, op);
    else return false;
    return true;
}


/// @brief Utility function that, given a generic grid pointer, calls
/// a functor on the fully-resolved grid, provided that the grid's
/// voxel values are scalars or PointIndex objects
template<typename GridPtrType, typename OpType>
bool
processTypedScalarOrPointDataGrid(GridPtrType grid, OpType& op)
{
    using namespace openvdb;
    if (processTypedScalarGrid(grid, op)) return true;
    if (grid->template isType<points::PointDataGrid>()) {
        doProcessTypedGrid<points::PointDataGrid>(grid, op);
        return true;
    }
    return false;
}


/// @brief Utility function that, given a generic grid pointer, calls
/// a functor on the fully-resolved grid, provided that the grid's
/// voxel values are vectors
template<typename GridPtrType, typename OpType>
bool
processTypedVectorGrid(GridPtrType grid, OpType& op)
{
    using namespace openvdb;
    if (grid->template isType<Vec3IGrid>())       doProcessTypedGrid<Vec3IGrid>(grid, op);
    else if (grid->template isType<Vec3SGrid>())  doProcessTypedGrid<Vec3SGrid>(grid, op);
    else if (grid->template isType<Vec3DGrid>())  doProcessTypedGrid<Vec3DGrid>(grid, op);
    else return false;
    return true;
}

template<class TreeType>
class MinMaxVoxel
{
public:
    using LeafArray = openvdb::tree::LeafManager<TreeType>;
    using ValueType = typename TreeType::ValueType;

    // LeafArray = openvdb::tree::LeafManager<TreeType> leafs(myTree)
    MinMaxVoxel(LeafArray&);

    void runParallel();
    void runSerial();

    const ValueType& minVoxel() const { return mMin; }
    const ValueType& maxVoxel() const { return mMax; }

    inline MinMaxVoxel(const MinMaxVoxel<TreeType>&, tbb::split);
    inline void operator()(const tbb::blocked_range<size_t>&);
    inline void join(const MinMaxVoxel<TreeType>&);

private:
    LeafArray& mLeafArray;
    ValueType mMin, mMax;
};


template <class TreeType>
MinMaxVoxel<TreeType>::MinMaxVoxel(LeafArray& leafs)
    : mLeafArray(leafs)
    , mMin(std::numeric_limits<ValueType>::max())
    , mMax(std::numeric_limits<ValueType>::lowest())
{
}


template <class TreeType>
inline
MinMaxVoxel<TreeType>::MinMaxVoxel(const MinMaxVoxel<TreeType>& rhs, tbb::split)
    : mLeafArray(rhs.mLeafArray)
    , mMin(std::numeric_limits<ValueType>::max())
    , mMax(std::numeric_limits<ValueType>::lowest())
{
}


template <class TreeType>
void
MinMaxVoxel<TreeType>::runParallel()
{
    tbb::parallel_reduce(mLeafArray.getRange(), *this);
}


template <class TreeType>
void
MinMaxVoxel<TreeType>::runSerial()
{
    (*this)(mLeafArray.getRange());
}


template <class TreeType>
inline void
MinMaxVoxel<TreeType>::operator()(const tbb::blocked_range<size_t>& range)
{
    typename TreeType::LeafNodeType::ValueOnCIter iter;

    for (size_t n = range.begin(); n < range.end(); ++n) {
        iter = mLeafArray.leaf(n).cbeginValueOn();
        for (; iter; ++iter) {
            const ValueType value = iter.getValue();
            mMin = std::min(mMin, value);
            mMax = std::max(mMax, value);
        }
    }
}


template <class TreeType>
inline void
MinMaxVoxel<TreeType>::join(const MinMaxVoxel<TreeType>& rhs)
{
    mMin = std::min(mMin, rhs.mMin);
    mMax = std::max(mMax, rhs.mMax);
}

} // namespace util


////////////////////////////////////////


inline void verifyShaderLinked(GLuint program)
{
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_FALSE) return;

    GLint maxLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

    std::vector<GLchar> infoLog(maxLength);
    glGetProgramInfoLog(program, maxLength, &maxLength, &infoLog[0]);
    glDeleteProgram(program);

    std::ostringstream os;
    os << "Error: Unable to link shader program:\n\n" << infoLog.data();
    throw std::runtime_error(os.str());
}


// OglBufferObject

OglBufferObject::OglBufferObject():
    mVertexBuffer(0),
    mNormalBuffer(0),
    mIndexBuffer(0),
    mColorBuffer(0),
    mPrimType(GL_POINTS),
    mPrimNum(0)
{
}

OglBufferObject::~OglBufferObject() { clear(); }

void
OglBufferObject::render() const
{
    if (!glIsBuffer(mVertexBuffer)) {
        OPENVDB_LOG_DEBUG_RUNTIME("request to render empty or uninitialized buffer");
        return;
    }

    const bool usesColorBuffer = glIsBuffer(mColorBuffer);
    const bool usesNormalBuffer = glIsBuffer(mNormalBuffer);
    const bool usesIndices = glIsBuffer(mIndexBuffer);

    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, nullptr);

    if (usesColorBuffer) {
        glEnableClientState(GL_COLOR_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, mColorBuffer);
        glColorPointer(3, GL_FLOAT, 0, nullptr);
    }

    if (usesNormalBuffer) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, mNormalBuffer);
        glNormalPointer(GL_FLOAT, 0, nullptr);
    }

    if (usesIndices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
        glDrawElements(mPrimType, mPrimNum, GL_UNSIGNED_INT, nullptr);
    }
    else {
        glDrawArrays(mPrimType, 0, mPrimNum);
    }

    // disable client-side capabilities
    if (usesColorBuffer) glDisableClientState(GL_COLOR_ARRAY);
    if (usesNormalBuffer) glDisableClientState(GL_NORMAL_ARRAY);

    // release vbo's
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void
OglBufferObject::genIndexBuffer(const std::vector<GLuint>& v, GLenum primType)
{
    // clear old buffer
    if (glIsBuffer(mIndexBuffer) == GL_TRUE) glDeleteBuffers(1, &mIndexBuffer);

    // gen new buffer
    glGenBuffers(1, &mIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
    if (glIsBuffer(mIndexBuffer) == GL_FALSE) throw std::runtime_error("Error: Unable to create index buffer");

    // upload data
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        sizeof(GLuint) * v.size(), &v[0], GL_STATIC_DRAW); // upload data
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to upload index buffer data");

    // release buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    mPrimNum = GLsizei(v.size());
    mPrimType = primType;
}

void
OglBufferObject::genVertexBuffer(const std::vector<GLfloat>& v)
{
    assert((v.size() % 3) == 0);
    if (glIsBuffer(mVertexBuffer) == GL_TRUE) glDeleteBuffers(1, &mVertexBuffer);

    glGenBuffers(1, &mVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    if (glIsBuffer(mVertexBuffer) == GL_FALSE) throw std::runtime_error("Error: Unable to create vertex buffer");

    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * v.size(), v.data(), GL_STATIC_DRAW);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to upload vertex buffer data");

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // If no index buffer has been associated with this buffer object,
    // set the prim num to the number of verts to draw. If an index buffer
    // is set later, this value is overwritten
    if (!glIsBuffer(mIndexBuffer)) mPrimNum = GLsizei(v.size() / 3);
}

void
OglBufferObject::genNormalBuffer(const std::vector<GLfloat>& v)
{
    if (glIsBuffer(mNormalBuffer) == GL_TRUE) glDeleteBuffers(1, &mNormalBuffer);

    glGenBuffers(1, &mNormalBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, mNormalBuffer);
    if (glIsBuffer(mNormalBuffer) == GL_FALSE) throw std::runtime_error("Error: Unable to create normal buffer");

    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * v.size(), v.data(), GL_STATIC_DRAW);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to upload normal buffer data");

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void
OglBufferObject::genColorBuffer(const std::vector<GLfloat>& v)
{
    if (glIsBuffer(mColorBuffer) == GL_TRUE) glDeleteBuffers(1, &mColorBuffer);

    glGenBuffers(1, &mColorBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, mColorBuffer);
    if (glIsBuffer(mColorBuffer) == GL_FALSE) throw std::runtime_error("Error: Unable to create color buffer");

    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * v.size(), v.data(), GL_STATIC_DRAW);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to upload color buffer data");

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void
OglBufferObject::clear()
{
    if (glIsBuffer(mIndexBuffer) == GL_TRUE) glDeleteBuffers(1, &mIndexBuffer);
    if (glIsBuffer(mVertexBuffer) == GL_TRUE) glDeleteBuffers(1, &mVertexBuffer);
    if (glIsBuffer(mColorBuffer) == GL_TRUE) glDeleteBuffers(1, &mColorBuffer);
    if (glIsBuffer(mNormalBuffer) == GL_TRUE) glDeleteBuffers(1, &mNormalBuffer);

    mPrimType = GL_POINTS;
    mPrimNum = 0;
}


////////////////////////////////////////


OglShaderProgram::OglShaderProgram():
    mProgram(0),
    mVertShader(0),
    mFragShader(0)
{
}

OglShaderProgram::~OglShaderProgram() { clear(); }

void
OglShaderProgram::setVertShader(const std::string& s)
{
    mVertShader = glCreateShader(GL_VERTEX_SHADER);
    if (glIsShader(mVertShader) == GL_FALSE) throw std::runtime_error("Error: Unable to create shader program.");

    GLint length = GLint(s.length());
    const char *str = s.c_str();
    glShaderSource(mVertShader, 1, &str, &length);

    glCompileShader(mVertShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to compile vertex shader.");
}

void
OglShaderProgram::setFragShader(const std::string& s)
{
    mFragShader = glCreateShader(GL_FRAGMENT_SHADER);
    if (glIsShader(mFragShader) == GL_FALSE) throw std::runtime_error("Error: Unable to create shader program.");

    GLint length = GLint(s.length());
    const char *str = s.c_str();
    glShaderSource(mFragShader, 1, &str, &length);

    glCompileShader(mFragShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to compile fragment shader.");
}

void
OglShaderProgram::build()
{
    mProgram = glCreateProgram();
    if (glIsProgram(mProgram) == GL_FALSE) throw std::runtime_error("Error: Unable to create shader program.");

    if (glIsShader(mVertShader) == GL_TRUE) glAttachShader(mProgram, mVertShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to attach vertex shader.");

    if (glIsShader(mFragShader) == GL_TRUE) glAttachShader(mProgram, mFragShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to attach fragment shader.");

    glLinkProgram(mProgram);
    verifyShaderLinked(mProgram);
}

void
OglShaderProgram::build(const std::vector<GLchar*>& attributes)
{
    mProgram = glCreateProgram();
    if (glIsProgram(mProgram) == GL_FALSE) throw std::runtime_error("Error: Unable to create shader program.");

    for (GLuint n = 0, N = GLuint(attributes.size()); n < N; ++n) {
        glBindAttribLocation(mProgram, n, attributes[n]);
    }

    if (glIsShader(mVertShader) == GL_TRUE) glAttachShader(mProgram, mVertShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to attach vertex shader.");

    if (glIsShader(mFragShader) == GL_TRUE) glAttachShader(mProgram, mFragShader);
    if (GL_NO_ERROR != glGetError()) throw std::runtime_error("Error: Unable to attach fragment shader.");

    glLinkProgram(mProgram);
    verifyShaderLinked(mProgram);
}

void
OglShaderProgram::startShading() const
{
    if (glIsProgram(mProgram) == GL_FALSE) {
        throw std::runtime_error("Error: called startShading() on uncompiled shader program.");
    }
    glUseProgram(mProgram);
}

void
OglShaderProgram::stopShading() const
{
    glUseProgram(0);
}

void
OglShaderProgram::clear()
{
    GLsizei numShaders = 0;
    GLuint shaders[2] = { 0, 0 };

    glGetAttachedShaders(mProgram, 2, &numShaders, shaders);

    // detach and remove shaders
    for (GLsizei n = 0; n < numShaders; ++n) {

        glDetachShader(mProgram, shaders[n]);

        if (glIsShader(shaders[n]) == GL_TRUE) glDeleteShader(shaders[n]);
    }

    // remove program
    if (glIsProgram(mProgram)) glDeleteProgram(mProgram);
}


////////////////////////////////////////

// ViewportModule
const vk::DeviceSize ViewportModule::csVertexBufferSize = ViewportModule::ceNumVerts * sizeof(VulkanClassicRasterGeo::Vertex);

ViewportModule::ViewportModule()
: mAxisGnomonScale(1.5),
  mGroundPlaneScale(8.0)
{
}

ViewportModule::~ViewportModule() {
    if (mVulkanDidInit && mGnomonVS != VK_NULL_HANDLE) {
        GVS::getInstance()->getDevice().logical.destroyShaderEXT(mGnomonVS);
        mGnomonVS = VK_NULL_HANDLE;
    }
}


void
ViewportModule::render()
{
    if (!mIsVisible) return;

    /// @todo use VBO's

    // Ground plane
    glPushMatrix();
    glScalef(mGroundPlaneScale, mGroundPlaneScale, mGroundPlaneScale);
    glColor3d(0.6, 0.6, 0.6);

    OPENVDB_NO_FP_EQUALITY_WARNING_BEGIN

    float step = 0.125;
    for (float x = -1; x < 1.125; x+=step) {

        if (std::fabs(x) == 0.5 || std::fabs(x) == 0.0) {
            glLineWidth(1.5);
        } else {
            glLineWidth(1.0);
        }

        glBegin(GL_LINES);
        glVertex3f(x, 0, 1);
        glVertex3f(x, 0, -1);
        glVertex3f(1, 0, x);
        glVertex3f(-1, 0, x);
        glEnd();
    }

    OPENVDB_NO_FP_EQUALITY_WARNING_END


    glPopMatrix();

    // Axis gnomon
    GLfloat modelview[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, &modelview[0]);

    // Stash current viewport settings.
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, &viewport[0]);

    GLint w = std::min(std::max(viewport[2] / 20, viewport[3] / 20), std::min(viewport[2], viewport[3]));
    glViewport(0, 0, w, w);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();

    glOrtho(-1.0, 1.0, -1.0, 1.0, 0.0, 10.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();


    GLfloat campos[3] = { modelview[2], modelview[6], modelview[10] };
    GLfloat up[3] = { modelview[1], modelview[5], modelview[9] };

    gluLookAt(campos[0], campos[1], campos[2], 0.0, 0.0, 0.0, up[0], up[1], up[2]);

    glScalef(mAxisGnomonScale, mAxisGnomonScale, mAxisGnomonScale);

    glLineWidth(1.0);

    glBegin(GL_LINES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(0, 0, 0);
    glVertex3f(1, 0, 0);

    glColor3f(0.0f, 1.0f, 0.0f );
    glVertex3f(0, 0, 0);
    glVertex3f(0, 1, 0);

    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(0, 0, 0);
    glVertex3f(0, 0, 1);
    glEnd();

    glLineWidth(1.0);

    // reset viewport
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

void ViewportModule::recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) {
    VulkanClassicRasterEngine& rasterEngine = *VulkanClassicRasterEngine::getInstance();

    if (!mVulkanDidInit) {
        initVulkanData();
    }

    aCmdBuffer.beginRendering(aRenderInfo);
    rasterEngine.recPreDraw(aCmdBuffer);

    aCmdBuffer.setLineWidth(1.0f);

    // Draw viewport geometry part #0: ground plane grid
    rasterEngine.recDrawGeo(aCmdBuffer, mViewportGeo,
        VulkanClassicRasterEngine::DRAW_MODE_LINE_LIST,
        VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR, {0});

    const float gnomonViewWidth = std::min(std::max(mViewport.width / 20.0f, mViewport.height / 20.0f), std::min(mViewport.width, mViewport.height));
    const vk::Viewport gnomonViewport = vk::Viewport(mViewport)
        .setX(mViewport.x)
        .setY(mViewport.y + (mViewport.height - gnomonViewWidth))
        .setWidth(gnomonViewWidth).setHeight(gnomonViewWidth);

    aCmdBuffer.setViewportWithCount(gnomonViewport);

    // mGnomonVS.bind(aCmdBuffer);
    aCmdBuffer.bindShadersEXT(vk::ShaderStageFlagBits::eVertex, mGnomonVS);

    // Draw viewport geometry part #1: Gnomon
    // Requires specialized shader and viewport.
    rasterEngine.recDrawGeo(aCmdBuffer, mViewportGeo,
        VulkanClassicRasterEngine::DRAW_MODE_LINE_LIST,
        VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR, {1});

    aCmdBuffer.endRendering();
}

void ViewportModule::initVulkanData() {
    using Vertex = VulkanClassicRasterGeoBuilder::Vertex;

    VulkanClassicRasterGeoBuilder geoBuilder(ceNumVerts);

    const Vertex gridTemplateVert = Vertex {
        openvdb::Vec3s::zero(), // Position
        openvdb::Vec3s::zero(), // Normals are unused
        openvdb::Vec4s(0.4f) // Gray
    };

    float step = 0.125f;
    for (float x = -1; x < 1.125f; x += step) {
        geoBuilder.addVert(gridTemplateVert).position = openvdb::Vec3s(x, 0.0f, 1.0f);
        geoBuilder.addVert(gridTemplateVert).position = openvdb::Vec3s(x, 0.0f, -1.0f);
        geoBuilder.addVert(gridTemplateVert).position = openvdb::Vec3s(1.0f, 0.0f, x);
        geoBuilder.addVert(gridTemplateVert).position = openvdb::Vec3s(-1.0f, 0.0f, x);
    }

    for (Vertex& vert : geoBuilder.mVerts) {
        vert.position *= mGroundPlaneScale;
    }

    const static openvdb::Vec4s csRed = openvdb::Vec4s(1.0f, 0.0f, 0.0f, 1.0f);
    const static openvdb::Vec4s csGreen = openvdb::Vec4s(0.0f, 1.0f, 0.0f, 1.0f);
    const static openvdb::Vec4s csBlue = openvdb::Vec4s(0.0f, 0.0f, 1.0f, 1.0f);
    // Start gnomon
    geoBuilder.startNewPart();

    geoBuilder.addVert(Vertex()).color = csRed;
    geoBuilder.addVert(Vertex{openvdb::Vec3s(1.0f, 0.0f, 0.0f)}).color = csRed;
    geoBuilder.mVerts.back().position *= mAxisGnomonScale;

    geoBuilder.addVert(Vertex()).color = csGreen;
    geoBuilder.addVert(Vertex{openvdb::Vec3s(0.0f, 1.0f, 0.0f)}).color = csGreen;
    geoBuilder.mVerts.back().position *= mAxisGnomonScale;

    geoBuilder.addVert(Vertex()).color = csBlue;
    geoBuilder.addVert(Vertex{openvdb::Vec3s(0.0f, 0.0f, 1.0f)}).color = csBlue;
    geoBuilder.mVerts.back().position *= mAxisGnomonScale;
    // Finish gnomon

    mViewportGeo = geoBuilder.build();


    // Load gnomon shader
    VulkanClassicRasterEngine* rasterEngine = VulkanClassicRasterEngine::getInstance();
    vk::DescriptorSetLayout descSetLayout = rasterEngine->getDescriptorSetLayout();

    // Specialization info is used to compile variant of standard vertex shader which is specialized
    // for drawing the gnomon.
    const vk::SpecializationMapEntry scGnomonMode(0u, 0u, sizeof(vk::Bool32));
    const static vk::Bool32 scGnomonModeData = vk::Bool32(true);
    const vk::SpecializationInfo specInfo(1u, &scGnomonMode, sizeof(vk::Bool32), &scGnomonModeData);

    vk::ShaderCreateInfoEXT shaderCreateInfo(
        {}, vk::ShaderStageFlagBits::eVertex,
        vk::ShaderStageFlagBits::eFragment, vk::ShaderCodeTypeEXT::eSpirv);
    shaderCreateInfo
        .setCode<uint32_t>(sStandardVertexShader)
        .setPName("main")
        .setSetLayouts(descSetLayout)
        .setPushConstantRanges(rasterEngine->getPushConstantRanges())
        .setPSpecializationInfo(&specInfo);
    vk::Device device = GVS::getInstance()->getDevice().logical;
    const auto [r, shaderObj] = device.createShaderEXT(shaderCreateInfo);
    vk::resultCheck(r, "Shader compilation failed due to incompatible binary. "
                       "This should be impossible, and must be programmer or driver error!");
    mGnomonVS = shaderObj;

    mVulkanDidInit = true;
}

////////////////////////////////////////


class TreeTopologyOp
{
public:
    TreeTopologyOp(OglBufferObject& buffer) : mBuffer(&buffer) {}
    TreeTopologyOp(VulkanClassicRasterGeoBuilder& aGeoBuilder) : mGeoBuilder(&aGeoBuilder) {}

    template<typename GridType>
    void operator()(typename GridType::ConstPtr grid)
    {
        using openvdb::Index64;

        Index64 nodeCount = grid->tree().leafCount() + grid->tree().nonLeafCount();
        const Index64 N = nodeCount * 8 * 3;

        std::vector<GLfloat> points(N);
        std::vector<GLfloat> colors(N);
        std::vector<GLuint> indices(N);


        openvdb::Vec3d ptn;
        openvdb::Vec3s color;
        openvdb::CoordBBox bbox;
        Index64 pOffset = 0, iOffset = 0,  cOffset = 0, idx = 0;

        for (typename GridType::TreeType::NodeCIter iter = grid->tree().cbeginNode(); iter; ++iter)
        {
            iter.getBoundingBox(bbox);

            // Nodes are rendered as cell-centered
            const openvdb::Vec3d min(bbox.min().x()-0.5, bbox.min().y()-0.5, bbox.min().z()-0.5);
            const openvdb::Vec3d max(bbox.max().x()+0.5, bbox.max().y()+0.5, bbox.max().z()+0.5);

            // corner 1
            ptn = grid->indexToWorld(min);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 2
            ptn = openvdb::Vec3d(min.x(), min.y(), max.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 3
            ptn = openvdb::Vec3d(max.x(), min.y(), max.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 4
            ptn = openvdb::Vec3d(max.x(), min.y(), min.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 5
            ptn = openvdb::Vec3d(min.x(), max.y(), min.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 6
            ptn = openvdb::Vec3d(min.x(), max.y(), max.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 7
            ptn = grid->indexToWorld(max);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);

            // corner 8
            ptn = openvdb::Vec3d(max.x(), max.y(), min.z());
            ptn = grid->indexToWorld(ptn);
            points[pOffset++] = static_cast<GLfloat>(ptn[0]);
            points[pOffset++] = static_cast<GLfloat>(ptn[1]);
            points[pOffset++] = static_cast<GLfloat>(ptn[2]);


            // edge 1
            indices[iOffset++] = GLuint(idx);
            indices[iOffset++] = GLuint(idx + 1);
            // edge 2
            indices[iOffset++] = GLuint(idx + 1);
            indices[iOffset++] = GLuint(idx + 2);
            // edge 3
            indices[iOffset++] = GLuint(idx + 2);
            indices[iOffset++] = GLuint(idx + 3);
            // edge 4
            indices[iOffset++] = GLuint(idx + 3);
            indices[iOffset++] = GLuint(idx);
            // edge 5
            indices[iOffset++] = GLuint(idx + 4);
            indices[iOffset++] = GLuint(idx + 5);
            // edge 6
            indices[iOffset++] = GLuint(idx + 5);
            indices[iOffset++] = GLuint(idx + 6);
            // edge 7
            indices[iOffset++] = GLuint(idx + 6);
            indices[iOffset++] = GLuint(idx + 7);
            // edge 8
            indices[iOffset++] = GLuint(idx + 7);
            indices[iOffset++] = GLuint(idx + 4);
            // edge 9
            indices[iOffset++] = GLuint(idx);
            indices[iOffset++] = GLuint(idx + 4);
            // edge 10
            indices[iOffset++] = GLuint(idx + 1);
            indices[iOffset++] = GLuint(idx + 5);
            // edge 11
            indices[iOffset++] = GLuint(idx + 2);
            indices[iOffset++] = GLuint(idx + 6);
            // edge 12
            indices[iOffset++] = GLuint(idx + 3);
            indices[iOffset++] = GLuint(idx + 7);

            // node vertex color
            const int level = iter.getLevel();
            color = sNodeColors[(level == 0) ? 3 : (level == 1) ? 2 : 1];

            for (Index64 n = 0; n < 8; ++n) {
                colors[cOffset++] = color[0];
                colors[cOffset++] = color[1];
                colors[cOffset++] = color[2];
            }

            idx += 8;
        } // end node iteration

        // gen buffers and upload data to GPU
        if (mBuffer != nullptr) {
            mBuffer->genVertexBuffer(points);
            mBuffer->genColorBuffer(colors);
            mBuffer->genIndexBuffer(indices, GL_LINES);
        }

        if (mGeoBuilder != nullptr) {
            // Vulkan gen buffers and upload data to GPU
            using openvdb::Vec3s, openvdb::Vec4s;
            assert(points.size() == colors.size() && points.size() % 3 == 0);

            // Transcribe into VulkanClassicRenderEngine vertex format. 
            mGeoBuilder->mVerts.assign(points.size() / 3, {});
            for (size_t i = 0; i < points.size(); i += 3) {
                mGeoBuilder->mVerts[i/3].position = Vec3s(&points[i]);
                mGeoBuilder->mVerts[i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
            }

            mGeoBuilder->mIndices = indices;
        }
    }

private:
    OglBufferObject *mBuffer = nullptr;
    VulkanClassicRasterGeoBuilder* mGeoBuilder = nullptr;

    static openvdb::Vec3s sNodeColors[];
}; // TreeTopologyOp


openvdb::Vec3s TreeTopologyOp::sNodeColors[] = {
    openvdb::Vec3s(0.045f, 0.045f, 0.045f),         // root
    openvdb::Vec3s(0.0432f, 0.33f, 0.0411023f),     // first internal node level
    openvdb::Vec3s(0.871f, 0.394f, 0.01916f),       // intermediate internal node levels
    openvdb::Vec3s(0.00608299f, 0.279541f, 0.625f)  // leaf nodes
};


////////////////////////////////////////

// Tree topology render module

TreeTopologyModule::TreeTopologyModule(const openvdb::GridBase::ConstPtr& grid, bool aVulkanMode):
    mGrid(grid),
    mIsInitialized(false),
    mVulkanMode(aVulkanMode)
{
    if (!mVulkanMode) {
        mShader.setVertShader(
            "#version 120\n"
            "void main() {\n"
            "gl_FrontColor = gl_Color;\n"
            "gl_Position = ftransform();\n"
            "gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;\n"
            "}\n");

        mShader.setFragShader(
            "#version 120\n"
            "void main() {\n"
                "gl_FragColor = gl_Color;}\n");

        mShader.build();
    }
}


void
TreeTopologyModule::init()
{
    mIsInitialized = true;

    // extract grid topology
    VulkanClassicRasterGeoBuilder builder;
    TreeTopologyOp drawTopology = mVulkanMode ?
        TreeTopologyOp(builder)
      : TreeTopologyOp(mOglBufferObject);

    if (!util::processTypedGrid(mGrid, drawTopology)) {
        OPENVDB_LOG_INFO("Ignoring unrecognized grid type"
            " during tree topology module initialization.");
    }

    if (mVulkanMode) mTopoGeo = builder.build();
}


void
TreeTopologyModule::render()
{
    if (!mIsVisible) return;
    if (!mIsInitialized) init();

    mShader.startShading();

    mOglBufferObject.render();

    mShader.stopShading();
}

void TreeTopologyModule::recRender(const vk::RenderingInfo &aRenderInfo, vk::CommandBuffer aCmdBuffer) {
    VulkanClassicRasterEngine& rasterEngine = *VulkanClassicRasterEngine::getInstance();

    if (!mIsInitialized) init();
    if (mTopoGeo.isEmpty()) return;

    aCmdBuffer.beginRendering(aRenderInfo);
    rasterEngine.recPreDraw(aCmdBuffer);

    aCmdBuffer.setLineWidth(1.0f);

    rasterEngine.recDrawGeo(
        aCmdBuffer, mTopoGeo,
        VulkanClassicRasterEngine::DRAW_MODE_LINE_LIST,
        VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR);

    aCmdBuffer.endRendering();
}


////////////////////////////////////////


template<typename TreeType>
class PointGenerator
{
public:
    using LeafManagerType = openvdb::tree::LeafManager<TreeType>;

    PointGenerator(
        std::vector<GLfloat>& points,
        std::vector<GLuint>& indices,
        LeafManagerType& leafs,
        std::vector<size_t>& indexMap,
        const openvdb::math::Transform& transform,
        openvdb::Index64 voxelsPerLeaf = TreeType::LeafNodeType::NUM_VOXELS)
        : mPoints(points)
        , mIndices(indices)
        , mLeafs(leafs)
        , mIndexMap(indexMap)
        , mTransform(transform)
        , mVoxelsPerLeaf(voxelsPerLeaf)
    {
    }

    void runParallel()
    {
        tbb::parallel_for(mLeafs.getRange(), *this);
    }


    inline void operator()(const typename LeafManagerType::RangeType& range) const
    {
        using openvdb::Index64;

        using ValueOnCIter = typename TreeType::LeafNodeType::ValueOnCIter;

        openvdb::Vec3d pos;
        size_t index = 0;
        Index64 activeVoxels = 0;

        for (size_t n = range.begin(); n < range.end(); ++n) {

            index = mIndexMap[n];
            ValueOnCIter it = mLeafs.leaf(n).cbeginValueOn();

            activeVoxels = mLeafs.leaf(n).onVoxelCount();

            if (activeVoxels <= mVoxelsPerLeaf) {

                for ( ; it; ++it) {
                    pos = mTransform.indexToWorld(it.getCoord());
                    insertPoint(pos, index);
                    ++index;
                }

            } else if (1 == mVoxelsPerLeaf) {

                 pos = mTransform.indexToWorld(it.getCoord());
                 insertPoint(pos, index);

            } else {

                std::vector<openvdb::Coord> coords;
                coords.reserve(static_cast<size_t>(activeVoxels));
                for ( ; it; ++it) { coords.push_back(it.getCoord()); }

                pos = mTransform.indexToWorld(coords[0]);
                insertPoint(pos, index);
                ++index;

                pos = mTransform.indexToWorld(coords[static_cast<size_t>(activeVoxels-1)]);
                insertPoint(pos, index);
                ++index;

                Index64 r = Index64(std::floor(double(mVoxelsPerLeaf) / double(activeVoxels)));
                for (Index64 i = 1, I = mVoxelsPerLeaf - 2; i < I; ++i) {
                    pos = mTransform.indexToWorld(coords[static_cast<size_t>(i * r)]);
                    insertPoint(pos, index);
                    ++index;
                }
            }
        }
    }

private:
    void insertPoint(const openvdb::Vec3d& pos, size_t index) const
    {
        mIndices[index] = GLuint(index);
        const size_t element = index * 3;
        mPoints[element    ] = static_cast<GLfloat>(pos[0]);
        mPoints[element + 1] = static_cast<GLfloat>(pos[1]);
        mPoints[element + 2] = static_cast<GLfloat>(pos[2]);
    }

    std::vector<GLfloat>& mPoints;
    std::vector<GLuint>& mIndices;
    LeafManagerType& mLeafs;
    std::vector<size_t>& mIndexMap;
    const openvdb::math::Transform& mTransform;
    const openvdb::Index64 mVoxelsPerLeaf;
}; // PointGenerator


template<typename GridType>
class NormalGenerator
{
public:
    using AccessorType = typename GridType::ConstAccessor;
    using Grad = openvdb::math::ISGradient<openvdb::math::CD_2ND>;

    NormalGenerator(const AccessorType& acc): mAccessor(acc) {}

    NormalGenerator(const NormalGenerator&) = delete;
    NormalGenerator& operator=(const NormalGenerator&) = delete;

    void operator()(const openvdb::Coord& ijk, openvdb::Vec3d& normal)
    {
        openvdb::Vec3d v{Grad::result(mAccessor, ijk)};
        const double length = v.length();
        if (length > 1.0e-7) {
            v *= 1.0 / length;
            normal = v;
        }
    }

private:
    const AccessorType& mAccessor;
}; // class NormalGenerator

// Specialization for PointDataGrids, for which normals are not generated
template<>
class NormalGenerator<openvdb::points::PointDataGrid>
{
public:
    NormalGenerator(const openvdb::points::PointDataGrid::ConstAccessor&) {}
    NormalGenerator(const NormalGenerator&) = delete;
    NormalGenerator& operator=(const NormalGenerator&) = delete;
    void operator()(const openvdb::Coord&, openvdb::Vec3d&) {}
};


template<typename GridType>
class PointAttributeGenerator
{
public:
    using ValueType = typename GridType::ValueType;

    PointAttributeGenerator(
        std::vector<GLfloat>& points,
        std::vector<GLfloat>& colors,
        const GridType& grid,
        ValueType minValue,
        ValueType maxValue,
        openvdb::Vec3s (&colorMap)[4],
        bool isLevelSet = false)
        : mPoints(points)
        , mColors(colors)
        , mNormals(nullptr)
        , mGrid(grid)
        , mAccessor(grid.tree())
        , mMinValue(minValue)
        , mMaxValue(maxValue)
        , mColorMap(colorMap)
        , mIsLevelSet(isLevelSet)
        , mZeroValue(openvdb::zeroVal<ValueType>())
    {
        init();
    }

    PointAttributeGenerator(
        std::vector<GLfloat>& points,
        std::vector<GLfloat>& colors,
        std::vector<GLfloat>& normals,
        const GridType& grid,
        ValueType minValue,
        ValueType maxValue,
        openvdb::Vec3s (&colorMap)[4],
        bool isLevelSet = false)
        : mPoints(points)
        , mColors(colors)
        , mNormals(&normals)
        , mGrid(grid)
        , mAccessor(grid.tree())
        , mMinValue(minValue)
        , mMaxValue(maxValue)
        , mColorMap(colorMap)
        , mIsLevelSet(isLevelSet)
        , mZeroValue(openvdb::zeroVal<ValueType>())
    {
        init();
    }

    void runParallel()
    {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, (mPoints.size() / 3)), *this);
    }

    inline void operator()(const tbb::blocked_range<size_t>& range) const
    {
        openvdb::Coord ijk;
        openvdb::Vec3d pos, normal(0.0, -1.0, 0.0);
        openvdb::Vec3s color(0.9f, 0.3f, 0.3f);
        float w = 0.0;
        NormalGenerator<GridType> computeNormal{mAccessor};

        size_t e1, e2, e3, voxelNum = 0;
        for (size_t n = range.begin(); n < range.end(); ++n) {
            e1 = 3 * n;
            e2 = e1 + 1;
            e3 = e2 + 1;

            pos[0] = mPoints[e1];
            pos[1] = mPoints[e2];
            pos[2] = mPoints[e3];

            pos = mGrid.worldToIndex(pos);
            ijk[0] = int(pos[0]);
            ijk[1] = int(pos[1]);
            ijk[2] = int(pos[2]);

            const ValueType& value = mAccessor.getValue(ijk);

            if (value < mZeroValue) { // is negative
                if (mIsLevelSet) {
                    color = mColorMap[1];
                } else {
                    w = (float(value) - mOffset[1]) * mScale[1];
                    color = openvdb::Vec3s(w * mColorMap[0] + (1.0 - w) * mColorMap[1]);
                }
            } else {
                if (mIsLevelSet) {
                    color = mColorMap[2];
                } else {
                    w = (float(value) - mOffset[0]) * mScale[0];
                    color = openvdb::Vec3s(w * mColorMap[2] + (1.0 - w) * mColorMap[3]);
                }
            }

            mColors[e1] = color[0];
            mColors[e2] = color[1];
            mColors[e3] = color[2];

            if (mNormals) {
                if ((voxelNum % 2) == 0) { computeNormal(ijk, normal); }
                ++voxelNum;
                (*mNormals)[e1] = static_cast<GLfloat>(normal[0]);
                (*mNormals)[e2] = static_cast<GLfloat>(normal[1]);
                (*mNormals)[e3] = static_cast<GLfloat>(normal[2]);
            }
        }
    }

private:

    void init()
    {
        mOffset[0] = static_cast<float>(std::min(mZeroValue, mMinValue));
        mScale[0] = static_cast<float>(
            1.0 / (std::abs(float(std::max(mZeroValue, mMaxValue)) - mOffset[0])));
        mOffset[1] = static_cast<float>(std::min(mZeroValue, mMinValue));
        mScale[1] = static_cast<float>(
            1.0 / (std::abs(float(std::max(mZeroValue, mMaxValue)) - mOffset[1])));
    }

    std::vector<GLfloat>& mPoints;
    std::vector<GLfloat>& mColors;
    std::vector<GLfloat>* mNormals;

    const GridType& mGrid;
    openvdb::tree::ValueAccessor<const typename GridType::TreeType> mAccessor;

    ValueType mMinValue, mMaxValue;
    openvdb::Vec3s (&mColorMap)[4];
    const bool mIsLevelSet;

    ValueType mZeroValue;
    float mOffset[2], mScale[2];
}; // PointAttributeGenerator


////////////////////////////////////////


class ActiveScalarValuesOp
{
public:
    ActiveScalarValuesOp(
        OglBufferObject& interiorBuffer, OglBufferObject& surfaceBuffer)
        : mInteriorBuffer(&interiorBuffer)
        , mSurfaceBuffer(&surfaceBuffer)
    {}

    explicit ActiveScalarValuesOp(VulkanClassicRasterGeoBuilder& aGeoBuilder) : mGeoBuilder(&aGeoBuilder) {}

    template<typename GridType>
    void operator()(typename GridType::ConstPtr grid)
    {
        using openvdb::Index64;

        const Index64 maxVoxelPoints = 26000000;

        openvdb::Vec3s colorMap[4];
        colorMap[0] = openvdb::Vec3s(0.3f, 0.9f, 0.3f); // green
        colorMap[1] = openvdb::Vec3s(0.9f, 0.3f, 0.3f); // red
        colorMap[2] = openvdb::Vec3s(0.9f, 0.9f, 0.3f); // yellow
        colorMap[3] = openvdb::Vec3s(0.3f, 0.3f, 0.9f); // blue

        //////////

        using ValueType = typename GridType::ValueType;
        using TreeType = typename GridType::TreeType;
        using BoolTreeT = typename TreeType::template ValueConverter<bool>::Type;

        const TreeType& tree = grid->tree();
        const bool isLevelSetGrid = grid->getGridClass() == openvdb::GRID_LEVEL_SET;

        ValueType minValue, maxValue;
        openvdb::tree::LeafManager<const TreeType> leafs(tree);

        {
            util::MinMaxVoxel<const TreeType> minmax(leafs);
            minmax.runParallel();
            minValue = minmax.minVoxel();
            maxValue = minmax.maxVoxel();
        }

        openvdb::Index64 voxelsPerLeaf = TreeType::LeafNodeType::NUM_VOXELS;

        if (!isLevelSetGrid) {

            typename BoolTreeT::Ptr interiorMask(new BoolTreeT(false));

            { // Generate Interior Points
                interiorMask->topologyUnion(tree);
                interiorMask->voxelizeActiveTiles();

                if (interiorMask->activeLeafVoxelCount() > maxVoxelPoints) {
                    voxelsPerLeaf = std::max<Index64>(1,
                        (maxVoxelPoints / interiorMask->leafCount()));
                }

                openvdb::tools::erodeActiveValues(*interiorMask, 2,
                    openvdb::tools::NN_FACE, openvdb::tools::IGNORE_TILES);
                openvdb::tools::pruneInactive(*interiorMask);

                openvdb::tree::LeafManager<BoolTreeT> maskleafs(*interiorMask);
                std::vector<size_t> indexMap(maskleafs.leafCount());
                size_t voxelCount = 0;
                for (Index64 l = 0, L = maskleafs.leafCount(); l < L; ++l) {
                    indexMap[l] = voxelCount;
                    voxelCount += std::min(maskleafs.leaf(l).onVoxelCount(), voxelsPerLeaf);
                }

                std::vector<GLfloat> points(voxelCount * 3), colors(voxelCount * 3);
                std::vector<GLuint> indices(voxelCount);

                PointGenerator<BoolTreeT> pointGen(
                    points, indices, maskleafs, indexMap, grid->transform(), voxelsPerLeaf);
                pointGen.runParallel();


                PointAttributeGenerator<GridType> attributeGen(
                    points, colors, *grid, minValue, maxValue, colorMap);
                attributeGen.runParallel();

                if (mInteriorBuffer != nullptr) {
                    // gen buffers and upload data to GPU
                    mInteriorBuffer->genVertexBuffer(points);
                    mInteriorBuffer->genColorBuffer(colors);
                    mInteriorBuffer->genIndexBuffer(indices, GL_POINTS);
                }
                if (mGeoBuilder != nullptr) {
                    using Vertex = VulkanClassicRasterGeo::Vertex;
                    using openvdb::Vec3s, openvdb::Vec4s;

                    assert(points.size() == colors.size() && points.size() % 3 == 0);

                    // Transcribe into VulkanClassicRenderEngine vertex format. 
                    mGeoBuilder->startNewPart();
                    const size_t vertOffset = mGeoBuilder->mVerts.size();
                    mGeoBuilder->mVerts.insert(mGeoBuilder->mVerts.end(), points.size() / 3, {});
                    for (size_t i = 0; i < points.size(); i += 3) {
                        mGeoBuilder->mVerts[vertOffset+i/3].position = Vec3s(&points[i]);
                        mGeoBuilder->mVerts[vertOffset+i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
                    }
                    // Write indices, applying as a relative offset it the builder already has existing vertices.
                    mGeoBuilder->mIndices.reserve(mGeoBuilder->mIndices.size() + indices.size());
                    for (const uint32_t idx : indices) {
                        mGeoBuilder->mIndices.push_back(vertOffset + idx);
                    }
                }
            }

            { // Generate Surface Points
                typename BoolTreeT::Ptr surfaceMask(new BoolTreeT(false));
                surfaceMask->topologyUnion(tree);
                surfaceMask->voxelizeActiveTiles();

                openvdb::tree::ValueAccessor<BoolTreeT> interiorAcc(*interiorMask);
                for (typename BoolTreeT::LeafIter leafIt = surfaceMask->beginLeaf();
                    leafIt; ++leafIt)
                {
                    const typename BoolTreeT::LeafNodeType* leaf =
                        interiorAcc.probeConstLeaf(leafIt->origin());
                    if (leaf) leafIt->topologyDifference(*leaf, false);
                }
                openvdb::tools::pruneInactive(*surfaceMask);

                openvdb::tree::LeafManager<BoolTreeT> maskleafs(*surfaceMask);
                std::vector<size_t> indexMap(maskleafs.leafCount());
                size_t voxelCount = 0;
                for (Index64 l = 0, L = maskleafs.leafCount(); l < L; ++l) {
                    indexMap[l] = voxelCount;
                    voxelCount += std::min(maskleafs.leaf(l).onVoxelCount(), voxelsPerLeaf);
                }

                std::vector<GLfloat>
                    points(voxelCount * 3),
                    colors(voxelCount * 3),
                    normals(voxelCount * 3);
                std::vector<GLuint> indices(voxelCount);

                PointGenerator<BoolTreeT> pointGen(
                    points, indices, maskleafs, indexMap, grid->transform(), voxelsPerLeaf);
                pointGen.runParallel();

                PointAttributeGenerator<GridType> attributeGen(
                    points, colors, normals, *grid, minValue, maxValue, colorMap);
                attributeGen.runParallel();

                if (mSurfaceBuffer != nullptr) {
                    mSurfaceBuffer->genVertexBuffer(points);
                    mSurfaceBuffer->genColorBuffer(colors);
                    mSurfaceBuffer->genNormalBuffer(normals);
                    mSurfaceBuffer->genIndexBuffer(indices, GL_POINTS);
                }

                if (mGeoBuilder != nullptr) {
                    using Vertex = VulkanClassicRasterGeo::Vertex;
                    using openvdb::Vec3s, openvdb::Vec4s;

                    assert(points.size() == normals.size() && points.size() == colors.size() && points.size() % 3 == 0);

                    // Transcribe into VulkanClassicRenderEngine vertex format. 
                    mGeoBuilder->startNewPart();
                    const size_t vertOffset = mGeoBuilder->mVerts.size();
                    mGeoBuilder->mVerts.insert(mGeoBuilder->mVerts.end(), points.size() / 3, {});
                    for (size_t i = 0; i < points.size(); i += 3) {
                        mGeoBuilder->mVerts[vertOffset+i/3].position = Vec3s(&points[i]);
                        mGeoBuilder->mVerts[vertOffset+i/3].normal = Vec3s(&normals[i]);
                        mGeoBuilder->mVerts[vertOffset+i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
                    }
                    // Write indices, applying as a relative offset it the builder already has existing vertices.
                    mGeoBuilder->mIndices.reserve(mGeoBuilder->mIndices.size() + indices.size());
                    for (const uint32_t idx : indices) {
                        mGeoBuilder->mIndices.push_back(vertOffset + idx);
                    }
                }
            }

            return;
        }

        // Level set rendering
        if (tree.activeLeafVoxelCount() > maxVoxelPoints) {
            voxelsPerLeaf = std::max<Index64>(1, (maxVoxelPoints / tree.leafCount()));
        }

        std::vector<size_t> indexMap(leafs.leafCount());
        size_t voxelCount = 0;
        for (Index64 l = 0, L = leafs.leafCount(); l < L; ++l) {
            indexMap[l] = voxelCount;
            voxelCount += std::min(leafs.leaf(l).onVoxelCount(), voxelsPerLeaf);
        }

        std::vector<GLfloat>
            points(voxelCount * 3),
            colors(voxelCount * 3),
            normals(voxelCount * 3);
        std::vector<GLuint> indices(voxelCount);

        PointGenerator<const TreeType> pointGen(
            points, indices, leafs, indexMap, grid->transform(), voxelsPerLeaf);
        pointGen.runParallel();

        PointAttributeGenerator<GridType> attributeGen(
            points, colors, normals, *grid, minValue, maxValue, colorMap, isLevelSetGrid);
        attributeGen.runParallel();

        if (mSurfaceBuffer != nullptr) {
            mSurfaceBuffer->genVertexBuffer(points);
            mSurfaceBuffer->genColorBuffer(colors);
            mSurfaceBuffer->genNormalBuffer(normals);
            mSurfaceBuffer->genIndexBuffer(indices, GL_POINTS);
        }

        if (mGeoBuilder != nullptr) {
            using Vertex = VulkanClassicRasterGeo::Vertex;
            using openvdb::Vec3s, openvdb::Vec4s;

            assert(points.size() == normals.size() && points.size() == colors.size() && points.size() % 3 == 0);

            // Transcribe into VulkanClassicRenderEngine vertex format. 
            mGeoBuilder->startNewPart();
            const size_t vertOffset = mGeoBuilder->mVerts.size();
            mGeoBuilder->mVerts.insert(mGeoBuilder->mVerts.end(), points.size() / 3, {});
            for (size_t i = 0; i < points.size(); i += 3) {
                mGeoBuilder->mVerts[vertOffset+i/3].position = Vec3s(&points[i]);
                mGeoBuilder->mVerts[vertOffset+i/3].normal = Vec3s(&normals[i]);
                mGeoBuilder->mVerts[vertOffset+i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
            }
            // Write indices, applying as a relative offset it the builder already has existing vertices.
            mGeoBuilder->mIndices.reserve(mGeoBuilder->mIndices.size() + indices.size());
            for (const uint32_t idx : indices) {
                mGeoBuilder->mIndices.push_back(vertOffset + idx);
            }
        }
    }

private:
    OglBufferObject *mInteriorBuffer = nullptr;
    OglBufferObject *mSurfaceBuffer = nullptr;
    VulkanClassicRasterGeoBuilder* mGeoBuilder = nullptr;
}; // ActiveScalarValuesOp


class ActiveVectorValuesOp
{
public:
    ActiveVectorValuesOp(OglBufferObject& vectorBuffer)
        : mVectorBuffer(&vectorBuffer)
    {}

    explicit ActiveVectorValuesOp(VulkanClassicRasterGeoBuilder& aGeoBuilder) : mGeoBuilder(&aGeoBuilder) {}

    template<typename GridType>
    void operator()(typename GridType::ConstPtr grid)
    {
        using openvdb::Index64;

        using ValueType = typename GridType::ValueType;
        using TreeType = typename GridType::TreeType;
        using BoolTreeT = typename TreeType::template ValueConverter<bool>::Type;


        const TreeType& tree = grid->tree();

        double length = openvdb::tools::minMax(tree).max().length();

        typename BoolTreeT::Ptr mask(new BoolTreeT(false));
        mask->topologyUnion(tree);
        mask->voxelizeActiveTiles();

        ///@todo thread and restructure.

        const Index64 voxelCount = mask->activeLeafVoxelCount();

        const Index64 pointCount = voxelCount * 2;
        std::vector<GLfloat> points(pointCount*3), colors(pointCount*3);
        std::vector<GLuint> indices(pointCount);

        openvdb::Coord ijk;
        openvdb::Vec3d pos, color;
        openvdb::tree::LeafManager<BoolTreeT> leafs(*mask);

        openvdb::tree::ValueAccessor<const TreeType> acc(tree);

        Index64 idx = 0, pt = 0, cc = 0;
        for (Index64 l = 0, L = leafs.leafCount(); l < L; ++l) {
            typename BoolTreeT::LeafNodeType::ValueOnIter iter = leafs.leaf(l).beginValueOn();
            for (; iter; ++iter) {
                ijk = iter.getCoord();
                ValueType vec = acc.getValue(ijk);

                pos = grid->indexToWorld(ijk);

                points[idx++] = static_cast<GLfloat>(pos[0]);
                points[idx++] = static_cast<GLfloat>(pos[1]);
                points[idx++] = static_cast<GLfloat>(pos[2]);

                indices[pt] = GLuint(pt);
                ++pt;
                indices[pt] = GLuint(pt);

                ++pt;
                double w = vec.length() / length;
                vec.normalize();
                pos += grid->voxelSize()[0] * 0.9 * vec;

                points[idx++] = static_cast<GLfloat>(pos[0]);
                points[idx++] = static_cast<GLfloat>(pos[1]);
                points[idx++] = static_cast<GLfloat>(pos[2]);


                color = w * openvdb::Vec3d(0.9, 0.3, 0.3)
                    + (1.0 - w) * openvdb::Vec3d(0.3, 0.3, 0.9);

                colors[cc++] = static_cast<GLfloat>(color[0] * 0.3);
                colors[cc++] = static_cast<GLfloat>(color[1] * 0.3);
                colors[cc++] = static_cast<GLfloat>(color[2] * 0.3);

                colors[cc++] = static_cast<GLfloat>(color[0]);
                colors[cc++] = static_cast<GLfloat>(color[1]);
                colors[cc++] = static_cast<GLfloat>(color[2]);
            }
        }

        if (mVectorBuffer != nullptr) {
            mVectorBuffer->genVertexBuffer(points);
            mVectorBuffer->genColorBuffer(colors);
            mVectorBuffer->genIndexBuffer(indices, GL_LINES);
        }

        if (mGeoBuilder != nullptr) {
            using Vertex = VulkanClassicRasterGeo::Vertex;
            using openvdb::Vec3s, openvdb::Vec4s;

            assert(points.size() == colors.size() && points.size() % 3 == 0);

            // Transcribe into VulkanClassicRenderEngine vertex format. 
            mGeoBuilder->startNewPart();
            const size_t vertOffset = mGeoBuilder->mVerts.size();
            mGeoBuilder->mVerts.insert(mGeoBuilder->mVerts.end(), points.size() / 3, {});
            for (size_t i = 0; i < points.size(); i += 3) {
                mGeoBuilder->mVerts[vertOffset+i/3].position = Vec3s(&points[i]);
                mGeoBuilder->mVerts[vertOffset+i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
            }
            // Write indices, applying as a relative offset it the builder already has existing vertices.
            mGeoBuilder->mIndices.reserve(mGeoBuilder->mIndices.size() + indices.size());
            for (const uint32_t idx : indices) {
                mGeoBuilder->mIndices.push_back(vertOffset + idx);
            }
        }
    }

private:
    OglBufferObject *mVectorBuffer = nullptr;
    VulkanClassicRasterGeoBuilder* mGeoBuilder = nullptr;

}; // ActiveVectorValuesOp


class PointDataOp
{
public:
    using GLfloatVec = std::vector<GLfloat>;
    using GLuintVec = std::vector<GLuint>;

private:
    struct VectorAttributeWrapper
    {
        using ValueType = openvdb::Vec3f;

        struct Handle
        {
            explicit Handle(VectorAttributeWrapper& attribute)
                : mValues(attribute.mValues) {}

            void set(openvdb::Index offset, openvdb::Index/*unused*/, const ValueType& value)
            {
                offset *= 3;
                for (int i = 0; i < 3; ++i, ++offset) { mValues[offset] = value[i]; }
            }

        private:
            GLfloatVec& mValues;
        }; // struct Handle

        explicit VectorAttributeWrapper(GLfloatVec& values)
            : mValues(values) {}

        void expand() {}
        void compact() {}
    private:
        GLfloatVec& mValues;
    }; // struct VectorAttributeWrapper

public:
    explicit PointDataOp(OglBufferObject& buffer) : mBuffer(&buffer) {}
    
    explicit PointDataOp(VulkanClassicRasterGeoBuilder& aGeoBuilder) : mGeoBuilder(&aGeoBuilder) {}

    template<typename GridType>
    void operator()(typename GridType::ConstPtr grid)
    {
        const typename GridType::TreeType& tree = grid->tree();

        // obtain cumulative point offsets and total points
        std::vector<openvdb::Index64> pointOffsets;
        const openvdb::Index64 total = openvdb::points::pointOffsets(pointOffsets, tree);

        GLfloatVec points(total * 3);
        VectorAttributeWrapper ptVecWrapper{points};
        openvdb::points::convertPointDataGridPosition(ptVecWrapper, *grid, pointOffsets, 0);

        // gen buffers and upload data to GPU
        if (mBuffer) mBuffer->genVertexBuffer(points);

        const auto leafIter = tree.cbeginLeaf();
        if (!leafIter) return;

        const size_t colorIdx = leafIter->attributeSet().find("Cd");
        if (colorIdx == openvdb::points::AttributeSet::INVALID_POS) return;

        GLfloatVec colors;
        const auto& colorArray = leafIter->constAttributeArray(colorIdx);
        if (colorArray.template hasValueType<openvdb::Vec3f>()) {
            colors.resize(points.size());
            VectorAttributeWrapper cdVecWrapper{colors};
            openvdb::points::convertPointDataGridAttribute(cdVecWrapper, tree, pointOffsets,
                /*startOffset=*/0, static_cast<unsigned>(colorIdx));

            // gen color buffer
            if (mBuffer) mBuffer->genColorBuffer(colors);
        }

        if (mGeoBuilder != nullptr) {
            using Vertex = VulkanClassicRasterGeo::Vertex;
            using openvdb::Vec3s, openvdb::Vec4s;

            assert(points.size() == colors.size() && points.size() % 3 == 0);

            // Transcribe into VulkanClassicRenderEngine vertex format. 
            mGeoBuilder->startNewPart();
            const size_t vertOffset = mGeoBuilder->mVerts.size();
            mGeoBuilder->mVerts.insert(mGeoBuilder->mVerts.end(), points.size() / 3, {});
            for (size_t i = 0; i < points.size(); i += 3) {
                mGeoBuilder->mVerts[vertOffset+i/3].position = Vec3s(&points[i]);
                mGeoBuilder->mVerts[vertOffset+i/3].color = Vec4s(colors[i], colors[i+1], colors[i+2], 1.0f);
            }
        }
    }

private:
    OglBufferObject* mBuffer = nullptr;
    VulkanClassicRasterGeoBuilder* mGeoBuilder = nullptr;
}; // PointDataOp


////////////////////////////////////////

// Active value render module

VoxelModule::VoxelModule(const openvdb::GridBase::ConstPtr& grid, bool aVulkanMode)
    : mGrid(grid)
    , mIsInitialized(false)
    , mDrawingPointGrid(false)
    , mVulkanMode(aVulkanMode)
{
    if (!mVulkanMode) {
        mFlatShader.setVertShader(
            "#version 120\n"
            "void main() {\n"
            "gl_PointSize = 1.0;\n" // only used for PointDataGrid shading, ignored otherwise
            "gl_FrontColor = gl_Color;\n"
            "gl_Position = ftransform();\n"
            "gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;\n"
            "}\n");

        mFlatShader.setFragShader(
            "#version 120\n"
            "void main() {\n"
                "gl_FragColor = gl_Color;}\n");

        mFlatShader.build();

        mSurfaceShader.setVertShader(
            "#version 120\n"
            "varying vec3 normal;\n"
            "void main() {\n"
                "gl_FrontColor = gl_Color;\n"
                "normal = normalize(gl_NormalMatrix * gl_Normal);\n"
                "gl_Position =  ftransform();\n"
                "gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;\n"
            "}\n");


        mSurfaceShader.setFragShader(
            "#version 120\n"
            "varying vec3 normal;\n"
            "void main() {\n"
                "vec3 normalized_normal = normalize(normal);\n"
                "float w = 0.5 * (1.0 + dot(normalized_normal, vec3(0.0, 1.0, 0.0)));\n"
                "vec4 diffuseColor = w * gl_Color + (1.0 - w) * (gl_Color * 0.3);\n"
                "gl_FragColor = diffuseColor;\n"
            "}\n");

        mSurfaceShader.build();
    }
}


void
VoxelModule::init()
{
    mIsInitialized = true;

    VulkanClassicRasterGeoBuilder builder;

    if (mGrid->isType<openvdb::points::PointDataGrid>()) {
        mDrawingPointGrid = true;
        mSurfaceBuffer.clear();
        PointDataOp drawPoints = mVulkanMode ?
            PointDataOp(builder)
          : PointDataOp(mInteriorBuffer);
        util::doProcessTypedGrid<openvdb::points::PointDataGrid>(mGrid, drawPoints);
    } else {
        mDrawingPointGrid = false;
        ActiveScalarValuesOp drawScalars = mVulkanMode ?
            ActiveScalarValuesOp(builder)
          : ActiveScalarValuesOp(mInteriorBuffer, mSurfaceBuffer);
        if (!util::processTypedScalarOrPointDataGrid(mGrid, drawScalars)) {
            ActiveVectorValuesOp drawVectors = mVulkanMode ?
                ActiveVectorValuesOp(builder)
              : ActiveVectorValuesOp(mVectorBuffer);
            if (!util::processTypedVectorGrid(mGrid, drawVectors)) {
                OPENVDB_LOG_INFO("Ignoring unrecognized grid type " << mGrid->type()
                    << " during active value module initialization.");
            }
        }
    }

    if (mVulkanMode) mUnifiedGeo = builder.build();
}


void
VoxelModule::render()
{
    if (!mIsVisible) return;
    if (!mIsInitialized) init();

    if (mDrawingPointGrid) {
        glEnable(GL_POINT_SMOOTH);
        glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
        mFlatShader.startShading();
            mInteriorBuffer.render();
        mFlatShader.stopShading();
        glDisable(GL_POINT_SMOOTH);
        glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
    }
    else {
        mFlatShader.startShading();
            mInteriorBuffer.render();
            mVectorBuffer.render();
        mFlatShader.stopShading();

        mSurfaceShader.startShading();
            mSurfaceBuffer.render();
        mSurfaceShader.stopShading();
    }
}

void VoxelModule::recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) {
    VulkanClassicRasterEngine& rasterEngine = *VulkanClassicRasterEngine::getInstance();

    if (!mIsInitialized) init();

    aCmdBuffer.beginRendering(aRenderInfo);
    rasterEngine.recPreDraw(aCmdBuffer);

    if (mDrawingPointGrid) {
        // Draw VDB points
        rasterEngine.setPointSize(2.0f);
        rasterEngine.recDrawGeo(
            aCmdBuffer, mUnifiedGeo,
            VulkanClassicRasterEngine::DRAW_MODE_POINT_LIST,
            VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR);
    } else {
        if (mUnifiedGeo.numParts() > 1) {
            // Draw unlit interior points
            rasterEngine.recDrawGeo(
                aCmdBuffer, mUnifiedGeo,
                VulkanClassicRasterEngine::DRAW_MODE_POINT_LIST,
                VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR,
                0u);
            
            // Draw diffuse shaded surface points. 
            rasterEngine.recDrawGeo(
                aCmdBuffer, mUnifiedGeo,
                VulkanClassicRasterEngine::DRAW_MODE_POINT_LIST,
                VulkanClassicRasterEngine::SHADE_MODE_DIFFUSE_COLOR_SURF,
                1u);
        } else {
            const bool isLevelSetGrid = mGrid->getGridClass() == openvdb::GRID_LEVEL_SET;
            
            // Draw unlit vector viz lines or fog vdb points
            const VulkanClassicRasterEngine::DrawMode mode = isLevelSetGrid ? 
                VulkanClassicRasterEngine::DRAW_MODE_POINT_LIST
              : VulkanClassicRasterEngine::DRAW_MODE_LINE_LIST;

            aCmdBuffer.setLineWidth(1.0f);
            rasterEngine.recDrawGeo(
                aCmdBuffer, mUnifiedGeo,
                mode,
                VulkanClassicRasterEngine::SHADE_MODE_UNLIT_COLOR);
        }
    }
    aCmdBuffer.endRendering();
}


////////////////////////////////////////


class MeshOp
{
public:
    MeshOp(OglBufferObject& buffer) : mBuffer(&buffer) {}
    MeshOp(VulkanClassicRasterGeoBuilder& aBuilder) : mGeoBuilder(&aBuilder) {}

    template<typename GridType>
    void operator()(typename GridType::ConstPtr grid)
    {
        using openvdb::Index64;

        // Special index value that restarts an ongoing primitive.
        // Used for more efficient quad splitting
        constexpr uint32_t cePrimRestart = UINT32_MAX;

        // Quads are a legacy OpenGL feature. We need to split them for Vulkan. 
        const bool splitQuads = mGeoBuilder != nullptr;

        openvdb::tools::VolumeToMesh mesher(
            grid->getGridClass() == openvdb::GRID_LEVEL_SET ? 0.0 : 0.01);
        mesher(*grid);

        // Copy points and generate point normals.
        std::vector<GLfloat> points(mesher.pointListSize() * 3);
        std::vector<GLfloat> normals(mesher.pointListSize() * 3);

        openvdb::tree::ValueAccessor<const typename GridType::TreeType> acc(grid->tree());
        openvdb::math::GenericMap map(grid->transform());
        openvdb::Coord ijk;

        for (Index64 n = 0, i = 0,  N = mesher.pointListSize(); n < N; ++n) {
            const openvdb::Vec3s& p = mesher.pointList()[n];
            points[i++] = p[0];
            points[i++] = p[1];
            points[i++] = p[2];
        }

        // Copy primitives
        openvdb::tools::PolygonPoolList& polygonPoolList = mesher.polygonPoolList();
        Index64 numQuads = 0;
        for (Index64 n = 0, N = mesher.polygonPoolListSize(); n < N; ++n) {
            numQuads += polygonPoolList[n].numQuads();
        }

        std::vector<GLuint> indices;
        indices.reserve(splitQuads ? numQuads * 5 : numQuads * 4);
        openvdb::Vec3d normal, e1, e2;

        for (Index64 n = 0, N = mesher.polygonPoolListSize(); n < N; ++n) {
            const openvdb::tools::PolygonPool& polygons = polygonPoolList[n];
            for (Index64 i = 0, I = polygons.numQuads(); i < I; ++i) {
                const openvdb::Vec4I& quad = polygons.quad(i);

                if (splitQuads) {
                    // Using triangle strip topology we draw a quad of clockwise verts 0,1,2,3
                    // as 0,1,3,2, and then insert a restart index to restart the strip before 
                    // the next quad. 
                    indices.push_back(quad[0]);
                    indices.push_back(quad[1]);
                    indices.push_back(quad[3]);
                    indices.push_back(quad[2]);
                    indices.push_back(cePrimRestart);
                } else {
                    indices.push_back(quad[0]);
                    indices.push_back(quad[1]);
                    indices.push_back(quad[2]);
                    indices.push_back(quad[3]);
                }

                e1 = mesher.pointList()[quad[1]];
                e1 -= mesher.pointList()[quad[0]];
                e2 = mesher.pointList()[quad[2]];
                e2 -= mesher.pointList()[quad[1]];
                normal = e1.cross(e2);

                const double length = normal.length();
                if (length > 1.0e-7) normal *= (1.0 / length);

                for (int v = 0; v < 4; ++v) {
                    normals[quad[v]*3]    = static_cast<GLfloat>(-normal[0]);
                    normals[quad[v]*3+1]  = static_cast<GLfloat>(-normal[1]);
                    normals[quad[v]*3+2]  = static_cast<GLfloat>(-normal[2]);
                }
            }
        }

        // Construct and transfer GPU buffers.
        if (mBuffer != nullptr) {
            mBuffer->genVertexBuffer(points);
            mBuffer->genNormalBuffer(normals);
            mBuffer->genIndexBuffer(indices, GL_QUADS);
        }

        if (mGeoBuilder) {
            using openvdb::Vec3s;
            assert(points.size() == normals.size() && points.size() % 3 == 0);

            // Transcribe into VulkanClassicRenderEngine vertex format. 
            mGeoBuilder->mVerts.assign(points.size() / 3, {});
            for (size_t i = 0; i < points.size(); i += 3) {
                mGeoBuilder->mVerts[i/3].position = Vec3s(&points[i]);
                mGeoBuilder->mVerts[i/3].normal = Vec3s(&normals[i]);
            }
            mGeoBuilder->mIndices = indices;
        }
    }

private:
    OglBufferObject *mBuffer = nullptr;
    VulkanClassicRasterGeoBuilder* mGeoBuilder = nullptr;

    static openvdb::Vec3s sNodeColors[];

}; // MeshOp


////////////////////////////////////////

// Meshing module

MeshModule::MeshModule(const openvdb::GridBase::ConstPtr& grid, bool aVulkanMode):
    mGrid(grid),
    mIsInitialized(false),
    mVulkanMode(aVulkanMode)
{
    if (!mVulkanMode) {
        mShader.setVertShader(
            "#version 120\n"
            "varying vec3 normal;\n"
            "void main() {\n"
                "normal = normalize(gl_NormalMatrix * gl_Normal);\n"
                "gl_Position =  ftransform();\n"
                "gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;\n"
            "}\n");

        mShader.setFragShader(
            "#version 120\n"
            "varying vec3 normal;\n"
            "const vec4 skyColor = vec4(0.9, 0.9, 1.0, 1.0);\n"
            "const vec4 groundColor = vec4(0.3, 0.3, 0.2, 1.0);\n"
            "void main() {\n"
                "vec3 normalized_normal = normalize(normal);\n"
                "float w = 0.5 * (1.0 + dot(normalized_normal, vec3(0.0, 1.0, 0.0)));\n"
                "vec4 diffuseColor = w * skyColor + (1.0 - w) * groundColor;\n"
                "gl_FragColor = diffuseColor;\n"
            "}\n");

        mShader.build();
    }
}

void
MeshModule::init()
{
    mIsInitialized = true;

    VulkanClassicRasterGeoBuilder builder;
    MeshOp drawMesh = mVulkanMode ?
        MeshOp(builder)
      : MeshOp(mOglBufferObject);

    if (!util::processTypedScalarGrid(mGrid, drawMesh)) {
        OPENVDB_LOG_INFO(
            "Ignoring non-scalar grid type during mesh module initialization.");
    }

    if (mVulkanMode) mMeshGeo = builder.build();
}


void
MeshModule::render()
{
    if (!mIsVisible) return;
    if (!mIsInitialized) init();

    mShader.startShading();

    mOglBufferObject.render();

    mShader.stopShading();
}

void MeshModule::recRender(const vk::RenderingInfo& aRenderInfo, vk::CommandBuffer aCmdBuffer) {
    VulkanClassicRasterEngine& rasterEngine = *VulkanClassicRasterEngine::getInstance();

    if (!mIsInitialized) init();
    if (mMeshGeo.isEmpty()) return; 

    aCmdBuffer.beginRendering(aRenderInfo);
    rasterEngine.recPreDraw(aCmdBuffer);

    rasterEngine.recDrawGeo(
        aCmdBuffer, mMeshGeo,
        VulkanClassicRasterEngine::DRAW_MODE_TRIANGLE_STRIP_FILL,
        VulkanClassicRasterEngine::SHADE_MODE_DIFFUSE_ISOSURF);

    aCmdBuffer.endRendering();
}

} // namespace openvdb_viewer
