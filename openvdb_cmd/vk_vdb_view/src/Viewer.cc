// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "Viewer.h"

#include "vulkan/Utils.h"
#include "vulkan/GlfwVulkan.h"
#include "vulkan/ClassicRaster.h"
#include "vulkan/BitmapFont.h"
#include "Camera.h"
#include "ClipBox.h"
#include "Font.h"
#include "RenderModules.h"

#include <openvdb/util/Formats.h> // for formattedInt()
#include <openvdb/util/logging.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointCount.h>
#include <openvdb/version.h> // for OPENVDB_LIBRARY_MAJOR_VERSION, etc.

#include <stdlib.h>
#include <iomanip> // for std::setprecision()
#include <iostream>
#include <sstream>

#include <memory>
#include <vector>
#include <bitset>
#include <tuple>

#include <chrono>
#include <limits>

#include <thread>
#include <atomic>
#include <mutex>

#include <vma/vk_mem_alloc.h>

#define GLFW_INCLUDE_VULKAN
#define GLFW_VULKAN_STATIC
#include <GLFW/glfw3.h>

#ifndef STRIFY
	#define _STRIFY(_PD) #_PD
	#define STRIFY(_PD) _STRIFY(_PD)
#endif

using vult::DevicePair;
using vult::DeviceBundle;
using vult::QueueClosure;
using GVS = vult::GlobalVulkanRuntimeScope;

namespace openvdb_viewer {

class ViewerAbstractImpl
{
 public:
    virtual ~ViewerAbstractImpl() = default;

    virtual void init(const std::string& progName) = 0;

    virtual std::string getVersionString() const = 0;

    virtual bool isOpen() const = 0;
    virtual bool open(int width = DEFAULT_WIDTH, int height = DEFAULT_HEIGHT, uint32_t samples = 1u) = 0;
    virtual void view(const openvdb::GridCPtrVec&) = 0;
    virtual void handleEvents() = 0;
    virtual void close() = 0;

    virtual void resize(int width, int height) = 0;

    virtual void showPrevGrid() = 0;
    virtual void showNextGrid() = 0;

    virtual bool needsDisplay() = 0;
    virtual void setNeedsDisplay() = 0;

    virtual void toggleRenderModule(size_t n) = 0;
    virtual void toggleInfoText() = 0;

    // Internal
    virtual void render() = 0;
    virtual void interrupt() = 0;
    virtual void setWindowTitle(double fps = 0.0) = 0;
    virtual void showNthGrid(size_t n) = 0;
    virtual void updateCutPlanes(int wheelPos) = 0;
    virtual void swapBuffers() = 0;

    virtual void keyCallback(int key, int action) = 0;
    virtual void mouseButtonCallback(int button, int action) = 0;
    virtual void mousePosCallback(int x, int y) = 0;
    virtual void mouseWheelCallback(int pos) = 0;
    virtual void windowSizeCallback(int width, int height) = 0;
    virtual void windowRefreshCallback() = 0;

    static openvdb::BBoxd worldSpaceBBox(
        const openvdb::math::Transform&,
        const openvdb::CoordBBox&);

    static void sleep(double seconds);
};

class OpenGlViewerImpl : virtual public ViewerAbstractImpl
{
public:
    using CameraPtr = std::shared_ptr<Camera>;
    using ClipBoxPtr = std::shared_ptr<ClipBox>;
    using RenderModulePtr = std::shared_ptr<RenderModule>;

    OpenGlViewerImpl();
    virtual ~OpenGlViewerImpl() = default;

    virtual void init(const std::string& progName) override;

    virtual std::string getVersionString() const override;

    virtual bool isOpen() const override;
    virtual bool open(int width = DEFAULT_WIDTH, int height = DEFAULT_HEIGHT, uint32_t samples = 1u) override;
    virtual void view(const openvdb::GridCPtrVec&) override;
    virtual void handleEvents() override;
    virtual void close() override;

    virtual void resize(int width, int height) override;

    virtual void showPrevGrid() override;
    virtual void showNextGrid() override;

    virtual bool needsDisplay() override;
    virtual void setNeedsDisplay() override;

    virtual void toggleRenderModule(size_t n) override;
    virtual void toggleInfoText() override;

    // Internal
    virtual void render() override;
    virtual void interrupt() override;
    virtual void setWindowTitle(double fps = 0.0) override;
    virtual void showNthGrid(size_t n) override;
    virtual void updateCutPlanes(int wheelPos) override;
    virtual void swapBuffers() override;

    virtual void keyCallback(int key, int action) override;
    virtual void mouseButtonCallback(int button, int action) override;
    virtual void mousePosCallback(int x, int y) override;
    virtual void mouseWheelCallback(int pos) override;
    virtual void windowSizeCallback(int width, int height) override;
    virtual void windowRefreshCallback() override;

private:
    bool mDidInit;
    CameraPtr mCamera;
    ClipBoxPtr mClipBox;
    RenderModulePtr mViewportModule;
    std::vector<RenderModulePtr> mRenderModules;
    openvdb::GridCPtrVec mGrids;
    size_t mGridIdx, mUpdates;
    std::string mGridName, mProgName, mGridInfo, mTransformInfo, mTreeInfo;
    int mWheelPos;
    bool mShiftIsDown, mCtrlIsDown, mShowInfo;
    bool mInterrupt;
    GLFWwindow* mWindow;
}; // class OpenGlViewerImpl

/// @brief Vulkan backend viewer implementation. 
///
/// Implements `ViewerAbstractImpl` as well as `vult::VulkanRuntimeScope`, making this viewer the owner
/// and distributor of all Vulkan handles resources for the application. 
class VulkanViewerImpl : virtual public ViewerAbstractImpl, virtual protected vult::VulkanRuntimeScope
{
public:
    using CameraPtr = std::shared_ptr<Camera>;
    using ClipBoxPtr = std::shared_ptr<ClipBox>;
    using RenderModulePtr = std::shared_ptr<RenderModule>;

    using Clock = std::chrono::system_clock;
    using TimePt = Clock::time_point;

    VulkanViewerImpl();
    virtual ~VulkanViewerImpl() = default;

    virtual void init(const std::string& progName) override;

    virtual std::string getVersionString() const override;

    virtual bool isOpen() const override;
    virtual bool open(int width = DEFAULT_WIDTH, int height = DEFAULT_HEIGHT, uint32_t samples = 1u) override;
    virtual void view(const openvdb::GridCPtrVec&) override;
    virtual void handleEvents() override;
    virtual void close() override;

    virtual void resize(int width, int height) override;

    virtual void showPrevGrid() override;
    virtual void showNextGrid() override;

    virtual bool needsDisplay() override;
    virtual void setNeedsDisplay() override;

    virtual void toggleRenderModule(size_t n) override;
    virtual void toggleInfoText() override;

    // Internal
    virtual void render() override;
    virtual void interrupt() override;
    virtual void setWindowTitle(double fps = 0.0) override;
    virtual void showNthGrid(size_t n) override;
    virtual void updateCutPlanes(int wheelPos) override;
    virtual void swapBuffers() override;

    virtual void keyCallback(int key, int action) override;
    virtual void mouseButtonCallback(int button, int action) override;
    virtual void mousePosCallback(int x, int y) override;
    virtual void mouseWheelCallback(int pos) override;
    virtual void windowSizeCallback(int width, int height) override;
    virtual void windowRefreshCallback() override;

    virtual void toggleFullscreen();
protected:
    virtual vk::Instance getVulkanInstance() const override { return mVulkanInstance; }

    virtual bool hasDeviceBundle() const override { return mDevice.isValid(); }
    virtual const DeviceBundle& getDeviceBundle() const override { return mDevice; }

    virtual DevicePair getDevice() const override { return mDevice; }
    virtual VmaAllocator getAllocator() const override { return mMemAllocator; }

    virtual QueueClosure& getGraphicsQueueClosure() override { return mOmniQueue; }
    virtual const QueueClosure& getGraphicsQueueClosure() const override { return mOmniQueue; }

    virtual QueueClosure& getTransferQueueClosure() override { return mOmniQueue; }
    virtual const QueueClosure& getTransferQueueClosure() const override { return mOmniQueue; }

    virtual QueueClosure& getComputeQueueClosure() override { return mOmniQueue; }
    virtual const QueueClosure& getComputeQueueClosure() const override { return mOmniQueue; }

    virtual QueueClosure& getBigThreeQueueClosure() override { return mOmniQueue; }
    virtual const QueueClosure& getBigThreeQueueClosure() const override { return mOmniQueue; }

    virtual QueueClosure& getPresentationQueueClosure() override { return mOmniQueue; }
    virtual const QueueClosure& getPresentationQueueClosure() const override { return mOmniQueue; }

    virtual bool hasGraphicsQueueClosure() const override { return mOmniQueue.isValid(); }
    virtual bool hasTransferQueueClosure() const override { return mOmniQueue.isValid(); }
    virtual bool hasComputeQueueClosure() const override { return mOmniQueue.isValid(); }
    virtual bool hasBigThreeQueueClosure() const override { return mOmniQueue.isValid(); }
    virtual bool hasPresentationQueueClosure() const override { return mOmniQueue.isValid(); }

private:
    void recreateAndResetRender();

    bool mDidInit = false;

    ClipBoxPtr mClipBox = nullptr;
    CameraPtr mCamera = nullptr;

    std::shared_ptr<ViewportModule> mViewportModule = nullptr;
    std::vector<RenderModulePtr> mRenderModules;
    std::bitset<3> mModuleVisibility = 1u; // Defaults to first module being turned on.
    std::bitset<3> mModuleRecorded = 0u;

    openvdb::GridCPtrVec mGrids;
    size_t mGridIdx = 0, mUpdates = 0;
    std::string mGridName, mProgName, mGridInfo, mTransformInfo, mTreeInfo;
    bool mInterrupt = false;
    int mWheelPos = 0;
    bool mShiftIsDown = false, mCtrlIsDown = false, mShowInfo = true;

    //// Vulkan stuff below ////

    vk::Instance mVulkanInstance;
    DeviceBundle mDevice;
    VmaAllocator mMemAllocator;

    // Vulkan queue supporting graphics, compute, transfer, and present operations.
    QueueClosure mOmniQueue; // Queue instance
    
    vk::CommandPool mCommandPool;
    std::vector<std::vector<vk::CommandBuffer>> mRenderCommands;
    std::vector<vk::CommandBuffer> mInfoRenderCommands;
    bool mRenderCommandsReset = false, mModuleNeedsRecord = false, mVisibilityChanged = false;

    // GLFW windowing, Vulkan flavor
    std::shared_ptr<GlfwVulkanWindow> mGlfwVulkanWindow = nullptr;
    struct {int xpos, ypos, width, height;} mStashedWindowPosSize;
    TimePt mWindowDamageCooldown = TimePt::min();

}; // class VulkanViewerImpl


class ThreadManager
{
public:
    ThreadManager();

    void view(const openvdb::GridCPtrVec& gridList);
    void close();
    void resize(int width, int height);

private:
    void doView();
    static void* doViewTask(void* arg);

    std::atomic<bool> mRedisplay;
    bool mClose, mHasThread;
    std::thread mThread;
    openvdb::GridCPtrVec mGrids;
};


////////////////////////////////////////


namespace {

ViewerAbstractImpl* sViewer = nullptr;
ThreadManager* sThreadMgr = nullptr;
std::mutex sLock;


void
keyCB(GLFWwindow*, int key, int /*scancode*/, int action, int /*modifiers*/)
{
    if (sViewer) sViewer->keyCallback(key, action);
}


void
mouseButtonCB(GLFWwindow*, int button, int action, int /*modifiers*/)
{
    if (sViewer) sViewer->mouseButtonCallback(button, action);
}


void
mousePosCB(GLFWwindow*, double x, double y)
{
    if (sViewer) sViewer->mousePosCallback(int(x), int(y));
}


void
mouseWheelCB(GLFWwindow*, double /*xoffset*/, double yoffset)
{
    if (sViewer) sViewer->mouseWheelCallback(int(yoffset));
}


void
windowSizeCB(GLFWwindow*, int width, int height)
{
    if (sViewer) sViewer->windowSizeCallback(width, height);
}


void
windowRefreshCB(GLFWwindow*)
{
    if (sViewer) sViewer->windowRefreshCallback();
}

} // unnamed namespace


////////////////////////////////////////


Viewer
init(const std::string& progName, bool background, ViewerBackend aBackend)
{
    if (sViewer == nullptr) {
        std::lock_guard<std::mutex> lock(sLock);
        if (sViewer == nullptr) {
            OPENVDB_START_THREADSAFE_STATIC_WRITE
            if (aBackend == ViewerBackend::eOpenGL)
                sViewer = new OpenGlViewerImpl;
            else if (aBackend == ViewerBackend::eVulkan)
                sViewer = new VulkanViewerImpl;
            OPENVDB_FINISH_THREADSAFE_STATIC_WRITE
        }
    }
    const std::string backendStr = (aBackend == ViewerBackend::eVulkan) ? "(Vulkan) " : "(OpenGL) ";
    sViewer->init(backendStr + progName);
    

    if (background) {
        if (sThreadMgr == nullptr) {
            std::lock_guard<std::mutex> lock(sLock);
            if (sThreadMgr == nullptr) {
                OPENVDB_START_THREADSAFE_STATIC_WRITE
                sThreadMgr = new ThreadManager;
                OPENVDB_FINISH_THREADSAFE_STATIC_WRITE
            }
        }
    } else {
        if (sThreadMgr != nullptr) {
            std::lock_guard<std::mutex> lock(sLock);
            delete sThreadMgr;
            OPENVDB_START_THREADSAFE_STATIC_WRITE
            sThreadMgr = nullptr;
            OPENVDB_FINISH_THREADSAFE_STATIC_WRITE
        }
    }

    return Viewer();
}


void
exit()
{
    if (sThreadMgr) {
        sThreadMgr->close();
        delete sThreadMgr;
    } else if (sViewer) {
        sViewer->close();
        delete sViewer;
    }
    glfwTerminate();
}


////////////////////////////////////////


Viewer::Viewer()
{
    OPENVDB_LOG_DEBUG_RUNTIME("constructed Viewer from thread " << std::this_thread::get_id());
}


void
Viewer::open(int width, int height, uint32_t samples)
{
    if (sViewer) sViewer->open(width, height, samples);
}


void
Viewer::view(const openvdb::GridCPtrVec& grids)
{
    if (sThreadMgr) {
        sThreadMgr->view(grids);
    } else if (sViewer) {
        sViewer->view(grids);
    }
}


void
Viewer::handleEvents()
{
    if (sViewer) sViewer->handleEvents();
}


void
Viewer::close()
{
    if (sThreadMgr) sThreadMgr->close();
    else if (sViewer) sViewer->close();
}


void
Viewer::resize(int width, int height)
{
    if (sViewer) sViewer->resize(width, height);
}


std::string
Viewer::getVersionString() const
{
    std::string version;
    if (sViewer) version = sViewer->getVersionString();
    return version;
}


////////////////////////////////////////


ThreadManager::ThreadManager()
    : mClose(false)
    , mHasThread(false)
{
    mRedisplay = false;
}


void
ThreadManager::view(const openvdb::GridCPtrVec& gridList)
{
    if (!sViewer) return;

    mGrids = gridList;
    mClose = false;
    mRedisplay = true;

    if (!mHasThread) {
        mThread = std::thread(doViewTask, this);
        mHasThread = true;
    }
}


void
ThreadManager::close()
{
    if (!sViewer) return;

    // Tell the viewer thread to exit.
    mRedisplay = false;
    mClose = true;
    // Tell the viewer to terminate its event loop.
    sViewer->interrupt();

    if (mHasThread) {
        mThread.join();
        mHasThread = false;
    }

    // Tell the viewer to close its window.
    sViewer->close();
}


void
ThreadManager::doView()
{
    // This function runs in its own thread.
    // The mClose and mRedisplay flags are set from the main thread.
    while (!mClose) {
        // If mRedisplay was true, then set it to false
        // and then, if sViewer, call view:
        bool expected = true;
        if (mRedisplay.compare_exchange_strong(expected, false)) {
            if (sViewer) sViewer->view(mGrids);
        }
        sViewer->sleep(0.5/*sec*/);
    }
}


//static
void*
ThreadManager::doViewTask(void* arg)
{
    if (ThreadManager* self = static_cast<ThreadManager*>(arg)) {
        self->doView();
    }
    return nullptr;
}


////////////////////////////////////////


OpenGlViewerImpl::OpenGlViewerImpl()
    : mDidInit(false)
    , mCamera(new Camera)
    , mClipBox(new ClipBox)
    , mGridIdx(0)
    , mUpdates(0)
    , mWheelPos(0)
    , mShiftIsDown(false)
    , mCtrlIsDown(false)
    , mShowInfo(true)
    , mInterrupt(false)
    , mWindow(nullptr)
{
}


void
OpenGlViewerImpl::init(const std::string& progName)
{
    mProgName = progName;

    if (!mDidInit) {
        struct Local {
            static void errorCB(int error, const char* descr) {
                OPENVDB_LOG_ERROR("GLFW Error " << error << ": " << descr);
            }
        };
        glfwSetErrorCallback(Local::errorCB);
        if (glfwInit() == GL_TRUE) {
            OPENVDB_LOG_DEBUG_RUNTIME("initialized GLFW from thread "
                << std::this_thread::get_id());
            mDidInit = true;
        } else {
            OPENVDB_LOG_ERROR("GLFW initialization failed");
        }
    }
    mViewportModule.reset(new ViewportModule);
}


std::string
OpenGlViewerImpl::getVersionString() const
{
    std::ostringstream ostr;

    ostr << "OpenVDB: " <<
        openvdb::OPENVDB_LIBRARY_MAJOR_VERSION << "." <<
        openvdb::OPENVDB_LIBRARY_MINOR_VERSION << "." <<
        openvdb::OPENVDB_LIBRARY_PATCH_VERSION;

    int major, minor, rev;
    glfwGetVersion(&major, &minor, &rev);
    ostr << ", " << "GLFW: " << major << "." << minor << "." << rev;

    if (mDidInit) {
        ostr << ", " << "OpenGL: ";
        std::shared_ptr<GLFWwindow> wPtr;
        GLFWwindow* w = mWindow;
        if (!w) {
            wPtr.reset(glfwCreateWindow(100, 100, "", nullptr, nullptr), &glfwDestroyWindow);
            w = wPtr.get();
        }
        if (w) {
            ostr << glfwGetWindowAttrib(w, GLFW_CONTEXT_VERSION_MAJOR) << "."
                << glfwGetWindowAttrib(w, GLFW_CONTEXT_VERSION_MINOR) << "."
                << glfwGetWindowAttrib(w, GLFW_CONTEXT_REVISION);
        }
    }
    return ostr.str();
}


bool
OpenGlViewerImpl::open(int width, int height, uint32_t samples)
{
    if (mWindow == nullptr) {
        if (samples > 1) glfwWindowHint(GLFW_SAMPLES, samples);

        glfwWindowHint(GLFW_RED_BITS, 8);
        glfwWindowHint(GLFW_GREEN_BITS, 8);
        glfwWindowHint(GLFW_BLUE_BITS, 8);
        glfwWindowHint(GLFW_ALPHA_BITS, 8);
        glfwWindowHint(GLFW_DEPTH_BITS, 32);
        glfwWindowHint(GLFW_STENCIL_BITS, 0);

        mWindow = glfwCreateWindow(
            width, height, mProgName.c_str(), /*monitor=*/nullptr, /*share=*/nullptr);


        if (samples > 1) glEnable(GL_MULTISAMPLE);

        OPENVDB_LOG_DEBUG_RUNTIME("created window " << std::hex << mWindow << std::dec
            << " from thread " << std::this_thread::get_id());

        if (mWindow != nullptr) {
            // Temporarily make the new window the current context, then create a font.
            std::shared_ptr<GLFWwindow> curWindow(
                glfwGetCurrentContext(), glfwMakeContextCurrent);
            glfwMakeContextCurrent(mWindow);
            BitmapFont13::initialize();
        }
    }
    mCamera->setWindow(mWindow);

    if (mWindow != nullptr) {
        glfwSetKeyCallback(mWindow, keyCB);
        glfwSetMouseButtonCallback(mWindow, mouseButtonCB);
        glfwSetCursorPosCallback(mWindow, mousePosCB);
        glfwSetScrollCallback(mWindow, mouseWheelCB);
        glfwSetWindowSizeCallback(mWindow, windowSizeCB);
        glfwSetWindowRefreshCallback(mWindow, windowRefreshCB);
    }
    return (mWindow != nullptr);
}


bool
OpenGlViewerImpl::isOpen() const
{
    return (mWindow != nullptr);
}


// Set a flag so as to break out of the event loop on the next iteration.
// (Useful only if the event loop is running in a separate thread.)
void
OpenGlViewerImpl::interrupt()
{
    mInterrupt = true;
    if (mWindow) glfwSetWindowShouldClose(mWindow, true);
}


void
OpenGlViewerImpl::handleEvents()
{
    glfwPollEvents();
}


void
OpenGlViewerImpl::close()
{
    OPENVDB_LOG_DEBUG_RUNTIME("about to close window " << std::hex << mWindow << std::dec
        << " from thread " << std::this_thread::get_id());

    mViewportModule.reset();
    mRenderModules.clear();
    mCamera->setWindow(nullptr);
    GLFWwindow* win = mWindow;
    mWindow = nullptr;
    glfwDestroyWindow(win);
    OPENVDB_LOG_DEBUG_RUNTIME("destroyed window " << std::hex << win << std::dec
        << " from thread " << std::this_thread::get_id());
}


////////////////////////////////////////


void
OpenGlViewerImpl::view(const openvdb::GridCPtrVec& gridList)
{
    if (!isOpen()) return;

    mGrids = gridList;
    mGridIdx = size_t(-1);
    mGridName.clear();

    // Compute the combined bounding box of all the grids.
    openvdb::BBoxd bbox(openvdb::Vec3d(0.0), openvdb::Vec3d(0.0));
    if (!gridList.empty()) {
        bbox = worldSpaceBBox(
            gridList[0]->transform(), gridList[0]->evalActiveVoxelBoundingBox());
        openvdb::Vec3d voxelSize = gridList[0]->voxelSize();

        for (size_t n = 1; n < gridList.size(); ++n) {
            bbox.expand(worldSpaceBBox(gridList[n]->transform(),
                gridList[n]->evalActiveVoxelBoundingBox()));

            voxelSize = minComponent(voxelSize, gridList[n]->voxelSize());
        }
        mClipBox->setStepSize(voxelSize);
    }
    mClipBox->setBBox(bbox);

    // Prepare window for rendering.
    glfwMakeContextCurrent(mWindow);

#if defined(_WIN32)
    // This must come after glfwMakeContextCurrent
    if (GLEW_OK != glewInit()) {
        OPENVDB_LOG_ERROR("GLEW initialization failed");
    }
#endif

    {
        // set up camera
        openvdb::Vec3d extents = bbox.extents();
        double maxExtent = std::max(extents[0], std::max(extents[1], extents[2]));
        mCamera->setTarget(bbox.getCenter(), maxExtent);
        mCamera->setLookToTarget();
        mCamera->setSpeed();
    }

    swapBuffers();
    setNeedsDisplay();


    //////////
    glEnable(GL_FRAMEBUFFER_SRGB);

    // Screen color
    // glClearColor(0.85f, 0.85f, 0.85f, 0.0f);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);

    glPointSize(4);
    glLineWidth(2);
    //////////

    // construct render modules
    showNthGrid(/*n=*/0);


    // main loop

    size_t frame = 0;
    double time = glfwGetTime();
    double elapsed = 0.0;

    glfwSwapInterval(1);

    OPENVDB_LOG_DEBUG_RUNTIME("starting to render in window " << std::hex << mWindow << std::dec
        << " from thread " << std::this_thread::get_id());

    mInterrupt = false;
    for (bool stop = false; !stop; ) {
        handleEvents();
        
        const bool doRender = true; //needsDisplay();
        if (doRender) {
            time = glfwGetTime();
            render();
            // Swap front and back buffers
            swapBuffers();

            // eval fps
            elapsed += glfwGetTime() - time;
            ++frame;
            if (frame >= 60) {
                setWindowTitle(/*fps=*/double(frame) / elapsed);
                frame = 0;
                elapsed = 0.0;
                time = glfwGetTime();
            }
        }


        // Exit if the Esc key is pressed or the window is closed.
        stop = (mInterrupt || glfwWindowShouldClose(mWindow));
    }

    if (glfwGetCurrentContext() == mWindow) { ///< @todo not thread-safe
        // Detach this viewer's GL context.
        glfwMakeContextCurrent(nullptr);
        OPENVDB_LOG_DEBUG_RUNTIME("detached window " << std::hex << mWindow << std::dec
            << " from thread " << std::this_thread::get_id());
    }

    OPENVDB_LOG_DEBUG_RUNTIME("finished rendering in window " << std::hex << mWindow << std::dec
        << " from thread " << std::this_thread::get_id());
}


////////////////////////////////////////


void
OpenGlViewerImpl::resize(int width, int height)
{
    if (mWindow) glfwSetWindowSize(mWindow, width, height);
}


////////////////////////////////////////


void
OpenGlViewerImpl::render()
{
    if (mWindow == nullptr) return;

    // Prepare window for rendering.
    glfwMakeContextCurrent(mWindow);

    mCamera->aim();

    // draw scene
    mViewportModule->render(); // ground plane.

    mClipBox->render();
    mClipBox->enableClipping();

    for (size_t n = 0, N = mRenderModules.size(); n < N; ++n) {
        mRenderModules[n]->render();
    }

    mClipBox->disableClipping();

    // Render text

    if (mShowInfo) {
        BitmapFont13::enableFontRendering();

        glColor3d(0.8, 0.8, 0.8);

        int width, height;
        glfwGetFramebufferSize(mWindow, &width, &height);

        BitmapFont13::print(10, height - 13 - 10, mGridInfo);
        BitmapFont13::print(10, height - 13 - 30, mTransformInfo);
        BitmapFont13::print(10, height - 13 - 50, mTreeInfo);

        // Indicate via their hotkeys which render modules are enabled.
        std::string keys = "123";
        for (auto n: {0, 1, 2}) { if (!mRenderModules[n]->visible()) keys[n] = ' '; }
        BitmapFont13::print(width - 10 - 30, 10, keys);
        glColor3d(0.25, 0.25, 0.25);
        BitmapFont13::print(width - 10 - 30, 10, "123");

        BitmapFont13::disableFontRendering();
    }
}


////////////////////////////////////////


//static
void
ViewerAbstractImpl::sleep(double secs)
{
    using std::chrono::microseconds;
    const microseconds uduration(microseconds::rep(abs(secs) * microseconds::period::den));
    std::this_thread::sleep_for(uduration);
}


////////////////////////////////////////


//static
openvdb::BBoxd
ViewerAbstractImpl::worldSpaceBBox(const openvdb::math::Transform& xform, const openvdb::CoordBBox& bbox)
{
    openvdb::Vec3d pMin = openvdb::Vec3d(std::numeric_limits<double>::max());
    openvdb::Vec3d pMax = -pMin;

    const openvdb::Coord& min = bbox.min();
    const openvdb::Coord& max = bbox.max();
    openvdb::Coord ijk;

    // corner 1
    openvdb::Vec3d ptn = xform.indexToWorld(min);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 2
    ijk[0] = min.x();
    ijk[1] = min.y();
    ijk[2] = max.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 3
    ijk[0] = max.x();
    ijk[1] = min.y();
    ijk[2] = max.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 4
    ijk[0] = max.x();
    ijk[1] = min.y();
    ijk[2] = min.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 5
    ijk[0] = min.x();
    ijk[1] = max.y();
    ijk[2] = min.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 6
    ijk[0] = min.x();
    ijk[1] = max.y();
    ijk[2] = max.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }


    // corner 7
    ptn = xform.indexToWorld(max);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    // corner 8
    ijk[0] = max.x();
    ijk[1] = max.y();
    ijk[2] = min.z();
    ptn = xform.indexToWorld(ijk);
    for (int i = 0; i < 3; ++i) {
        if (ptn[i] < pMin[i]) pMin[i] = ptn[i];
        if (ptn[i] > pMax[i]) pMax[i] = ptn[i];
    }

    return openvdb::BBoxd(pMin, pMax);
}


////////////////////////////////////////


void
OpenGlViewerImpl::updateCutPlanes(int wheelPos)
{
    double speed = std::abs(mWheelPos - wheelPos);
    if (mWheelPos < wheelPos) mClipBox->update(speed);
    else mClipBox->update(-speed);
    setNeedsDisplay();
}


////////////////////////////////////////


void
OpenGlViewerImpl::swapBuffers()
{
    glfwSwapBuffers(mWindow);
}


////////////////////////////////////////


void
OpenGlViewerImpl::setWindowTitle(double fps)
{
    std::ostringstream ss;
    ss  << mProgName << ": "
        << (mGridName.empty() ? std::string("OpenVDB") : mGridName)
        << " (" << (mGridIdx + 1) << " of " << mGrids.size() << ") @ "
        << std::setprecision(1) << std::fixed << fps << " fps";
    if (mWindow) glfwSetWindowTitle(mWindow, ss.str().c_str());
}


////////////////////////////////////////


void
OpenGlViewerImpl::showPrevGrid()
{
    if (const size_t numGrids = mGrids.size()) {
        size_t idx = ((numGrids + mGridIdx) - 1) % numGrids;
        showNthGrid(idx);
    }
}


void
OpenGlViewerImpl::showNextGrid()
{
    if (const size_t numGrids = mGrids.size()) {
        size_t idx = (mGridIdx + 1) % numGrids;
        showNthGrid(idx);
    }
}


void
OpenGlViewerImpl::showNthGrid(size_t n)
{
    if (mGrids.empty()) return;
    n = n % mGrids.size();
    if (n == mGridIdx) return;

    mGridName = mGrids[n]->getName();
    mGridIdx = n;

    // save render settings
    std::vector<bool> active(mRenderModules.size());
    for (size_t i = 0, I = active.size(); i < I; ++i) {
        active[i] = mRenderModules[i]->visible();
    }

    mRenderModules.clear();
    mRenderModules.push_back(RenderModulePtr(new TreeTopologyModule(mGrids[n])));
    mRenderModules.push_back(RenderModulePtr(new MeshModule(mGrids[n])));
    mRenderModules.push_back(RenderModulePtr(new VoxelModule(mGrids[n])));

    if (active.empty()) {
        for (size_t i = 1, I = mRenderModules.size(); i < I; ++i) {
            mRenderModules[i]->setVisible(false);
        }
    } else {
        for (size_t i = 0, I = active.size(); i < I; ++i) {
            mRenderModules[i]->setVisible(active[i]);
        }
    }

    // Collect info
    {
        std::ostringstream ostrm;
        std::string s = mGrids[n]->getName();
        const openvdb::GridClass cls = mGrids[n]->getGridClass();
        if (!s.empty()) ostrm << s << " / ";
        ostrm << mGrids[n]->valueType() << " / ";
        if (cls == openvdb::GRID_UNKNOWN) ostrm << " class unknown";
        else ostrm << " " << openvdb::GridBase::gridClassToString(cls);
        mGridInfo = ostrm.str();
    }
    {
        openvdb::Coord dim = mGrids[n]->evalActiveVoxelDim();
        std::ostringstream ostrm;
        ostrm << dim[0] << " x " << dim[1] << " x " << dim[2]
            << " / voxel size " << std::setprecision(4) << mGrids[n]->voxelSize()[0]
            << " (" << mGrids[n]->transform().mapType() << ")";
        mTransformInfo = ostrm.str();
    }
    {
        std::ostringstream ostrm;
        const openvdb::Index64 count = mGrids[n]->activeVoxelCount();
        ostrm << openvdb::util::formattedInt(count)
            << " active voxel" << (count == 1 ? "" : "s");
        mTreeInfo = ostrm.str();
    }
    {
        if (mGrids[n]->isType<openvdb::points::PointDataGrid>()) {
            const openvdb::points::PointDataGrid::ConstPtr points =
                openvdb::gridConstPtrCast<openvdb::points::PointDataGrid>(mGrids[n]);
            const openvdb::Index64 count = openvdb::points::pointCount(points->tree());
            std::ostringstream ostrm;
            ostrm << " / " << openvdb::util::formattedInt(count)
                 << " point" << (count == 1 ? "" : "s");
            mTreeInfo.append(ostrm.str());
        }
    }

    setWindowTitle();
}


////////////////////////////////////////


void
OpenGlViewerImpl::keyCallback(int key, int action)
{
    mCamera->keyCallback(key, action);

    if (mWindow == nullptr) return;
    const bool keyPress = (glfwGetKey(mWindow, key) == GLFW_PRESS);
    /// @todo Should use "modifiers" argument to keyCB().
    mShiftIsDown = glfwGetKey(mWindow, GLFW_KEY_LEFT_SHIFT);
    mCtrlIsDown = glfwGetKey(mWindow, GLFW_KEY_LEFT_CONTROL);

    if (keyPress) {
        switch (key) {
        case '1': case GLFW_KEY_KP_1:
            toggleRenderModule(0);
            break;
        case '2': case GLFW_KEY_KP_2:
            toggleRenderModule(1);
            break;
        case '3': case GLFW_KEY_KP_3:
            toggleRenderModule(2);
            break;
        case 'c': case 'C':
            mClipBox->reset();
            break;
        case 'h': case 'H': // center home
            mCamera->setLookAtPoint(openvdb::Vec3d(0.0), 10.0);
            break;
        case 'g': case 'G': // center geometry
            mCamera->setLookToTarget();
            break;
        case 'i': case 'I':
            toggleInfoText();
            break;
        case GLFW_KEY_LEFT:
            showPrevGrid();
            break;
        case GLFW_KEY_RIGHT:
            showNextGrid();
            break;
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(mWindow, true);
            break;
        }
    }

    switch (key) {
    case 'x': case 'X':
        mClipBox->activateXPlanes() = keyPress;
        break;
    case 'y': case 'Y':
        mClipBox->activateYPlanes() = keyPress;
        break;
    case 'z': case 'Z':
        mClipBox->activateZPlanes() = keyPress;
        break;
    }

    mClipBox->shiftIsDown() = mShiftIsDown;
    mClipBox->ctrlIsDown() = mCtrlIsDown;

    setNeedsDisplay();
}


void
OpenGlViewerImpl::mouseButtonCallback(int button, int action)
{
    mCamera->mouseButtonCallback(button, action);
    mClipBox->mouseButtonCallback(button, action);
    if (mCamera->needsDisplay()) setNeedsDisplay();
}


void
OpenGlViewerImpl::mousePosCallback(int x, int y)
{
    bool handled = mClipBox->mousePosCallback(x, y);
    if (!handled) mCamera->mousePosCallback(x, y);
    if (mCamera->needsDisplay()) setNeedsDisplay();
}


void
OpenGlViewerImpl::mouseWheelCallback(int pos)
{
    pos += mWheelPos;
    if (mClipBox->isActive()) {
        updateCutPlanes(pos);
    } else {
        mCamera->mouseWheelCallback(pos, mWheelPos);
        if (mCamera->needsDisplay()) setNeedsDisplay();
    }

    mWheelPos = pos;
}


void
OpenGlViewerImpl::windowSizeCallback(int, int)
{
    setNeedsDisplay();
}


void
OpenGlViewerImpl::windowRefreshCallback()
{
    setNeedsDisplay();
}


////////////////////////////////////////


bool
OpenGlViewerImpl::needsDisplay()
{
    if (mUpdates < 2) {
        mUpdates += 1;
        return true;
    }
    return false;
}


void
OpenGlViewerImpl::setNeedsDisplay()
{
    mUpdates = 0;
}


void
OpenGlViewerImpl::toggleRenderModule(size_t n)
{
    mRenderModules[n]->setVisible(!mRenderModules[n]->visible());
}


void
OpenGlViewerImpl::toggleInfoText()
{
    mShowInfo = !mShowInfo;
}

//////////////////////////////////////////////////////



VulkanViewerImpl::VulkanViewerImpl() {
    mCamera.reset(new Camera());
    mClipBox.reset(new ClipBox());
}

void VulkanViewerImpl::init(const std::string& progName) {
    mProgName = progName;

    if (!mDidInit) {
        // Register error callback
        glfwSetErrorCallback([](int error, const char* descr) {
            OPENVDB_LOG_ERROR("GLFW Error " << error << ": " << descr);
        });

        // Basic GLFW init
        if (glfwInit() == true) {
            OPENVDB_LOG_DEBUG_RUNTIME("initialized GLFW from thread "
                << std::this_thread::get_id());
            mDidInit = true;
        } else {
            OPENVDB_LOG_ERROR("GLFW initialization failed");
        }

        // VULKAN INITIALIZATION
        /////////////////////////////////////////////////////////

        // First of three dispatcher inits. Loads the functions necessary for 
        // creating a Vulkan instance
        VULKAN_HPP_DEFAULT_DISPATCHER.init();

        // Initialize Vulkan instance
        {
            const uint32_t version = vk::makeApiVersion<uint32_t>(
                0, openvdb::OPENVDB_LIBRARY_MAJOR_VERSION,
                openvdb::OPENVDB_LIBRARY_MINOR_VERSION,
                openvdb::OPENVDB_LIBRARY_PATCH_VERSION);

            vk::ApplicationInfo appInfo("vdb_view", version, "vdb_view", version, vk::ApiVersion13);
            vk::InstanceCreateInfo createInfo(vk::InstanceCreateFlags(), &appInfo);

            // GLFW requires certain instance extensions, queried here.
            uint32_t glfwExtCount = 0;
            const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtCount);
            std::vector<const char*> instanceExtensions(glfwExtensions, std::next(glfwExtensions, glfwExtCount));
            createInfo.setPEnabledExtensionNames(instanceExtensions);

            // Enable shader object emulation layer if enabled in build and available on this system.
            std::vector<const char*> extraLayers;
            const auto detectedLayers = vk::enumerateInstanceLayerProperties();
            const bool hasShaderObjLayer = std::find_if(detectedLayers.begin(), detectedLayers.end(), [](const vk::LayerProperties& props) {
                return std::string(props.layerName) == "VK_LAYER_KHRONOS_shader_object";
            }) != detectedLayers.end();
            if (hasShaderObjLayer) {
                std::cout << "INFO: Enabling shader object emulation layer 'VK_LAYER_KHRONOS_shader_object'" << std::endl;
                extraLayers.push_back("VK_LAYER_KHRONOS_shader_object");
            }
            createInfo.setPEnabledLayerNames(extraLayers);

            // Create Instance
            mVulkanInstance = vk::createInstance(createInfo);

            // Second dynamic dispatch init. Loads functions from the new instance, 
            // making it possible to start querying for physical devices.
            VULKAN_HPP_DEFAULT_DISPATCHER.init(mVulkanInstance);

        }

        // Select a Vulkan physical device, and create a logical device instance for that physical device.
        {
            /* Device requirements:
               * Extensions
                 * VK_KHR_swapchain: required
                 * VK_EXT_vertex_input_dynamic_state: required
                 * VK_KHR_push_descriptor: required
                 * VK_EXT_shader_object: required (emulation ok)
               * Physical Device Features
                 * fullDrawIndexUint32: required
                 * dualSrcBlend: desired
                 * fillModeNonSolid: required
                 * wideLines: required
                 * largePoints: required
                 * VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT
                   * vertexInputDynamicState: required
                 * VkPhysicalDeviceVulkan12Features
                   * separateDepthStencilLayouts: required
                   * scalarBlockLayout: required
                   * shaderInt8: required
                   * storageBuffer8BitAccess: required
                   * uniformAndStorageBuffer8BitAccess: required
                 * VkPhysicalDeviceVulkan13Features
                   * synchronization2: required
                   * dynamicRendering: required
                   * maintenance4: required
               * Physical Device Properties
                 * pointSizeRange ⊇ [1, 4]
               * 1 Queue supporting graphics, compute, transfer, and presentation operations
            */

            std::array<std::string, 4> requiredExtensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
                VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                VK_EXT_SHADER_OBJECT_EXTENSION_NAME
            };
            std::sort(requiredExtensions.begin(), requiredExtensions.end());

            std::stringstream deviceFindStream;
            deviceFindStream << "Scanning for compatible Vulkan devices:" << std::endl;
            
            // Lambda verifying that a physical device supports the requirements documented above.
            // Used to filter the list of physical devices available on the system. The lambda also
            // populates `eligibleDeviceQueueFamily`, with one queue family index per-compatible device.
            // The queue family index is that of the queue family determined to be compatible. 
            std::vector<uint32_t> eligibleDeviceQueueFamily;
            const auto deviceEligibleFn = [this, &eligibleDeviceQueueFamily, &requiredExtensions, &deviceFindStream](const vk::PhysicalDevice& aPhysDev) -> bool {
                // Check device extension support
                deviceFindStream << "  Checking " << aPhysDev.getProperties().deviceName << ":" << std::endl;
                const std::vector<vk::ExtensionProperties> extensionProperties = aPhysDev.enumerateDeviceExtensionProperties();
                
                std::vector<std::string> extensions(extensionProperties.size(), std::string());
                std::transform(extensionProperties.cbegin(), extensionProperties.cend(), extensions.begin(), [](const auto& prop) { return std::string(prop.extensionName); });
                std::sort(extensions.begin(), extensions.end());
                std::vector<std::string> missingExtensions;
                std::set_difference(requiredExtensions.cbegin(), requiredExtensions.cend(), extensions.cbegin(), extensions.cend(), std::back_inserter(missingExtensions));
                
                const bool hasRequiredExtensions = missingExtensions.empty();
                if (!hasRequiredExtensions) {
                    deviceFindStream << "    Missing required extension(s):\n";
                    for (const std::string& ext : missingExtensions) {
                        deviceFindStream << "      " << ext << std::endl;
                    }
                }

                // Check device features
                const auto features2 = aPhysDev.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                             vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT,
                                                             vk::PhysicalDeviceVulkan12Features,
                                                             vk::PhysicalDeviceVulkan13Features>();
                const vk::PhysicalDeviceFeatures& features = features2.get().features;
                const bool supportsCoreFeatures = features.fullDrawIndexUint32 && features.dualSrcBlend &&
                                                  features.fillModeNonSolid && features.wideLines && features.largePoints;

                const vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT& vertexInputDynamicStateFeatures = features2.get<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>();
                const vk::PhysicalDeviceVulkan12Features& vulkan12Features = features2.get<vk::PhysicalDeviceVulkan12Features>();
                const vk::PhysicalDeviceVulkan13Features& vulkan13Features = features2.get<vk::PhysicalDeviceVulkan13Features>();
                const bool supportsVertexInputDynamicStateFeatures = vertexInputDynamicStateFeatures.vertexInputDynamicState;
                const bool supportsVulkan12Features = vulkan12Features.separateDepthStencilLayouts && vulkan12Features.uniformAndStorageBuffer8BitAccess &&
                                                      vulkan12Features.shaderInt8 && vulkan12Features.storageBuffer8BitAccess && vulkan12Features.scalarBlockLayout;
                const bool supportsVulkan13Features = vulkan13Features.synchronization2 && vulkan13Features.dynamicRendering && vulkan13Features.maintenance4;

                // Check device properties
                const auto props2 = aPhysDev.getProperties2<vk::PhysicalDeviceProperties2,
                                                            vk::PhysicalDeviceVulkan12Properties>();
                const vk::PhysicalDeviceLimits& limits = props2.get().properties.limits;
                const vk::PhysicalDeviceVulkan12Properties& vulkan12Props = props2.get<vk::PhysicalDeviceVulkan12Properties>();
                const bool hasCoreProperties = limits.pointSizeRange[0] <= 1.0f && limits.pointSizeRange[1] >= 4.0f;

                if (!supportsCoreFeatures) deviceFindStream << "    Missing one or more required Vulkan core features" << std::endl;
                if (!hasCoreProperties) deviceFindStream << "    Failed to meet requirements for one or more required Vulkan core properties" << std::endl;
                if (!supportsVertexInputDynamicStateFeatures) deviceFindStream << "    Missing support for vertex input dynamic state" << std::endl;
                if (!supportsVulkan13Features) deviceFindStream << "    Missing one or more required Vulkan 1.2 features" << std::endl;
                if (!supportsVulkan13Features) deviceFindStream << "    Missing one or more required Vulkan 1.3 features" << std::endl;

                // Retrieve a list of queue families supporting the big three capabilities, while also supporting presentation to via GLFW.
                const vk::QueueFlags requiredQueueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer;
                const auto queueOptions = vult::get_supported_queue_family_indices(aPhysDev, requiredQueueFlags, 1,
                    [&](uint32_t aFamilyIndex){
                        return glfwGetPhysicalDevicePresentationSupport(mVulkanInstance, aPhysDev, aFamilyIndex);
                });

                if (queueOptions.empty()) {
                    deviceFindStream << "    No queue found supporting graphics, compute, and transfer operations.\n\n";
                    return false;
                }
                eligibleDeviceQueueFamily.push_back(queueOptions[0]);

                deviceFindStream << "\n\n";
                
                return (hasRequiredExtensions && supportsCoreFeatures && hasCoreProperties && supportsVulkan12Features
                       && supportsVulkan13Features && supportsVertexInputDynamicStateFeatures);
            };

            // Retrieve list of compatible devices using the filtering function. They will be heuristically ordered from most to least suitable based on 
            // device type. 
            const std::vector<vk::PhysicalDevice> eligibleDevices = vult::get_filtered_and_ranked_physical_devices(mVulkanInstance, deviceEligibleFn);

            if (eligibleDevices.empty()) {
                OPENVDB_LOG_ERROR("No compatible Vulkan devices found on this system!");
                std::cerr << deviceFindStream.str() << std::endl;
                ::exit(2);
            }

            // There should be one eligible queue family index stashed away for each suitable physical device. 
            assert(eligibleDevices.size() == eligibleDeviceQueueFamily.size());

            vk::PhysicalDevice physical = eligibleDevices[0];
            OPENVDB_LOG_INFO(std::string("Selected Vulkan device: '") + physical.getProperties().deviceName +
                            "' and queue family [" + eligibleDeviceQueueFamily[0] + "]");

            // Configure and create a logical device to interface with the selected physical device.
            {
                // Queue creation is part of logical device creation. Create a single queue of the family previously selected.
                const float priority = 0.0f;
                vk::DeviceQueueCreateInfo queueCreateInfo(vk::DeviceQueueCreateFlags(0u), eligibleDeviceQueueFamily[0], 1u, &priority);

                // List of device level extensions to be requested during device creation
                std::vector<const char*> deviceExtensions(requiredExtensions.size(), nullptr);
                std::transform(requiredExtensions.cbegin(), requiredExtensions.cend(), deviceExtensions.begin(), [](const auto& ext) { return ext.c_str(); });
                // Linked list of Vulkan logical device creation information. Starts with basic device creation info, 
                // and is then linked to a series of Vulkan feature structures enabling the optional features 
                // needed by vdb_view. 
                vk::StructureChain<vk::DeviceCreateInfo,
                                vk::PhysicalDeviceFeatures2,
                                // vk::PhysicalDeviceRobustness2FeaturesEXT, // Nice to have & good for debugging, but not required to run
                                vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT,
                                vk::PhysicalDeviceVulkan12Features,
                                vk::PhysicalDeviceVulkan13Features,
                                vk::PhysicalDeviceShaderObjectFeaturesEXT> deviceCreateChain;
                
                deviceCreateChain.get()
                    .setQueueCreateInfos(queueCreateInfo)
                    .setPEnabledExtensionNames(deviceExtensions);
                deviceCreateChain.get<vk::PhysicalDeviceFeatures2>().features
                    .setRobustBufferAccess(true)
                    .setSampleRateShading(true)
                    .setFullDrawIndexUint32(true)
                    .setDualSrcBlend(true)
                    .setFillModeNonSolid(true)
                    .setWideLines(true)
                    .setLargePoints(true);
                // Nice to have & good for debugging, but not required to run
                // deviceCreateChain.get<vk::PhysicalDeviceRobustness2FeaturesEXT>()
                //     .setRobustBufferAccess2(true)
                //     .setRobustImageAccess2(true)
                //     .setNullDescriptor(true);
                deviceCreateChain.get<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>()
                    .setVertexInputDynamicState(true);
                deviceCreateChain.get<vk::PhysicalDeviceVulkan12Features>()
                    .setScalarBlockLayout(true)
                    .setSeparateDepthStencilLayouts(true)
                    .setShaderInt8(true)
                    .setStorageBuffer8BitAccess(true)
                    .setUniformAndStorageBuffer8BitAccess(true);
                deviceCreateChain.get<vk::PhysicalDeviceVulkan13Features>()
                    .setRobustImageAccess(true)
                    .setSynchronization2(true)
                    .setDynamicRendering(true)
                    .setMaintenance4(true);
                deviceCreateChain.get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().setShaderObject(true);

                // Create the logical device via the `vult::DeviceBundle` utility class, which will stash 
                // device creation information for later reference.
                mDevice = DeviceBundle(physical, deviceCreateChain);

                // Retrieve the queue vdb_view will use for all submitting all device commands.
                mOmniQueue = mDevice.retrieveQueueClosure(eligibleDeviceQueueFamily[0], 0u);
            }
        }

        // Final dynamic dispatcher init to load functions from the device. 
        VULKAN_HPP_DEFAULT_DISPATCHER.init(mVulkanInstance, mDevice.logical);

        // Finally, create and initialize other Vulkan objects and utilities.
        {
            // Create Vulkan memory allocator instance for the instance and device.
            // Used to efficiently allocate and sub-allocate device memory. 
            VmaAllocatorCreateInfo createInfo = {};
            {
                createInfo.instance = mVulkanInstance;
                createInfo.physicalDevice = mDevice.physical;
                createInfo.device = mDevice.logical;
                createInfo.vulkanApiVersion = mDevice.physical.getProperties().apiVersion;
            }

            VkResult r = vmaCreateAllocator(&createInfo, &mMemAllocator);
            if (r != VK_SUCCESS) throw std::runtime_error("Failed to create VMA memory allocator!");

            // Create command pool from which to allocate our rendering command buffers.
            mCommandPool = mDevice.logical.createCommandPool({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, mOmniQueue.queueFamily()});
        }

        // Register this viewer class as the global Vulkan scope, and the Vulkan scope for the 
        // rendering engines which are used to implement the Vulkan render modules. This broadly
        // exposes the Vulkan handles and state owned by this viewer to other files, without requiring
        // direct dependence and/or excessive passing through parameters.
        GVS::setScope(*this);
        VulkanClassicRasterEngine::setScope(*this);
        VulkanBitmapFont13Engine::setScope(*this);
    }

    mViewportModule.reset(new ViewportModule);
}

std::string VulkanViewerImpl::getVersionString() const {
    std::ostringstream ostr;

    int major, minor, rev;
    glfwGetVersion(&major, &minor, &rev);
    ostr << "GLFW: " << major << "." << minor << "." << rev;

    if (mDidInit) {
        ostr << "\n";
        const auto propChain = mDevice.physical.getProperties2<vk::PhysicalDeviceProperties2,
            vk::PhysicalDeviceVulkan11Properties,
            vk::PhysicalDeviceVulkan12Properties,
            vk::PhysicalDeviceVulkan13Properties>();
        const auto props = propChain.get<vk::PhysicalDeviceProperties2>().properties;
        const auto props11 = propChain.get<vk::PhysicalDeviceVulkan11Properties>();
        const auto props12 = propChain.get<vk::PhysicalDeviceVulkan12Properties>();
        const auto props13 = propChain.get<vk::PhysicalDeviceVulkan12Properties>();

        std::string conformanceVersion = std::to_string(props12.conformanceVersion.major) + '.'
            + std::to_string(props12.conformanceVersion.minor) + '.'
            + std::to_string(props12.conformanceVersion.subminor) + '.'
            + std::to_string(props12.conformanceVersion.patch);

        const auto formatUUID = [](const vk::ArrayWrapper1D<uint8_t, 16UL>& uuid) -> std::string {
            char buffer[38];
            snprintf(buffer, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], 
                    uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
            return std::string(buffer);
        };

        const auto formatApiVersion = [](uint32_t version) -> std::string {
            char buffer[128];
            if (VK_API_VERSION_VARIANT(version) == 0) {
                snprintf(buffer, 128, "%d.%d.%d", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
            } else {
                snprintf(buffer, 128, "%d.%d.%d - Variant %d", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version), VK_API_VERSION_VARIANT(version));
            }
            return std::string(buffer);
        };

        const auto formatNvidiaDriverVersion = [](uint32_t version) -> std::string {
            char buffer[128];
            const uint32_t major = version >> 22;
            const uint32_t minor = (version >> 14) & 0xFF;
            const uint32_t subminor = (version >> 6) & 0xFF;
            const uint32_t patch = version & 0x3F;
            snprintf(buffer, 128, "%d.%d.%d.%d", major, minor, subminor, patch);
            return std::string(buffer);
        };

        const char* usingShaderObjects = mDevice.extensionEnabled(VK_EXT_SHADER_OBJECT_EXTENSION_NAME) ? "true" : "false";

        ostr << "Vulkan: \n" <<
                "  API Version: " << formatApiVersion(props.apiVersion) << "\n" <<
                "  Conformance Version: " << conformanceVersion << "\n" <<
                "  Device Name: " << props.deviceName << "\n" <<
                "  Device ID: 0x" << std::hex << props.deviceID << std::dec << "\n" <<
                "  Vendor ID: 0x" << std::hex << props.vendorID << std::dec << "\n" <<
                "  Device UUID: " << formatUUID(props11.deviceUUID) << "\n" <<
                "  Driver ID: " << vk::to_string(props12.driverID) << "\n" <<
                "  Driver Version: " << formatNvidiaDriverVersion(props.driverVersion) << "\n" <<
                "  Driver UUID: " << formatUUID(props11.driverUUID) << "\n" <<
                "  VK_EXT_shader_object: " << usingShaderObjects << "\n";
                
    }
    return ostr.str();
}

bool VulkanViewerImpl::open(int width, int height, uint32_t samples) {
    // Verify MSAA sample count is supportable. 
    if (samples > 1u) {
        const vk::PhysicalDeviceLimits limits = mDevice.physical.getProperties().limits;
        uint32_t sampleLimit = uint32_t(limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts);
        if (!bool(sampleLimit & samples)) {
            // Bit-slide down to next supported value
            uint32_t s = samples >> 1u;
            while (!bool(s & sampleLimit) && s > 1u) s >>= 1u;

            std::cerr << "Requested MSAA sample count " << uint32_t(samples) << " not supported on this device. "
                    << "falling back to the next highest supported count: (" << s << "x)" << std::endl;

            samples = s;
        }
    }

    assert((samples & (samples-1)) == 0 && samples > 0u && samples <= 64u);

    if (mGlfwVulkanWindow == nullptr) {
        GlfwVulkanWindowBuilder builder(mVulkanInstance, mDevice.physical, mDevice.logical, mMemAllocator);
        builder
            .setDimensions(width, height)
            .setDepthBufferEnabled(true)
            .setPreferredSwapchainLength(3u)
            .setTitle(mProgName.c_str())
            .setSamplingCount(vk::SampleCountFlagBits(samples));
        
        assert(builder.isBuildReady());
        mGlfwVulkanWindow = std::make_shared<GlfwVulkanWindow>(std::move(builder.build()));
    }

    if (mGlfwVulkanWindow->isWindowOpen()) {
        GLFWwindow* window = mGlfwVulkanWindow->getWindow();
        mCamera->setWindow(window);

        glfwSetKeyCallback(window, keyCB);
        glfwSetMouseButtonCallback(window, mouseButtonCB);
        glfwSetCursorPosCallback(window, mousePosCB);
        glfwSetScrollCallback(window, mouseWheelCB);
        glfwSetWindowSizeCallback(window, windowSizeCB);
        glfwSetWindowRefreshCallback(window, windowRefreshCB);
    }

    return mGlfwVulkanWindow->isPresentable();
}

bool VulkanViewerImpl::isOpen() const {
    return mGlfwVulkanWindow != nullptr && mGlfwVulkanWindow->isWindowOpen();
}

void VulkanViewerImpl::interrupt() {
    mInterrupt = true;
    if (mGlfwVulkanWindow && mGlfwVulkanWindow->isWindowOpen()) glfwSetWindowShouldClose(mGlfwVulkanWindow->getWindow(), true);
}

void VulkanViewerImpl::handleEvents()
{
    glfwPollEvents();
}


void VulkanViewerImpl::close()
{
    // This whole function is dead code. Same with the OpenGL version. 
    OPENVDB_LOG_DEBUG_RUNTIME("about to close window " << std::hex << mGlfwVulkanWindow->getWindow() << std::dec
        << " from thread " << std::this_thread::get_id());
    
    mDevice.logical.waitIdle();

    mViewportModule.reset();
    mRenderModules.clear();

    mCamera->setWindow(nullptr);
    mGlfwVulkanWindow->close();

    mDevice.logical.destroyCommandPool(mCommandPool);
    mOmniQueue.reset();
    
    closeScope(); 

    vmaDestroyAllocator(mMemAllocator);
    mMemAllocator = VK_NULL_HANDLE;
    mDevice.logical.destroy();
    mDevice.reset();
    mVulkanInstance.destroy();
}

void VulkanViewerImpl::view(const openvdb::GridCPtrVec& gridList) {
    if (!isOpen()) return;

    mGrids = gridList;
    mGridIdx = size_t(-1);
    mGridName.clear();

    // Compute the combined bounding box of all the grids.
    openvdb::BBoxd bbox(openvdb::Vec3d(0.0), openvdb::Vec3d(0.0));
    if (!gridList.empty()) {
        bbox = worldSpaceBBox(
            gridList[0]->transform(), gridList[0]->evalActiveVoxelBoundingBox());
        openvdb::Vec3d voxelSize = gridList[0]->voxelSize();

        for (size_t n = 1; n < gridList.size(); ++n) {
            bbox.expand(worldSpaceBBox(gridList[n]->transform(),
                gridList[n]->evalActiveVoxelBoundingBox()));

            voxelSize = minComponent(voxelSize, gridList[n]->voxelSize());
        }
        mClipBox->setStepSize(voxelSize);
    }
    mClipBox->setBBox(bbox);

    {
        // set up camera
        openvdb::Vec3d extents = bbox.extents();
        double maxExtent = std::max(extents[0], std::max(extents[1], extents[2]));
        mCamera->setTarget(bbox.getCenter(), maxExtent);
        mCamera->setLookToTarget();
        mCamera->setSpeed();
    }

    setNeedsDisplay();
    showNthGrid(0);

    size_t frame = 0;
    double time = glfwGetTime();
    double elapsed = 0.0;

    bool stop = mInterrupt = false;
    while (!stop) {
        handleEvents();

        const bool doRender = true; //needsDisplay();
        if (doRender) {
            if (mRenderCommandsReset || mModuleNeedsRecord) {
                setWindowTitle(-1.0);
            }
            time = glfwGetTime();
            render();
            swapBuffers();
            
            // eval fps
            elapsed += glfwGetTime() - time;
            ++frame;
            if (frame > 60) {
                setWindowTitle(/*fps=*/double(frame) / elapsed);
                frame = 0;
                elapsed = 0.0;
                time = glfwGetTime();
            }
        }

        stop = (mInterrupt || glfwWindowShouldClose(mGlfwVulkanWindow->getWindow()));
    }
}

void VulkanViewerImpl::resize(int width, int height)
{
    if (mGlfwVulkanWindow->isWindowOpen()) glfwSetWindowSize(mGlfwVulkanWindow->getWindow(), width, height);
}

void VulkanViewerImpl::recreateAndResetRender() {
    mOmniQueue.getQueue().waitIdle();
    mDevice.logical.resetCommandPool(mCommandPool);
    mGlfwVulkanWindow->recreateRenderResources();
    mRenderCommandsReset = true;
}

void VulkanViewerImpl::render() {
    if (!mGlfwVulkanWindow || !mGlfwVulkanWindow->isPresentable()) return;

    if (mGlfwVulkanWindow->isSuboptimal()) {
        OPENVDB_LOG_INFO("Recreating suboptimal swapchain");
        recreateAndResetRender();
    }

    const openvdb::Mat4s MV = mCamera->getModelView();
    const openvdb::Mat4s P = mCamera->getProjection(/*aFlipY=*/true);
    VulkanClassicRasterEngine::getInstance()->setUniforms(MV, P);

    const bool isMultisampled = mGlfwVulkanWindow->isMultisampled();
    const vk::SampleCountFlagBits sampleCount = mGlfwVulkanWindow->multisampleCount();
    VulkanBitmapFont13Engine::getInstance()->setMultisamplingCount(sampleCount);
    VulkanClassicRasterEngine::getInstance()->setMultisamplingCount(sampleCount);
    VulkanClassicRasterEngine::getInstance()->setPointSize(4.0f);

    const bool isFirstCall = mRenderCommands.empty();

    // If render commands are not initialized, initialize them
    if (isFirstCall || mRenderCommandsReset || mModuleNeedsRecord || mVisibilityChanged) {
        if (mRenderCommands.empty()) mRenderCommands.resize(mGlfwVulkanWindow->numSwapchainImages());
        if (mInfoRenderCommands.empty()) mInfoRenderCommands.resize(mGlfwVulkanWindow->numSwapchainImages());

        const std::vector<vk::Image>& images = mGlfwVulkanWindow->getSwapchainImages();
        const std::vector<vk::ImageView>& imageViews = mGlfwVulkanWindow->getSwapchainImageViews();
        const vk::Extent2D attachmentExtent = mGlfwVulkanWindow->currentExtent();
        const vk::Viewport viewport(0u, 0u, float(attachmentExtent.width), float(attachmentExtent.height), 0.0f, 1.0f);
        
        const vk::ClearValue clearColor({0.0f, 0.0f, 0.0f, 0.0f});
        const vk::ClearValue clearDepth(vk::ClearDepthStencilValue(1.0f, 0.0f));
        
        // Setup several structures used together to define the essentials of the render pass.
        vk::RenderingAttachmentInfo colorAttachment = vk::RenderingAttachmentInfo()
            .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setClearValue(clearColor);
        vk::RenderingAttachmentInfo depthAttachment = vk::RenderingAttachmentInfo()
            .setImageLayout(vk::ImageLayout::eDepthAttachmentOptimal)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setClearValue(clearDepth);
        // The two structures above are incomplete at this point, as we will be re-using them, attaching
        // different images each time. The `RenderingInfo` struct below uses them by pointer reference, so 
        // we can update them without redefining the rendering info. 

        // The render pass uses `eSuspending`, allowing us to start rendering in this command buffer, and then resume later
        // with a different command buffer. This is being used to toggle different render modules on/off by including/excluding
        // their command buffers from the final render pass. 
        vk::RenderingInfo renderingInfo(
            {}, vk::Rect2D({0u, 0u}, attachmentExtent),
            1u, 0u, colorAttachment, &depthAttachment);
        
        // Record identical command buffers, one for each image in the swapchain. As strange as it may
        // seem, this is best practice. 
        for (size_t frameIdx = 0; frameIdx < images.size(); ++frameIdx) {

            // Command buffers, one for each render module + 1 special for UI rendering.
            std::vector<vk::CommandBuffer>& cmdBuffers = mRenderCommands[frameIdx];
            vk::CommandBuffer& uiRenderCommands = mInfoRenderCommands[frameIdx];

            // We can re-use command buffers after swapchain recreation, hence this check. 
            if (cmdBuffers.empty()) {
                // +1 for the viewport, +1 more for ensuring the framebuffer gets prepared for presentation, and +1 extra
                // to use for ui render commands.
                const uint32_t numBuffers = mRenderModules.size() + 3u;
                cmdBuffers = mDevice.logical.allocateCommandBuffers(
                    vk::CommandBufferAllocateInfo(mCommandPool, vk::CommandBufferLevel::ePrimary, numBuffers));

                uiRenderCommands = cmdBuffers.back();
                cmdBuffers.pop_back();
            }

            // Grab depth images to attach to this frame.
            const auto [depthBuffer, depthBufferView] = mGlfwVulkanWindow->getDepthBuffer();
            const auto [msaaColorImage, msaaColorImageView] = isMultisampled ? mGlfwVulkanWindow->getMultisampledColorImage() : std::tuple<vk::Image, vk::ImageView>();

            // Add correct image views to the color and depth attachments
            if (isMultisampled) {
                colorAttachment
                    .setResolveMode(vk::ResolveModeFlagBits::eAverage)
                    .setImageView(msaaColorImageView)
                    .setResolveImageView(imageViews[frameIdx])
                    .setResolveImageLayout(vk::ImageLayout::eColorAttachmentOptimal);
            } else {
                colorAttachment.setImageView(imageViews[frameIdx]);
            }
            depthAttachment.setImageView(depthBufferView);


            // Structures for defining a barrier that transitions the attachments into optimal layout prior to rendering.
            const vk::ImageSubresourceRange colorSubresourceRange = vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setLevelCount(1)
                .setLayerCount(1);
            const vk::ImageSubresourceRange depthSubresourceRange = vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eDepth)
                .setLevelCount(1)
                .setLayerCount(1);

            std::array<vk::ImageMemoryBarrier2, 3> attachmentBarriers {
                vk::ImageMemoryBarrier2() // Color attachment barrier. Transitions from undefined layout to attachment optimal layout. 
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eColorAttachmentOutput)
                    .setSrcAccessMask(vk::AccessFlagBits2::eColorAttachmentWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eColorAttachmentOutput)
                    .setDstAccessMask(vk::AccessFlagBits2::eColorAttachmentWrite)
                    .setOldLayout(vk::ImageLayout::eUndefined)
                    .setNewLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setImage(images[frameIdx])
                    .setDstQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSrcQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSubresourceRange(colorSubresourceRange),
                vk::ImageMemoryBarrier2() // Depth attachment barrier. Transitions from undefined layout to attachment optimal layout. 
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eLateFragmentTests)
                    .setSrcAccessMask(vk::AccessFlagBits2::eDepthStencilAttachmentWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eEarlyFragmentTests)
                    .setDstAccessMask(vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite)
                    .setOldLayout(vk::ImageLayout::eUndefined)
                    .setNewLayout(vk::ImageLayout::eDepthAttachmentOptimal)
                    .setImage(depthBuffer)
                    .setDstQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSrcQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSubresourceRange(depthSubresourceRange),
                vk::ImageMemoryBarrier2() // MSAA color attachment barrier. Transitions from undefined layout to attachment optimal layout.
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eColorAttachmentOutput)
                    .setSrcAccessMask(vk::AccessFlagBits2::eColorAttachmentWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eColorAttachmentOutput)
                    .setDstAccessMask(vk::AccessFlagBits2::eColorAttachmentWrite)
                    .setOldLayout(vk::ImageLayout::eUndefined)
                    .setNewLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setImage(msaaColorImage)
                    .setDstQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSrcQueueFamilyIndex(vk::QueueFamilyIgnored)
                    .setSubresourceRange(colorSubresourceRange),
            };

            // Begin command buffer recording with initial image layout transitions, then record viewport draw commands
            if (isFirstCall || mRenderCommandsReset) {
                vk::CommandBuffer& cmdBuffer = cmdBuffers[0];
                cmdBuffer.begin(vk::CommandBufferBeginInfo());

                // Transition color and depth attachments to attachment optimal layouts. 
                // When single sampled, only the first two image barriers are used. When multisampled, the third barrier is added to 
                // transition the multi-sample color attachment. 
                const vk::DependencyInfo depInfo({}, 0u, nullptr, 0u, nullptr, isMultisampled ? 3u : 2u, attachmentBarriers.data());
                cmdBuffer.pipelineBarrier2(depInfo);

                // Memory barrier to ensure uniform buffer carrying MV and P matrices is up to date. 
                VulkanClassicRasterEngine::getInstance()->recUniformBufferHostBarrier(cmdBuffers[0]);

                cmdBuffer.setViewportWithCount(viewport);
                cmdBuffer.setScissorWithCount(vk::Rect2D({}, attachmentExtent));
                // Record viewport draw render pass
                renderingInfo.setFlags(vk::RenderingFlagBits::eSuspending);
                mViewportModule->setViewport(viewport);
                mViewportModule->recRender(renderingInfo, cmdBuffer);

                cmdBuffers[0].end();
            }

            // Record rendering commands for all other modules
            for (uint32_t modIdx = 0; modIdx < mRenderModules.size(); ++modIdx) {
                // Skip recording this modules rendering commands if it is totally unviewed (invisible and previously unrecorded),
                // or if it was previously recorded and the commands were not reset. 
                const bool isRecorded = mModuleRecorded[modIdx];
                const bool isVisible = mModuleVisibility[modIdx];
                if ((!isRecorded && !isVisible) || (isRecorded && !mRenderCommandsReset)) continue;

                RenderModule& module = *mRenderModules[modIdx];
                vk::CommandBuffer cmdBuffer = cmdBuffers[modIdx+1];
                cmdBuffer.begin(vk::CommandBufferBeginInfo());

                cmdBuffer.setViewportWithCount(viewport);
                cmdBuffer.setScissorWithCount(vk::Rect2D({}, attachmentExtent));

                renderingInfo.setFlags(vk::RenderingFlagBits::eResuming | vk::RenderingFlagBits::eSuspending);

                module.recRender(renderingInfo, cmdBuffer);

                cmdBuffer.end();
            }

            // Render bitmap font UI
            if (isFirstCall || mRenderCommandsReset || mVisibilityChanged) {
                // Make sure the ui command buffer isn't in use during update
                const vk::Fence& inFlightFence = mGlfwVulkanWindow->getInFlightFences()[frameIdx];
                vk::Result waitResult = mDevice.logical.waitForFences(inFlightFence, true, 3000000000);
                vk::resultCheck(waitResult, "Waiting to re-record UI render commands");

                uiRenderCommands.reset({});
                uiRenderCommands.begin(vk::CommandBufferBeginInfo());

                uiRenderCommands.setViewportWithCount(viewport);
                uiRenderCommands.setScissorWithCount(vk::Rect2D({}, attachmentExtent));
                renderingInfo.setFlags(vk::RenderingFlagBits::eResuming | vk::RenderingFlagBits::eSuspending);

                // Attempt to detect high-dpi display, and increase font size. 
                GLFWmonitor* monitor = glfwGetWindowMonitor(mGlfwVulkanWindow->getWindow());
                if (monitor == nullptr) monitor = glfwGetPrimaryMonitor();
                const GLFWvidmode* vidMode = glfwGetVideoMode(monitor);
                const int minRes = std::min(vidMode->width, vidMode->height);
                const float textScale = minRes >= 2160 ? 1.5f : 1.0f;

                VulkanBitmapFont13Engine* fontEngine = VulkanBitmapFont13Engine::getInstance();
                using Color = VulkanBitmapFont13Engine::Color;
                const Color fontColor = Color(0.8f, 0.8f, 0.8f, 1.0f);
                const Color inactiveKeyColor = Color(0.25f, 0.25f, 0.25f, 1.0f);
                
                fontEngine->startFontRendering(viewport);

                fontEngine->addLine(10u, 12u, mGridInfo, textScale, fontColor);
                fontEngine->addLine(10u, 32u, mTransformInfo, textScale, fontColor);
                fontEngine->addLine(10u, 52u, mTreeInfo, textScale, fontColor);

                std::string onKeys(3, ' ');
                for (uint8_t i = 0; i < mModuleVisibility.size(); ++i) { if (mModuleVisibility[i]) onKeys[i] = char(49+i); }
                fontEngine->addLine(
                    viewport.width - uint32_t(std::ceil(40u*textScale)),
                    viewport.height - uint32_t(std::ceil(25u*textScale)),
                    "123", textScale, inactiveKeyColor);
                fontEngine->addLine(
                    viewport.width - uint32_t(std::ceil(41u*textScale)),
                    viewport.height - uint32_t(std::ceil(26u*textScale)),
                    onKeys, textScale, fontColor);
                
                fontEngine->recCommitFontRendering(renderingInfo, uiRenderCommands);

                uiRenderCommands.end();
            }

            // Final command buffer which completes the render pass suspend/resume chain and executes a memory barrier to 
            // prepare the frame for presentation to the window. 
            if (isFirstCall || mRenderCommandsReset) {
                cmdBuffers.back().begin(vk::CommandBufferBeginInfo());
                vk::CommandBuffer& cmdBuffer = cmdBuffers.back();

                // The spec requires that any suspended render pass must later be resumed, so we must
                // add this bogus empty render pass to close things out.  
                renderingInfo.setFlags(vk::RenderingFlagBits::eResuming);
                cmdBuffer.beginRendering(renderingInfo);
                cmdBuffer.endRendering();

                const vk::ImageMemoryBarrier2 presentBarrier = vk::ImageMemoryBarrier2(attachmentBarriers[0])
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eColorAttachmentOutput)
                    .setSrcAccessMask(vk::AccessFlagBits2::eColorAttachmentWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eBottomOfPipe)
                    .setDstAccessMask(vk::AccessFlagBits2::eNone)
                    .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::ePresentSrcKHR);
            
                cmdBuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, presentBarrier));

                cmdBuffers.back().end();
            }
        }

        // Mark that all visible modules have been recorded. 
        for (uint32_t i = 0; i < mModuleVisibility.size(); ++i) if (mModuleVisibility[i]) mModuleRecorded[i] = true;

        mRenderCommandsReset = mModuleNeedsRecord = mVisibilityChanged = false;
    }

}

void VulkanViewerImpl::updateCutPlanes(int wheelPos)
{
    double speed = std::abs(mWheelPos - wheelPos);
    if (mWheelPos < wheelPos) mClipBox->update(speed);
    else mClipBox->update(-speed);
    setNeedsDisplay();
}

void VulkanViewerImpl::swapBuffers()
{
    // Cooldown check hit whenever the swapchain has been invalidated (usually because of a window resize).
    // The cooldown is enforced because when the user resizes the window, this usually triggers dozens of 
    // resize events. Without a cooldown, this causes thrashing as the app tries to repeatedly recreate all
    // render resources, just for them to be immediately invalidated. 
    if (mWindowDamageCooldown > TimePt::min()) {
        if (Clock::now() < mWindowDamageCooldown) return; // Still on cooldown. Do nothing and return.

        // Cool down over. Reset cooldown variable, recreate render resources, re-record render buffers, 
        // and then continue to submission and presentation.
        mWindowDamageCooldown = TimePt::min();
        recreateAndResetRender();
        render();
    }

    const auto [result, bundle] = mGlfwVulkanWindow->acquireNextFrameBundle();

    // If acquire failed, we have no choice but to drop this frame...
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        // Swapchain is broken, initiate 100ms cooldown after which swapchain and render resources will be recreated.
        mWindowDamageCooldown = Clock::now() + std::chrono::milliseconds(100);
        return;
    } else {

        const std::vector<vk::CommandBuffer>& renderCommands = mRenderCommands[bundle.imageIndex];
        std::vector<vk::CommandBuffer> enabledCommands; enabledCommands.reserve(5);

        // Enqueue enabled command buffers. First command buffer is for the viewport elements, and is always enabled.
        enabledCommands.push_back(renderCommands[0]);
        for (uint32_t i = 0; i < mRenderModules.size(); ++i) {
            if (mModuleVisibility[i]) enabledCommands.push_back(renderCommands[i+1]);
        }

        // Add UI elements if info is enabled
        if (mShowInfo) enabledCommands.push_back(mInfoRenderCommands[bundle.imageIndex]);

        // Last command buffer is always required to close out the render
        enabledCommands.push_back(renderCommands.back());
        
        // Submit command buffers for execution
        const vk::PipelineStageFlags dstMask(vk::PipelineStageFlagBits::eColorAttachmentOutput);
        mOmniQueue.getQueue().submit(vk::SubmitInfo(bundle.acquireSemaphore, dstMask, enabledCommands, bundle.renderSemaphore), bundle.inFlightFence);

        // Submit frame for presentation on the window
        const vk::Result presentResult = mGlfwVulkanWindow->submitNextFrameBundle(mOmniQueue, bundle);
        if (presentResult != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
            // Swapchain is broken, initiate 100ms cooldown after which swapchain and render resources will be recreated.
            mWindowDamageCooldown = Clock::now() + std::chrono::milliseconds(100);
            return;
        }
    }
}

void VulkanViewerImpl::setWindowTitle(double fps)
{
    std::ostringstream ss;
    ss  << mProgName << ": ";
    if (fps >= 0.0) {
        ss << (mGridName.empty() ? std::string("OpenVDB") : mGridName)
           << " (" << (mGridIdx + 1) << " of " << mGrids.size() << ") @ "
           << std::setprecision(1) << std::fixed << fps << " fps";
    } else {
        ss << "Loading Render Module... " << (mGridName.empty() ? std::string("OpenVDB") : mGridName)
           << " (" << (mGridIdx + 1) << " of " << mGrids.size() << ")";
    }

    if (mGlfwVulkanWindow->isWindowOpen()) glfwSetWindowTitle(mGlfwVulkanWindow->getWindow(), ss.str().c_str());
}

void VulkanViewerImpl::showPrevGrid()
{
    if (const size_t numGrids = mGrids.size()) {
        size_t idx = ((numGrids + mGridIdx) - 1) % numGrids;
        showNthGrid(idx);
    }
}

void VulkanViewerImpl::showNextGrid()
{
    if (const size_t numGrids = mGrids.size()) {
        size_t idx = (mGridIdx + 1) % numGrids;
        showNthGrid(idx);
    }
}

void VulkanViewerImpl::showNthGrid(size_t n) {
    if (mGrids.empty()) return;
    n = n % mGrids.size();
    if (n == mGridIdx) return;

    mGridName = mGrids[n]->getName();
    mGridIdx = n;

    if (!mRenderModules.empty()) {
        mOmniQueue.waitIdle();
        mDevice.logical.resetCommandPool(mCommandPool);
        mModuleRecorded.reset();
        mRenderCommandsReset = true;
    }
    
    mRenderModules.clear();
    mRenderModules.push_back(RenderModulePtr(new TreeTopologyModule(mGrids[n], true)));
    mRenderModules.push_back(RenderModulePtr(new MeshModule(mGrids[n], true)));
    mRenderModules.push_back(RenderModulePtr(new VoxelModule(mGrids[n], true)));

    // Collect info
    {
        std::ostringstream ostrm;
        std::string s = mGrids[n]->getName();
        const openvdb::GridClass cls = mGrids[n]->getGridClass();
        if (!s.empty()) ostrm << s << " / ";
        ostrm << mGrids[n]->valueType() << " / ";
        if (cls == openvdb::GRID_UNKNOWN) ostrm << " class unknown";
        else ostrm << openvdb::GridBase::gridClassToString(cls);
        mGridInfo = ostrm.str();
    }
    {
        openvdb::Coord dim = mGrids[n]->evalActiveVoxelDim();
        std::ostringstream ostrm;
        ostrm << dim[0] << " x " << dim[1] << " x " << dim[2]
            << " / voxel size " << std::setprecision(4) << mGrids[n]->voxelSize()[0]
            << " (" << mGrids[n]->transform().mapType() << ")";
        mTransformInfo = ostrm.str();
    }
    {
        std::ostringstream ostrm;
        const openvdb::Index64 count = mGrids[n]->activeVoxelCount();
        ostrm << openvdb::util::formattedInt(count)
            << " active voxel" << (count == 1 ? "" : "s");
        mTreeInfo = ostrm.str();
    }
    {
        if (mGrids[n]->isType<openvdb::points::PointDataGrid>()) {
            const openvdb::points::PointDataGrid::ConstPtr points =
                openvdb::gridConstPtrCast<openvdb::points::PointDataGrid>(mGrids[n]);
            const openvdb::Index64 count = openvdb::points::pointCount(points->tree());
            std::ostringstream ostrm;
            ostrm << " / " << openvdb::util::formattedInt(count)
                 << " point" << (count == 1 ? "" : "s");
            mTreeInfo.append(ostrm.str());
        }
    }

    setWindowTitle();
}

void VulkanViewerImpl::keyCallback(int key, int action) {
    mCamera->keyCallback(key, action);

    if (!mGlfwVulkanWindow->isWindowOpen()) return;
    GLFWwindow* window = mGlfwVulkanWindow->getWindow();

    const bool keyPress = (glfwGetKey(window, key) == GLFW_PRESS);
    /// @todo Should use "modifiers" argument to keyCB().
    mShiftIsDown = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT);
    mCtrlIsDown = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL);

    if (keyPress) {
        switch (key) {
        case '1': case GLFW_KEY_KP_1:
            toggleRenderModule(0);
            break;
        case '2': case GLFW_KEY_KP_2:
            toggleRenderModule(1);
            break;
        case '3': case GLFW_KEY_KP_3:
            toggleRenderModule(2);
            break;
        case 'c': case 'C':
            mClipBox->reset();
            break;
        case 'h': case 'H': // center home
            mCamera->setLookAtPoint(openvdb::Vec3d(0.0), 10.0);
            break;
        case 'g': case 'G': // center geometry
            mCamera->setLookToTarget();
            break;
        case 'i': case 'I':
            toggleInfoText();
            break;
        case GLFW_KEY_F11:
            toggleFullscreen();
            break;
        case GLFW_KEY_LEFT:
            showPrevGrid();
            break;
        case GLFW_KEY_RIGHT:
            showNextGrid();
            break;
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(window, true);
            break;
        }
    }

    switch (key) {
    case 'x': case 'X':
        mClipBox->activateXPlanes() = keyPress;
        break;
    case 'y': case 'Y':
        mClipBox->activateYPlanes() = keyPress;
        break;
    case 'z': case 'Z':
        mClipBox->activateZPlanes() = keyPress;
        break;
    }

    mClipBox->shiftIsDown() = mShiftIsDown;
    mClipBox->ctrlIsDown() = mCtrlIsDown;

    setNeedsDisplay();
}


void VulkanViewerImpl::mouseButtonCallback(int button, int action) {
    mCamera->mouseButtonCallback(button, action);
    mClipBox->mouseButtonCallback(button, action);
    if (mCamera->needsDisplay()) setNeedsDisplay();
}

void VulkanViewerImpl::mousePosCallback(int x, int y) {
    bool handled = mClipBox->mousePosCallback(x, y);
    if (!handled) mCamera->mousePosCallback(x, y);
    if (mCamera->needsDisplay()) setNeedsDisplay();
}

void VulkanViewerImpl::mouseWheelCallback(int pos) {
    pos += mWheelPos;
    if (mClipBox->isActive()) {
        updateCutPlanes(pos);
    } else {
        mCamera->mouseWheelCallback(pos, mWheelPos);
        if (mCamera->needsDisplay()) setNeedsDisplay();
    }

    mWheelPos = pos;
}


void VulkanViewerImpl::windowSizeCallback(int, int) {
    // A window change has occurred and will almost certainly break the swapchain.
    // Initiate a 100ms cooldown after which swapchain and other render resources will be recreated.
    mWindowDamageCooldown = Clock::now() + std::chrono::milliseconds(100);
    setNeedsDisplay();
}


void VulkanViewerImpl::windowRefreshCallback() {
    // A window change has occurred and will almost certainly break the swapchain.
    // Initiate a 100ms cooldown after which swapchain and other render resources will be recreated.
    mWindowDamageCooldown = Clock::now() + std::chrono::milliseconds(100);
    setNeedsDisplay();
}

void VulkanViewerImpl::toggleFullscreen() {
    GLFWwindow* window = mGlfwVulkanWindow->getWindow();
    if(glfwGetWindowMonitor(window) != nullptr) {
        glfwSetWindowMonitor(
            window, nullptr, mStashedWindowPosSize.xpos, mStashedWindowPosSize.ypos,
            mStashedWindowPosSize.width, mStashedWindowPosSize.height, GLFW_DONT_CARE);
    } else {
        glfwGetWindowPos(window, &mStashedWindowPosSize.xpos, &mStashedWindowPosSize.ypos);
        glfwGetWindowSize(window, &mStashedWindowPosSize.width, &mStashedWindowPosSize.height);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* vidMode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0, vidMode->width, vidMode->height, vidMode->refreshRate);
    }

    recreateAndResetRender();
    mWindowDamageCooldown = TimePt::min();
}

bool VulkanViewerImpl::needsDisplay() {
    if (mUpdates < 2) {
        mUpdates += 1;
        return true;
    }
    return false;
}

void VulkanViewerImpl::setNeedsDisplay() {
    mUpdates = 0;
}

void VulkanViewerImpl::toggleRenderModule(size_t n) {
    mModuleVisibility[n] = !mModuleVisibility[n];

    // If this module has just become visible, but has no recorded render commands, flag the 
    // renderer that new commands are needed. 
    if (mModuleVisibility[n] && !mModuleRecorded[n])
        mModuleNeedsRecord = true;

    mVisibilityChanged = true;
}

void VulkanViewerImpl::toggleInfoText() {
    mShowInfo = !mShowInfo;
}

} // namespace openvdb_viewer
