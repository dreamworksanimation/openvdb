// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "GlfwVulkan.h"
#include <iostream>

GlfwVulkanWindow::GlfwVulkanWindow(GlfwVulkanWindow&& aOther) : GlfwVulkanWindow(aOther) {
    aOther.release();
}

GlfwVulkanWindow& GlfwVulkanWindow::operator=(GlfwVulkanWindow&& aOther) {
    *this = aOther;
    aOther.release();
    return *this;
}

void GlfwVulkanWindow::createAllRenderResources() {
    createSwapchain();
    if (supportsDepthBuffer())
        createDepthBuffer();
    if (isMultisampled())
        createMultisampleColorImage();
}

void GlfwVulkanWindow::createWindowAndSurface(
    uint32_t aWidth, uint32_t aHeight, const char* aTitle,
    GLFWmonitor* aMonitor, GLFWwindow* aShareWindow)
{
    if (isWindowOpen()) return;

    // Tell GLFW not to create an OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    mWindow = glfwCreateWindow(aWidth, aHeight, aTitle, aMonitor, aShareWindow);
    if (mWindow == nullptr) {
        const char* what = nullptr;
        glfwGetError(&what);
        throw std::runtime_error("GlfwVulkanWindow: GLFW failed to create window: '" + (what ? std::string(what) : "unknown reason") + "'");
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vk::Result r = vk::Result(glfwCreateWindowSurface(mParentInstance, mWindow, nullptr, &surface));
    if (r != vk::Result::eSuccess) {
        const char* what = nullptr;
        glfwGetError(&what);
        const std::string desc("GlfwVulkanWindow: GLFW failed to create window surface: '" + (what ? std::string(what) : "(unknown reason)") + "'");
        vk::resultCheck(r, desc.c_str());
    }

    mSurface = surface;
}

void GlfwVulkanWindow::createSwapchain() {
    if (!isWindowOpen()) throw std::runtime_error("GlfwVulkanWindow: createSwapchain() called, but no window has been created!");

    const bool recycleSwapchain = bool(mSwapchain);
    if (recycleSwapchain) mSwapchainCreateInfo.setOldSwapchain(mSwapchain);
    mSwapchain = mParentDevice.logical.createSwapchainKHR(mSwapchainCreateInfo);
    if (recycleSwapchain) mParentDevice.logical.destroy(mSwapchainCreateInfo.oldSwapchain);
    mSwapchainImages = mParentDevice.logical.getSwapchainImagesKHR(mSwapchain);
    
    // Get ready to construct other objects associated with each image. 
    mSwapchainImageViews.resize(mSwapchainImages.size());
    mAcquireSemaphores.resize(mSwapchainImages.size());
    mRenderSemaphores.resize(mSwapchainImages.size());
    mInFlightFences.resize(mSwapchainImages.size());

    // Create standard 2D color image views for each swapchain image
    vk::ImageViewCreateInfo viewCreateInfo(vk::ImageViewCreateFlags(0u), VK_NULL_HANDLE,
                                           vk::ImageViewType::e2D, mSwapchainCreateInfo.imageFormat);
    viewCreateInfo.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    
    std::transform(mSwapchainImages.cbegin(), mSwapchainImages.cend(), mSwapchainImageViews.begin(),
        [this, &viewCreateInfo](const vk::Image& image) {
            viewCreateInfo.setImage(image);
            return mParentDevice.logical.createImageView(viewCreateInfo);
    });

    // Create a fence and semaphore for each image
    for (size_t i = 0; i < mSwapchainImages.size(); ++i) {
        mAcquireSemaphores[i] = mParentDevice.logical.createSemaphore({});
        mRenderSemaphores[i] = mParentDevice.logical.createSemaphore({});
        mInFlightFences[i] = mParentDevice.logical.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
    }
}

void GlfwVulkanWindow::createDepthBuffer() {
    if (!supportsDepthBuffer()) throw std::logic_error("GlfwVulkanWindow: createDepthBuffer() called on window not setup to provide a depth buffer!");

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    const VkImageCreateInfo& imageInfoC = *mDepthBufferCreateInfo;
    VkImage tempImageHandle;

    VkResult r = vmaCreateImage(mAllocator, &imageInfoC, &allocInfo, &tempImageHandle, &std::get<2>(mDepthBuffer), nullptr);
    vk::resultCheck(vk::Result(r), "GlfwVulkanWindow: Failed to allocate device memory for depth buffer!");
    std::get<0>(mDepthBuffer) = tempImageHandle;

    vk::ImageViewCreateInfo depthImageViewCreateInfo({}, {}, vk::ImageViewType::e2D, mDepthBufferCreateInfo->format);
    depthImageViewCreateInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0u, vk::RemainingMipLevels, 0u, 1u));
    std::get<1>(mDepthBuffer) = mParentDevice.logical.createImageView((depthImageViewCreateInfo.setImage(tempImageHandle)));
}

void GlfwVulkanWindow::createMultisampleColorImage() {
    if (!isMultisampled()) throw std::logic_error("GlfwVulkanWindow: createMultisampleColorImage() called on window not setup to support multisampling!");
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    const VkImageCreateInfo& imageInfoC = *mMultisampleColorCreateInfo;
    VkImage tempImageHandle;

    VkResult r = vmaCreateImage(mAllocator, &imageInfoC, &allocInfo, &tempImageHandle, &std::get<2>(mMultisampleColor), nullptr);
    vk::resultCheck(vk::Result(r), "GlfwVulkanWindow: Failed to allocate device memory for multisampled color image!");
    std::get<0>(mMultisampleColor) = tempImageHandle;

    vk::ImageViewCreateInfo multisampleImageViewCreateInfo({}, {}, vk::ImageViewType::e2D, mMultisampleColorCreateInfo->format);
    multisampleImageViewCreateInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0u, vk::RemainingMipLevels, 0u, 1u));
    std::get<1>(mMultisampleColor) = mParentDevice.logical.createImageView((multisampleImageViewCreateInfo.setImage(tempImageHandle)));
}

void GlfwVulkanWindow::recreateRenderResources() {
    if (!isWindowOpen()) throw std::runtime_error("GlfwVulkanWindow: recreateSwapchain() called, but no window has been created!");
    if (!mSwapchain) throw std::runtime_error("GlfwVulkanWindow: recreateSwapchain() called, but no swapchain has been created!");

    destroySwapchain(true);
    populateSwapchainInfo(
        vk::SurfaceFormatKHR(mSwapchainCreateInfo.imageFormat, mSwapchainCreateInfo.imageColorSpace),
        mSwapchainCreateInfo.presentMode, mSwapchainCreateInfo.minImageCount, false);
    createSwapchain();
    mNextFrameIndex = 0;

    if (isMultisampled()) recreateMultisamplingResources();
    if (hasDepthBuffer()) recreateDepthBuffer();
}

void GlfwVulkanWindow::close() {
    if (isMultisampled()) destroyMultisampleColorImage();
    if (hasDepthBuffer()) destroyDepthBuffer();
    if (hasSwapchain()) destroySwapchain();
    if (isWindowOpen()) destroySurfaceAndWindow();
}

std::tuple<vk::Image, vk::ImageView> GlfwVulkanWindow::getDepthBuffer() const {
    assert(hasDepthBuffer());
    return std::tie(std::get<0>(mDepthBuffer), std::get<1>(mDepthBuffer));
}

std::tuple<vk::Image, vk::ImageView> GlfwVulkanWindow::getMultisampledColorImage() const {
    assert(isMultisampled());
    return std::tie(std::get<0>(mMultisampleColor), std::get<1>(mMultisampleColor));
}

vk::ResultValue<GlfwVulkanWindow::SwapchainFrameBundle> GlfwVulkanWindow::acquireNextFrameBundle(uint64_t aTimeout, vk::Fence aFence)
{
    vk::resultCheck(mParentDevice.logical.waitForFences(mInFlightFences[mNextFrameIndex], true, mInFlightTimeout), "waiting on GLFW swapchain fence");
    
    const auto [result, index] = mParentDevice.logical.acquireNextImageKHR(mSwapchain, aTimeout, mAcquireSemaphores[mNextFrameIndex], aFence);

    switch (result) {
        case vk::Result::eSuboptimalKHR:
        case vk::Result::eSuccess:
            mIsSuboptimal = result == vk::Result::eSuboptimalKHR;
            assert(mNextFrameIndex == index);
            mNextFrameIndex = (mNextFrameIndex + 1) % mSwapchainImages.size();
            mParentDevice.logical.resetFences(mInFlightFences[index]);
            return vk::ResultValue<SwapchainFrameBundle>(result, SwapchainFrameBundle {
                /*.imageIndex = */ index,
                /*.acquireSemaphore = */ mAcquireSemaphores[index],
                /*.presentSemaphore = */ mRenderSemaphores[index],
                /*.inFlightFence = */ mInFlightFences[index]
            });
        default:
            return vk::ResultValue<SwapchainFrameBundle>(result, {});
    }
}

vk::Result GlfwVulkanWindow::submitNextFrameBundle(const vk::Queue& aPresentQueue, const SwapchainFrameBundle& aBundle) {
    assert((aBundle.imageIndex + 1) % mSwapchainImages.size() == mNextFrameIndex);

    try {
        vk::Result result = aPresentQueue.presentKHR(vk::PresentInfoKHR(mRenderSemaphores[aBundle.imageIndex], mSwapchain, aBundle.imageIndex));
        if (result == vk::Result::eSuboptimalKHR) {
            // Swapchain is now suboptimal, schedule recreate for later.
            mIsSuboptimal = true;
        } else mIsSuboptimal = false;
        return result;
    } catch (const vk::OutOfDateKHRError& err) {
        // Swapchain is unsuitable, must recreate...
        return vk::Result::eErrorOutOfDateKHR;
    }
}

void GlfwVulkanWindow::populateSwapchainInfo(
    const vk::SurfaceFormatKHR& aSurfFormatPreferred, const vk::PresentModeKHR& aPresentModePreferred,
    uint32_t aLengthPreferred, bool aReset)
{
    // Choose surface image format
    const std::vector<vk::SurfaceFormatKHR> formats = mParentDevice.physical.getSurfaceFormatsKHR(mSurface);
    if (formats.empty()) throw std::runtime_error("No formats are supported by this device/surface pairing!?");
    
    const bool hasPreferredFormat = std::find(formats.begin(), formats.end(), aSurfFormatPreferred) != formats.end();
    const vk::SurfaceFormatKHR surfFormat = hasPreferredFormat ? aSurfFormatPreferred : formats[0];

    if (!hasPreferredFormat) {
        if (surfFormat.format == vk::Format::eUndefined)
            throw std::runtime_error("GlfwVulkanWindow: Error, surface reports undefined format!");
        else {
            std::cerr << "GlfwVulkanWindow: Warning! Preferred surface format 'VkSurfaceFormatKHR {"
                      << vk::to_string(aSurfFormatPreferred.format) << ", " << vk::to_string(aSurfFormatPreferred.colorSpace)
                      << "}' not available, falling back to:\n" << "VkSurfaceFormatKHR:{" << vk::to_string(surfFormat.format)
                      << ", " << vk::to_string(surfFormat.colorSpace) << "}" << std::endl;
        }
    }

    // Choose surface presentation mode. Prefer relaxed FIFO, but a regular FIFO is fine. 
    const std::vector<vk::PresentModeKHR> modes = mParentDevice.physical.getSurfacePresentModesKHR(mSurface);
    const bool hasPreferredMode = std::find(modes.begin(), modes.end(), aPresentModePreferred) != modes.end();
    const vk::PresentModeKHR mode = hasPreferredMode ? aPresentModePreferred : vk::PresentModeKHR::eFifo;
    if (!hasPreferredMode) {
        std::cerr << "GlfwVulkanWindow: Warning! Preferred presentation mode '" << vk::to_string(aPresentModePreferred) << "'"
                  << "not available. Falling back to FIFO mode." << std::endl;
    }

    // Setup surface extents
    const vk::SurfaceCapabilitiesKHR capabilities = mParentDevice.physical.getSurfaceCapabilitiesKHR(mSurface);
    vk::Extent2D extent;
    if (capabilities.currentExtent != vk::Extent2D{0xFFFFFFFF, 0xFFFFFFFF}) {
        extent = capabilities.currentExtent;
    } else {
        // Surface dimensions not set correctly, get it from GLFW and clamp
        int width, height; 
        glfwGetFramebufferSize(mWindow, &width, &height);
        extent = vk::Extent2D(
            std::clamp(uint32_t(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(uint32_t(height), capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
        );
    }

    const uint32_t imageCount = (capabilities.minImageCount >= aLengthPreferred) ?
        capabilities.minImageCount : std::min(aLengthPreferred, std::max(capabilities.minImageCount, capabilities.maxImageCount));

    constexpr vk::ImageUsageFlags ceDefaultUsageFlags = vk::ImageUsageFlagBits::eColorAttachment
                                                      | vk::ImageUsageFlagBits::eTransferSrc
                                                      | vk::ImageUsageFlagBits::eTransferDst;

    if (aReset) {
        mSwapchainCreateInfo = vk::SwapchainCreateInfoKHR(
            vk::SwapchainCreateFlagsKHR(0u), // Flags
            mSurface,
            imageCount,
            surfFormat.format, surfFormat.colorSpace,
            extent,
            1, // imageArrayLayers
            ceDefaultUsageFlags, // imageUsage
            vk::SharingMode::eExclusive, {});
        mSwapchainCreateInfo.setPresentMode(mode);
    } else {
        // Restrict changes only to the values possibly changed during swapchain invalidation. 
        // This is done under the assumption that the class user may have customized the create info structure,
        // and so we should leave everything else along just in-case.
        mSwapchainCreateInfo
            .setImageExtent(extent)
            .setImageFormat(surfFormat.format)
            .setImageColorSpace(surfFormat.colorSpace)
            .setMinImageCount(imageCount);
    }

}

void GlfwVulkanWindow::populateDepthBufferInfo(const vk::Format& aDepthFormatPreferred, bool aReset) {
    const vk::Extent3D extent(mSwapchainCreateInfo.imageExtent, 1u);
    if (bool(mDepthBufferCreateInfo) && !aReset) {
        mDepthBufferCreateInfo->setExtent(extent);
        return;
    }

    vk::Format depthFormat = aDepthFormatPreferred;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;

    const vk::FormatProperties props = mParentDevice.physical.getFormatProperties(depthFormat);
    const bool optimalSupport = bool(vk::FormatFeatureFlagBits::eDepthStencilAttachment & props.optimalTilingFeatures);
    const bool linearSupport = bool(vk::FormatFeatureFlagBits::eDepthStencilAttachment & props.optimalTilingFeatures);
    if (!optimalSupport && linearSupport) {
        tiling = vk::ImageTiling::eLinear;
    } else if (!optimalSupport && !linearSupport) {
        const vk::FormatProperties props = mParentDevice.physical.getFormatProperties(vk::Format::eD32Sfloat);
        const bool optimalSupport = bool(vk::FormatFeatureFlagBits::eDepthStencilAttachment & props.optimalTilingFeatures);
        const bool linearSupport = bool(vk::FormatFeatureFlagBits::eDepthStencilAttachment & props.optimalTilingFeatures);
        depthFormat = vk::Format::eD32Sfloat;
        if (!optimalSupport && linearSupport) {
            tiling = vk::ImageTiling::eLinear;
            std::cerr << "GlfwVulkanWindow: Warning! Preferred depth format '" << vk::to_string(depthFormat) << "' not supported."
                        << "Falling back to 16-bit UNORM depth buffer." << std::endl;
            depthFormat = vk::Format::eD32Sfloat;
        } else if (!optimalSupport && !linearSupport) {
            std::cerr << "GlfwVulkanWindow: Critical Warning! Preferred depth format '" << vk::to_string(depthFormat) << "' nor fallback '"
                        << "' are supported! Depth buffer create info is invalid, and must be manually overriden or depth buffer creation will fail!"
                        << std::endl;
            depthFormat = vk::Format::eUndefined;
        }
    }

    const vk::SampleCountFlagBits sampleCount = bool(mMultisampleColorCreateInfo) ? mMultisampleColorCreateInfo->samples : vk::SampleCountFlagBits::e1;

    mDepthBufferCreateInfo = vk::ImageCreateInfo(
        {}, vk::ImageType::e2D, depthFormat, extent, 1u, 1u, sampleCount, tiling,
        vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::SharingMode::eExclusive);
}

void GlfwVulkanWindow::populateMultisampleInfo(vk::SampleCountFlagBits aSampleCount, const bool aReset) {
    const vk::Extent3D extent(mSwapchainCreateInfo.imageExtent, 1u);
    if (bool(mMultisampleColorCreateInfo) && !aReset) {
        mMultisampleColorCreateInfo->setExtent(extent);
        return;
    }

    const vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eColorAttachment
                                         | vk::ImageUsageFlagBits::eTransferSrc
                                         | vk::ImageUsageFlagBits::eTransferDst;

    mMultisampleColorCreateInfo = vk::ImageCreateInfo(
        {}, vk::ImageType::e2D, mSwapchainCreateInfo.imageFormat, extent, 1u, 1u, aSampleCount,
        vk::ImageTiling::eOptimal, usageFlags, vk::SharingMode::eExclusive);
}

void GlfwVulkanWindow::recreateDepthBuffer() {
    destroyDepthBuffer();
    populateDepthBufferInfo(mDepthBufferCreateInfo->format, false);
    createDepthBuffer();
}

void GlfwVulkanWindow::recreateMultisamplingResources() {
    destroyMultisampleColorImage();
    populateMultisampleInfo(mMultisampleColorCreateInfo->samples, false);
    createMultisampleColorImage();
}

void GlfwVulkanWindow::destroySwapchain(const bool aKeepForRecycle) {
    if (mSwapchain) {
        for (size_t i = 0; i < mSwapchainImages.size(); ++i) {
            mParentDevice.logical.destroyImageView(mSwapchainImageViews[i]);
            mParentDevice.logical.destroySemaphore(mAcquireSemaphores[i]);
            mParentDevice.logical.destroySemaphore(mRenderSemaphores[i]);
            mParentDevice.logical.destroyFence(mInFlightFences[i]);
        }
        mSwapchainImages.clear();
        mSwapchainImageViews.clear();
        mAcquireSemaphores.clear();
        mRenderSemaphores.clear();
        mInFlightFences.clear();
        if (!aKeepForRecycle) {
            mParentDevice.logical.destroySwapchainKHR(mSwapchain);
            mSwapchain = VK_NULL_HANDLE;
        }
    }
}

void GlfwVulkanWindow::destroyDepthBuffer() {
    mParentDevice.logical.destroyImageView(std::get<1>(mDepthBuffer));
    vmaDestroyImage(mAllocator, std::get<0>(mDepthBuffer), std::get<2>(mDepthBuffer));
    mDepthBuffer = {};
}

void GlfwVulkanWindow::destroyMultisampleColorImage() {
    mParentDevice.logical.destroyImageView(std::get<1>(mMultisampleColor));
    vmaDestroyImage(mAllocator, std::get<0>(mMultisampleColor), std::get<2>(mMultisampleColor));
    mMultisampleColor = {};
}

void GlfwVulkanWindow::destroySurfaceAndWindow() {
    if (isWindowOpen()) {
        mParentInstance.destroySurfaceKHR(mSurface);
        glfwDestroyWindow(mWindow);
        mWindow = nullptr;
    }
}

void GlfwVulkanWindow::release() {
    mWindow = nullptr;
    mSurface = VK_NULL_HANDLE;
    mSwapchain = VK_NULL_HANDLE;
    mSwapchainImages.clear();
    mSwapchainImageViews.clear();
    mAcquireSemaphores.clear();
    mRenderSemaphores.clear();
    mInFlightFences.clear();
    mDepthBuffer = {};
    mMultisampleColor = {};
    mParentInstance = VK_NULL_HANDLE;
    mParentDevice = {};
    mAllocator = VK_NULL_HANDLE;
}

GlfwVulkanWindowBuilder::GlfwVulkanWindowBuilder(
    vk::Instance aVulkanInstance, vk::PhysicalDevice aPhysDev,
    vk::Device aDevice, VmaAllocator aAllocator)
: mVulkanInstance(aVulkanInstance), mPhysicalDevice(aPhysDev),
  mDevice(aDevice), mAllocator(aAllocator)
{}

GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setAllocator(VmaAllocator aAllocator) { mAllocator = aAllocator; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setTitle(const char* aTitle) { mTitle = aTitle; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setDimensions(uint32_t width, uint32_t height) { mWidth = width; mHeight = height; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setDepthBufferEnabled(bool aValue) { mCreateDepthBuffer = aValue; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setSamplingCount(vk::SampleCountFlagBits aSampleCount) { mSampleCount = aSampleCount; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setGlfwMonitor(GLFWmonitor* aMonitor) { mMonitor = aMonitor; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setGlfwSharedWindow(GLFWwindow* aShareWindow) { mSharedWindow = aShareWindow; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setPreferredSurfaceFormat(const vk::SurfaceFormatKHR& aSurfFormat) { mSurfaceFormatPreferred = aSurfFormat; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setPreferredSwapchainLength(uint32_t aLength) { mSwapchainLengthPreferred = aLength; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setPreferredPresentMode(const vk::PresentModeKHR& aPresentMode) { mPresentModePreferred = aPresentMode; return *this;}
GlfwVulkanWindowBuilder& GlfwVulkanWindowBuilder::setPreferredDepthFormat(const vk::Format& aDepthFormat) { mDepthFormatPreferred = aDepthFormat; return *this;}

bool GlfwVulkanWindowBuilder::isBuildReady(bool aWarn) const { return _isBuildReady(false); }

bool GlfwVulkanWindowBuilder::_isBuildReady(bool aThrows) const {
    const auto failFn = aThrows ?
        [](const char* what) -> bool { throw std::logic_error(what); return false; }
      : [](const char* what) -> bool { std::cerr << what << std::endl; return false; };

    if (!mVulkanInstance)
        return failFn("GlfwVulkanWindowBuilder: Vulkan instance is invalid. Valid instance, physical device, and logical device are required to build window.");
    if (!mPhysicalDevice)
        return failFn("GlfwVulkanWindowBuilder: Physical device is invalid. Valid instance, physical device, and logical device are required to build window.");
    if (!mDevice)
        return failFn("GlfwVulkanWindowBuilder: Logical device is invalid. Valid instance, physical device, and logical device are required to build window.");
    
    if (mCreateDepthBuffer && mAllocator == VK_NULL_HANDLE) {
        return failFn("GlfwVulkanWindowBuilder: Depth buffer creation requested, but no memory allocator provided. "
                      "If either depth buffering of multisampling support is requested, a valid VMA allocator must be provided.");
    } else if (mSampleCount != vk::SampleCountFlagBits::e1 && mAllocator == VK_NULL_HANDLE) {
        return failFn("GlfwVulkanWindowBuilder: Sampling count is greater than 1, but no memory allocator provided. "
                      "If either depth buffering of multisampling support is requested, a valid VMA allocator must be provided.");
    }

    return true;
}

GlfwVulkanWindow GlfwVulkanWindowBuilder::build(bool aDeferRenderResources) {
    _isBuildReady(true);

    GlfwVulkanWindow window;
    window.mParentInstance = mVulkanInstance;
    window.mParentDevice = {mPhysicalDevice, mDevice};
    window.mAllocator = mAllocator;

    window.createWindowAndSurface(mWidth, mHeight, mTitle, mMonitor, mSharedWindow);

    window.populateSwapchainInfo(mSurfaceFormatPreferred, mPresentModePreferred, mSwapchainLengthPreferred);
    if (mSampleCount != vk::SampleCountFlagBits::e1) window.populateMultisampleInfo(mSampleCount);
    if (mCreateDepthBuffer) window.populateDepthBufferInfo(mDepthFormatPreferred);

    if (aDeferRenderResources) return std::move(window);
    window.createAllRenderResources();

    return std::move(window);
}
