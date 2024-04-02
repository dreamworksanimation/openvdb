// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
#pragma once

#define GLFW_INCLUDE_VULKAN
#define GLFW_VULKAN_STATIC
#include <GLFW/glfw3.h>

#include <tuple>
#include <optional>

#include <vulkan/vulkan.hpp>
#include <vma/vk_mem_alloc.h>

/// @brief Class encapsulating a GLFW window used as a Vulkan rendering surface, and provides utilities for implementing a basic
///        render loop presenting to that window. 
///
/// The owned GLFW window and all associated Vulkan resources have the same lifetime as the class instance. Likewise, instances
/// can only be moved, not copied. 
class GlfwVulkanWindow
{
public:
    // Only move construction/assignment and construction using the `GlfwVulkanWindowBuilder` is supported. 
    // Necessary to protect Vulkan resource lifetimes. 
    GlfwVulkanWindow(GlfwVulkanWindow&& aOther);
    GlfwVulkanWindow& operator=(GlfwVulkanWindow&& aOther);

    ~GlfwVulkanWindow() { if (isWindowOpen()) close(); }

    /// @brief Creates swapchain, as well as the depth buffer and multisampling resources if enabled. 
    void createAllRenderResources();
    /// @brief Creates swapchain. Doesn't need to be called unless render resource creation was deferred.
    void createSwapchain();
    /// @brief Creates depth buffer. Doesn't need to be called unless render resource creation was deferred.
    void createDepthBuffer();
    /// @brief Creates MS image attachment. Doesn't need to be called unless resource creation was deferred.
    void createMultisampleColorImage();

    /// @brief Recreates swapchain and, if enabled, the depth buffer and multisampled color image. This function
    ///        is intended to handle window resizing and other window changes which invalidate the swapchain. 
    void recreateRenderResources();

    /// @brief Cleanup all Vulkan resources and destroy the GLFW window.
    ///        Safely does nothing if no window resources were ever created. 
    void close();
    
    /// @brief Returns true if the GLFW window is open. An open window implies a valid Vulkan presentation surface exists as well. 
    bool isWindowOpen() const { return mWindow != nullptr; }
    /// @brief Returns true if a swapchain has been created for this window. 
    bool hasSwapchain() const { return mSwapchain; }
    /// @brief Returns true if the GLFW window, presentation surface, and swapchain are all good to go.
    bool isPresentable() const { return isWindowOpen() && hasSwapchain(); }
    /// @brief Returns true if the swapchain recently indicated it was suboptimal
    bool isSuboptimal() const { return mIsSuboptimal; }

    /// @brief Returns true if this window **can** create a depth buffer matching the swapchain.
    /// You must check `hasDepthBuffer()` to confirm that one is actually being maintained. 
    bool supportsDepthBuffer() const { return bool(mDepthBufferCreateInfo); }
    /// @brief Returns true if this window is currently maintaining a depth buffer.
    bool hasDepthBuffer() const { return bool(std::get<0>(mDepthBuffer)); }

    /// Returns true if this window is setup to support multisample rendering. When true, this window 
    /// provides a multisampled color image matching the format and extent of the images in the swapchain. 
    /// This image can be used as a color attachment for multisampled rendering, and then resolved into
    /// swapchain images for presentation. The image uses `multisampleCount()` many samples.
    ///
    /// When both depth buffering and multisampling are enabled, then the depth buffer image is also 
    /// multisampled with `multisampleCount()` many samples. 
    bool isMultisampled() const { return bool(mMultisampleColorCreateInfo); }
    /// @brief Returns the number of samples being used for multisampling. Returns `e1` if multisampling 
    /// is disabled. 
    vk::SampleCountFlagBits multisampleCount() const {
        return mMultisampleColorCreateInfo.has_value() ? mMultisampleColorCreateInfo->samples : vk::SampleCountFlagBits::e1;
    }

    /// @brief Return GLFW window pointer, or `nullptr` if `isWindowOpen() == false`
    GLFWwindow* getWindow() { return mWindow; }
    const GLFWwindow* getWindow() const { return mWindow; }

    /// @brief Current extent of the swapchain images, the extents of the depth buffer and multisample
    /// color image are the same. 
    const vk::Extent2D currentExtent() const { return mSwapchainCreateInfo.imageExtent; }

    /// @brief Returns cref to window's presentation surface
    const vk::SurfaceKHR& getSurface() const { return mSurface; }
    /// @brief Returns cref to window's swapchain
    const vk::SwapchainKHR& getSwapchain() const { return mSwapchain; }

    /// @brief Returns the number of images within the swapchain. 
    size_t numSwapchainImages() const { return mSwapchainImages.size(); }

    /// @brief Returns the images in this window's swapchain.
    ///        Will return an empty list if no swapchain has been created. 
    const std::vector<vk::Image>& getSwapchainImages() const { return mSwapchainImages; };
    /// @brief Returns standard full image views of the images in the swapchain.
    ///        Will return an empty list if no swapchain is created.
    const std::vector<vk::ImageView>& getSwapchainImageViews() const { return mSwapchainImageViews; }
    /// @brief Returns list of semaphores, one for each image in the swapchain, which are signalled when the 
    ///        corresponding image is acquired and can be written to. 
    const std::vector<vk::Semaphore>& getAcquireSemaphores() const { return mAcquireSemaphores; }
    /// @brief Returns list of semaphores, one for each image in the swapchain, which must be signalled after
    ///        render to the corresponding completes, to inform the presentation image it is ready to display. 
    const std::vector<vk::Semaphore>& getRenderSemaphores() const { return mRenderSemaphores; }
    /// @brief Returns list of fences associated with the images in the swapchain. One for each image.
    ///        Will return an empty list if no swapchain has been created.
    const std::vector<vk::Fence>& getInFlightFences() { return mInFlightFences; }

    /// @brief Timeout, in nanoseconds, when waiting on frame-in-flight fences. Defaults to 3 seconds.
    const uint64_t getInFlightTimeout() const { return mInFlightTimeout; }
    void setInFlightTimeout(uint64_t t) { mInFlightTimeout = t; }

    /// @brief Return depth buffer image and full image view. Will return invalid handle if this window isn't maintaining a depth buffer. 
    std::tuple<vk::Image, vk::ImageView> getDepthBuffer() const;
    /// @brief Return multisampled color image and full image view. Will return invalid handle if this window isn't providing multisampling. 
    std::tuple<vk::Image, vk::ImageView> getMultisampledColorImage() const;
    
    /// Bundle of resource handles typical used to render a single frame into the swapchain. 
    /// Returned by `acquireNextFrameBundle()`, which documents further. 
    struct SwapchainFrameBundle
    {
        uint32_t imageIndex;
        vk::Semaphore acquireSemaphore;
        vk::Semaphore renderSemaphore;
        vk::Fence inFlightFence;
    };

    /// @brief Request the next image from the swapchain. 
    ///
    /// When successful, returns a bundle containing the index of
    /// the acquired swapchain image, the semaphore that will be signalled once the image is ready for writing,
    /// the semaphore that will be waited on before presenting that image to the window, and a fence for blocking
    /// duplicate submissions to the acquired image. See @ref sRenderLoop. 
    ///
    /// @param aTimeout [Optional] Number of nanoseconds to wait for an image to become available. Default is no timeout.
    /// @param aFence [Optional] A fence to signal when the next image becomes available. The provided fence handle is passed 
    ///               directly to `vkAcquireNextImageKHR()`. By default, no **fence** is signalled when the image becomes
    ///               available, as synchronizing using semaphores is preferable.
    ///               
    /// @returns `vk::ResultValue<SwapchainFrameBundle>` containing the next frame bundle if acquisition succeeded. Acquisition is 
    ///          a success when the result value is `eSuccess` or `eSuboptimalKHR`. When acquisition fails, a failure enum value is 
    ///          returned along with an invalid `SwapchainFrameBundle`. 
    vk::ResultValue<SwapchainFrameBundle> acquireNextFrameBundle(uint64_t aTimeout = UINT64_MAX, vk::Fence aFence = VK_NULL_HANDLE);

    /// @brief Submits the swapchain frame bundle previously acquired via `acquireNextFrameBundle()` for presentation on
    ///        the GLFW window. 
    ///
    /// @param aPresentQueue Presentation capable queue instance to which the frame will be submitted. 
    /// @param aBundle       The frame bundle returned by the previous call to `acquireNextFrameBundle()`
    vk::Result submitNextFrameBundle(const vk::Queue& aPresentQueue, const SwapchainFrameBundle& aBundle);

    /// @brief Returns cref to the info struct used internally to create and recreate the swapchain.
    const vk::SwapchainCreateInfoKHR& getSwapchainCreateInfo() const { return mSwapchainCreateInfo; }
    /// Returns mutable reference to info struct used to create and recreate the swapchain, allowing arbitrary tweaks. 
    /// WARNING: It is easy to break things by overriding the values within this structure! 
    vk::SwapchainCreateInfoKHR& getSwapchainCreateInfoMut() { return mSwapchainCreateInfo; }

    /// @brief Returns cref to info struct used internally to create the depth buffer, if it exists. Otherwise `null_opt`. 
    const std::optional<vk::ImageCreateInfo>& getDepthBufferCreateInfo() const { return mDepthBufferCreateInfo; }
    /// Returns mutable reference to info struct used internally to create the depth buffer, if it exists. Otherwise `null_opt`.
    /// This can be used to make arbitrary tweaks to depth resource creation, **at your own risk**. 
    std::optional<vk::ImageCreateInfo>& getDepthBufferCreateInfoMut() { return mDepthBufferCreateInfo; }

    /// @brief Returns cref to info struct used internally to create the multisample color image, if it exists. Otherwise `null_opt`. 
    const std::optional<vk::ImageCreateInfo>& getMultisampleColorImageCreateInfo() const { return mMultisampleColorCreateInfo; }
    /// Returns mutable reference to info struct used internally to create the depth buffer, if it exists. Otherwise `null_opt`.
    /// This can be used to make arbitrary tweaks to multisampled resource creation, **at your own risk**. 
    std::optional<vk::ImageCreateInfo>& getMultisampleColorImageCreateInfoMut() { return mMultisampleColorCreateInfo; }

protected:
    friend class GlfwVulkanWindowBuilder;

    GlfwVulkanWindow() = default;

    void createWindowAndSurface(
        uint32_t aWidth, uint32_t aHeight, const char* aTitle = "",
        GLFWmonitor* aMonitor = nullptr, GLFWwindow* aShareWindow = nullptr);

    void populateSwapchainInfo(
        const vk::SurfaceFormatKHR& aSurfFormatPreferred, const vk::PresentModeKHR& aPresentModePreferred,
        uint32_t aLengthPreferred, bool aReset = true);
    void populateDepthBufferInfo(const vk::Format& aDepthFormatPreferred, bool aReset = true);
    void populateMultisampleInfo(vk::SampleCountFlagBits aSampleCount, bool aReset = true);

    void recreateDepthBuffer();
    void recreateMultisamplingResources();

    void destroySwapchain(const bool aKeepForRecycle = false);
    void destroyDepthBuffer();
    void destroyMultisampleColorImage();
    void destroySurfaceAndWindow();

    GLFWwindow* mWindow = nullptr;
    vk::SurfaceKHR mSurface = VK_NULL_HANDLE;

    vk::SwapchainCreateInfoKHR mSwapchainCreateInfo;
    std::optional<vk::ImageCreateInfo> mDepthBufferCreateInfo;
    std::optional<vk::ImageCreateInfo> mMultisampleColorCreateInfo;

    vk::SwapchainKHR mSwapchain;
    uint32_t mNextFrameIndex = 0u;
    std::vector<vk::Image> mSwapchainImages;
    std::vector<vk::ImageView> mSwapchainImageViews;
    std::vector<vk::Semaphore> mAcquireSemaphores;
    std::vector<vk::Semaphore> mRenderSemaphores;
    std::vector<vk::Fence> mInFlightFences;
    uint64_t mInFlightTimeout = 3000000000; // Nanoseconds. Default 3s timeout when waiting on in-flight fences.

    std::tuple<vk::Image, vk::ImageView, VmaAllocation> mDepthBuffer;
    std::tuple<vk::Image, vk::ImageView, VmaAllocation> mMultisampleColor;

    bool mIsSuboptimal = false;

    // Necessary Vulkan handles
    vk::Instance mParentInstance;
    struct {vk::PhysicalDevice physical; vk::Device logical;} mParentDevice;
    VmaAllocator mAllocator = VK_NULL_HANDLE; // Optional. Required if managing depth and/or multisample color images. 

private:
    GlfwVulkanWindow(const GlfwVulkanWindow&) = default;
    GlfwVulkanWindow& operator=(const GlfwVulkanWindow&) = default;

    /// @brief Release ownership of all contained resources and invalidate this class. Prevents premature destruction of resources
    ///        after move operation. 
    void release();
};

/// @brief Utility class required for creating `GlfwVulkanWindow` instances.
///
/// Construction of a `GlfwVulkanWindow` involves many inputs and parameters, both required and optional. The builder organizes
/// these parameters, provides reasonable defaults where possible, and provides setter functions which can be cascaded. 
///
/// The builder's constructor takes, as parameters, the required Vulkan instance (`vk::Instance`), physical device (`vk::PhysicalDevice`),
/// and logical device (`vk::Device`). It optionally accepts a Vulkan memory allocator, which is necessary if the window is going to 
/// manage additional render resources like a depth buffer or multisample image. Afterwards, the remaining parameters can either be modified
/// directly as class members or via setter methods. Each member is documented separately. 
///
/// The window can be spawned by calling `build()`. This will throw `std::logical_error` if the state of the builder is invalid.
/// Invalidity occurs if any of the Vulkan handles are invalid, or if depth buffer support or multisampling was requested without 
/// a VMA allocator instance having been provided. A call to `isBuildReady()` can be made to check validity without exceptions.
///
/// **Example:**
/// ```
/// // Create a 600x400 GLFW Vulkan window which is maintains a depth buffer and is triple-buffered (if supported). 
/// GlfwVulkanWindowBuilder builder(vkInstance, vkPhysDev, vkLogicalDev, vmaAlloc);
/// builder
///     .setDimensions(600, 400)
///     .setDepthBufferEnabled(true)
///     .setPreferredSwapchainLength(3u)
///     .setTitle("Hello World");
/// assert(builder.isBuildReady())
/// glfwVulkanWindow = builder.build();
/// ```
class GlfwVulkanWindowBuilder
{
public:
    /// @brief Construct a new builder. Requires a Vulkan instance, physical device, and logical device. Vulkan memory allocator optional. 
    ///
    /// @param 
    GlfwVulkanWindowBuilder(
        vk::Instance aVulkanInstance, vk::PhysicalDevice aPhysDev,
        vk::Device aDevice, VmaAllocator aAllocator = VK_NULL_HANDLE);
    
     /*! @brief Sets @ref mAllocator */         GlfwVulkanWindowBuilder& setAllocator(VmaAllocator aAllocator);
     /*! @brief Sets @ref mTitle */             GlfwVulkanWindowBuilder& setTitle(const char* aTitle);
     /*! @brief Sets window dimensions */       GlfwVulkanWindowBuilder& setDimensions(uint32_t width, uint32_t height);
     /*! @brief Sets @ref mCreateDepthBuffer */ GlfwVulkanWindowBuilder& setDepthBufferEnabled(bool aValue);
     /*! @brief Sets @ref mSampleCount */       GlfwVulkanWindowBuilder& setSamplingCount(vk::SampleCountFlagBits aSampleCount);
     /*! @brief Sets @ref mMonitor */           GlfwVulkanWindowBuilder& setGlfwMonitor(GLFWmonitor* aMonitor);
     /*! @brief Sets @ref mSharedWindow */      GlfwVulkanWindowBuilder& setGlfwSharedWindow(GLFWwindow* aShareWindow);

     /*! @brief Sets @ref mSwapchainLengthPreferred */ GlfwVulkanWindowBuilder& setPreferredSwapchainLength(uint32_t aLength);
     /*! @brief Sets @ref mPresentModePreferred */     GlfwVulkanWindowBuilder& setPreferredPresentMode(const vk::PresentModeKHR& aPresentMode);
     /*! @brief Sets @ref mSurfaceFormatPreferred */   GlfwVulkanWindowBuilder& setPreferredSurfaceFormat(const vk::SurfaceFormatKHR& aSurfFormat);
     /*! @brief Sets @ref mDepthFormatPreferred */     GlfwVulkanWindowBuilder& setPreferredDepthFormat(const vk::Format& aDepthFormat);

    /// @brief Validate whether or not builder contents is ready for a call to `build()`. If @a aWarn is true,
    ///        this function will emit specifics to stderr.
    bool isBuildReady(bool aWarn = true) const;

    /// @brief Create a `GlfwVulkanWindow` from the builder, optionally deferring actual creation of the window and its resources.
    ///
    /// @param aDeferRenderResources [Default = false] When true, the window and Vulkan surface are created, but creation of the
    ///                              swapchain and other resources is deferred, allowing the caller to tweak resource creation 
    ///                              parameters first.
    /// @throws std::logic_error if the builder contains an invalid combination of values. This can be checked using `isBuildReady()`,
    ///         which will print warnings to stderr rather than throwing them. 
    GlfwVulkanWindow build(bool aDeferRenderResources = false);

    /// [Required] Vulkan instance to create and own the window surface.
    vk::Instance mVulkanInstance;
    /// [Required] Vulkan physical device for which the swapchain and other resources will be created.
    vk::PhysicalDevice mPhysicalDevice;
    /// [Required] Vulkan logical device on which the swapchain and other resources are created.
    vk::Device mDevice;
    /// [Optional] Vulkan memory allocator for allocating additional resources. Required if enabling depth or multisampling
    VmaAllocator mAllocator = VK_NULL_HANDLE;

    /// [Default = 256x256] Width of created window.
    uint32_t mWidth = 256u;
    /// [Default = 256x256] Height of created window.
    uint32_t mHeight = 256u;
    /// [Optional] Initial window title.
    const char* mTitle = "";
    /// [Optional] Forwarded to `glfwCreateWindow()`. See GLFW docs.
    GLFWmonitor* mMonitor = nullptr;
    /// [Optional] Forwarded to `glfwCreateWindow()`. See GLFW docs.
    GLFWwindow* mSharedWindow = nullptr;

    /// [Optional] Request that the window create and maintain a depth buffer matching the swapchain.
    bool mCreateDepthBuffer = false;
    /// @brief [Optional] Set the number of samples used for rendering, enabling multisampling support if not 1.
    ///
    /// When equal to anything other than `e1`, enables multisampling support, causing the window to create and
    /// maintain a multisample color image matching the swapchain, that can be used as multisampled color attachment 
    /// for rendering. 
    vk::SampleCountFlagBits mSampleCount = vk::SampleCountFlagBits::e1;

    /// @brief [Default = 2] The preferred length of the swapchain. '2' means double buffered. 
    /// 
    /// If the preferred value is not supported, the swapchain will clamp the preferred value in-between the surface's
    /// supported minimum and maximum supported image counts.
    uint32_t mSwapchainLengthPreferred = 2u;
    /// @brief [Default = FIFO] The preferred presentation mode for the swapchain to utilize.
    /// 
    /// If not supported, the swapchain will fallback to FIFO mode, which is universally supported. 
    vk::PresentModeKHR mPresentModePreferred = vk::PresentModeKHR::eFifo;
    /// @brief [Default = 8-bit sRGB+A] The preferred surface format and colorspace for the swapchain.
    /// 
    /// If the preferred value is not supported, the window will fall back to the first format supported by the surface. 
    vk::SurfaceFormatKHR mSurfaceFormatPreferred = {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear};
    /// @brief [Default = 24-bit unorm depth + 8-bit unsigned stencil] The preferred image format of the depth buffer, if one is to be created.
    /// 
    /// If the preferred value is not supported, the window will attempt to fall back to 32-bit float or 16-bit unorm,
    /// which are widely supported. 
    vk::Format mDepthFormatPreferred = vk::Format::eD24UnormS8Uint;

private:
    bool _isBuildReady(bool aThrows) const;
};
