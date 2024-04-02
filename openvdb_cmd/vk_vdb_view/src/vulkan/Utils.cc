// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
#define VMA_IMPLEMENTATION
#include "Utils.h"

#include <vma/vk_mem_alloc.h>
#include <cassert>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <numeric>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace vult {

namespace fs = std::filesystem;

std::vector<uint8_t> load_shader_bytecode(const fs::path& aFilepath) {
    std::ifstream shaderFile(aFilepath, std::ios::in | std::ios::binary | std::ios::ate);
    if(!shaderFile.is_open()){
        perror(aFilepath.c_str());
        throw std::runtime_error("Failed to open SPIR-V file" + aFilepath.string() + "!");
    }
    size_t fileSize = static_cast<size_t>(shaderFile.tellg());
    std::vector<uint8_t> byteCode(fileSize);
    shaderFile.seekg(std::ios::beg);
    shaderFile.read(reinterpret_cast<char*>(byteCode.data()), fileSize);
    shaderFile.close();

    return byteCode;
}

//////////////////////////////////////////////// Wrappers and Scoping ////////////////////////////////////////////////

void VulkanRuntimeScope::registerChild(Child* aChild) {
    mChildren.push_back(aChild);
}

void VulkanRuntimeScope::registerChild(pfn_cleanupVk aStrayChild) {
    mChildren.push_back(aStrayChild);
}

void VulkanRuntimeScope::closeScope() {
    for (ChildVariant& child : mChildren) {
        if (child.index() == 0) std::get<Child*>(child)->cleanupVk(*this);
        else std::get<pfn_cleanupVk>(child)(*this);
    }
    mChildren.clear();
}

DevicePair VulkanRuntimeScope::getDevice() const {
    if (hasDeviceBundle()) return getDeviceBundle();
    else return DevicePair();
}
vk::Queue VulkanRuntimeScope::getGraphicsQueue() const {
    if (hasGraphicsQueueClosure()) return getGraphicsQueueClosure();
    else return VK_NULL_HANDLE;
}
vk::Queue VulkanRuntimeScope::getTransferQueue() const {
    if (hasTransferQueueClosure()) return getTransferQueueClosure();
    else return VK_NULL_HANDLE;
}
vk::Queue VulkanRuntimeScope::getComputeQueue() const {
    if (hasComputeQueueClosure()) return getComputeQueueClosure();
    else return VK_NULL_HANDLE;
}
vk::Queue VulkanRuntimeScope::getBigThreeQueue() const {
    if (hasBigThreeQueueClosure()) return getBigThreeQueueClosure();
    else return VK_NULL_HANDLE;
}
vk::Queue VulkanRuntimeScope::getPresentationQueue() const {
    if (hasPresentationQueueClosure()) return getPresentationQueueClosure();
    else return VK_NULL_HANDLE;
}

QueueClosure::QueueClosure(const DevicePair& aDevice, uint32_t aQueueFamily, uint32_t aIndex)
: mQueue(aDevice.logical.getQueue(aQueueFamily, aIndex)),
  mFamily(aQueueFamily),
  mLogicalIndex(aIndex),
  mParentDevice(aDevice)
{}

QueueClosure::QueueClosure(const DevicePair& aDevice, uint32_t aQueueFamily, vk::Queue aQueue)
: mQueue(aQueue),
  mFamily(aQueueFamily),
  mParentDevice(aDevice)
{}

QueueClosure::QueueClosure(const QueueClosure& aOther)
: mQueue(aOther.mQueue),
  mFamily(aOther.mFamily),
  mLogicalIndex(aOther.mLogicalIndex),
  mParentDevice(aOther.mParentDevice),
  mIsProtected(aOther.isProtected())
{}

QueueClosure& QueueClosure::operator=(const QueueClosure& aOther) {
    new (this) QueueClosure(aOther);
    return *this;
}


// Disable -Wterminate/-Wexception warning temporarily. This logic error is a critical safety check,
// and should be enforced even if it means killing the app when violated. 
#pragma GCC diagnostic push
#if defined (__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wterminate"
#elif defined(__clang__) && defined(__llvm__)
#pragma GCC diagnostic ignored "-Wexceptions"
#endif

QueueClosure::~QueueClosure() {
    if (mQueue) mQueue.waitIdle();
    if (mCmdBuffer || mCommandPool) {
        throw std::logic_error("QueueClosure is being destroyed while a single-submit operation is either recording or possibly still in flight!\n"
                               "This is illegal, as it can destroy Vulkan objects currently in-use. Check you synchronization and make sure you're "
                               "not making copies of your QueueClosure prior to completing a single-submit operation!");
    }
}

#pragma GCC diagnostic pop

std::vector<QueueClosure> QueueClosure::getClosures(const DevicePair &aDevice, const vk::ArrayProxy<vk::DeviceQueueCreateInfo>& aCreateInfos) {
    assert(aDevice.isValid());
     const uint32_t totalQueues = std::accumulate(aCreateInfos.begin(), aCreateInfos.end(), 0u,
        [](const uint32_t a, const vk::DeviceQueueCreateInfo& v) {
            return a + v.queueCount;
    });
    std::vector<QueueClosure> closures; closures.reserve(totalQueues);
    for (const vk::DeviceQueueCreateInfo& info : aCreateInfos) {
        for (uint32_t i = 0; i < info.queueCount; ++i) {
            vk::Queue q = aDevice.logical.getQueue2(vk::DeviceQueueInfo2(info.flags, info.queueFamilyIndex, i));
            QueueClosure& cl = closures.emplace_back(aDevice, info.queueFamilyIndex, q);
            cl.mLogicalIndex = i;
            cl.mIsProtected = bool(info.flags & vk::DeviceQueueCreateFlagBits::eProtected) ? PROTECTED : UNPROTECTED;
        }
    }

    return closures;
}

vk::CommandBuffer QueueClosure::beginSingleSubmitCommands(vk::CommandPool aCustomPool) {
    // An existing command pool is in 
    if (mCommandPool) {
        #ifndef NDEBUG
        std::cerr << "Warning! QueueClosure around VkQueue(" << mQueue << ") made a call to beginSingleSubmitCommands(..)\n"
                  << "         while a prior single-submit operation may have still been in flight! Forcing a queue flush..."
                  << "         Be sure you are calling signalSingleSubmitCommandsComplete() manually once your single-submit\n"
                  << "         operation has signalled completion, or use endSingleSubmitCommandsAndFlush(..) to make the\n"
                  << "         operation blocking." << std::endl;
        #endif 
        flush();
    }

    if (!aCustomPool) {
        mCommandPoolIsInternal = true;
        // Create transient command pool internally to use just for the duration of this single queue submission. 
        mCommandPool = mParentDevice.logical.createCommandPool(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, mFamily));
    } else {
        mCommandPoolIsInternal = false;
        mCommandPool = aCustomPool;
    }

    // Allocate a single command buffer, begin recording, and return it to the caller so they can record their own commands. 
    vk::CommandBufferAllocateInfo allocInfo = vk::CommandBufferAllocateInfo(mCommandPool, vk::CommandBufferLevel::ePrimary, 1u);
    mCmdBuffer = mParentDevice.logical.allocateCommandBuffers(allocInfo)[0];
    mCmdBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    return mCmdBuffer;
}

void QueueClosure::endSingleSubmitCommandsAndFlush(vk::CommandBuffer& aCmdBuffer) {
    if (aCmdBuffer != mCmdBuffer) throw vk::LogicError("Single-submit command buffer is not the one began with!");

    // End recording and submit
    aCmdBuffer.end();
    try {
        mQueue.submit(vk::SubmitInfo({}, {}, aCmdBuffer, {}));
    } catch (vk::DeviceLostError& devLostErr) {
        throw devLostErr;
    } catch (vk::SystemError& submitSystemErr) {
        flush();
        throw submitSystemErr;
    }
    // Nullify caller command buffer handle to prevent caller from trying to use the buffer again.
    if (mCommandPoolIsInternal) aCmdBuffer = VK_NULL_HANDLE;
    
    flush();
    mCmdBuffer = VK_NULL_HANDLE;
}

void QueueClosure::endSingleSubmitCommands(
    vk::CommandBuffer& aCmdBuffer, vk::Fence aFence,
    const vk::ArrayProxy<vk::Semaphore>& aWaitSemaphores,
    const vk::ArrayProxy<vk::PipelineStageFlags> aWaitDstMasks,
    const vk::ArrayProxy<vk::Semaphore>& aSignalSemaphores)
{
    if (aCmdBuffer != mCmdBuffer) throw vk::LogicError("Single-submit command buffer is not the one began with!");

    // End recording and submit
    aCmdBuffer.end();
    try {
        mQueue.submit(vk::SubmitInfo(aWaitSemaphores, aWaitDstMasks, aCmdBuffer, aSignalSemaphores), aFence);
    } catch (vk::DeviceLostError& devLostErr) {
        throw devLostErr;
    } catch (vk::SystemError& submitSystemErr) {
        flush();
        throw submitSystemErr;
    }
    // Nullify caller command buffer handle to prevent caller from trying to use the buffer again.
    if (mCommandPoolIsInternal) aCmdBuffer = VK_NULL_HANDLE;
    mCmdBuffer = VK_NULL_HANDLE;
}

void QueueClosure::signalSingleSubmitCommandsComplete() {
    if (mCommandPoolIsInternal) {
        mParentDevice.logical.freeCommandBuffers(mCommandPool, mCmdBuffer);
        mParentDevice.logical.destroyCommandPool(mCommandPool);
        mCommandPool = VK_NULL_HANDLE;
        mCmdBuffer = VK_NULL_HANDLE;
    }
}

void QueueClosure::flush() {
    mQueue.waitIdle();

    if (mCommandPoolIsInternal) {
        if (mCommandPool) mParentDevice.logical.destroyCommandPool(mCommandPool);
    }
    mCommandPool = VK_NULL_HANDLE;
    mCmdBuffer = VK_NULL_HANDLE;
}

DeviceBundle::DeviceBundle(const vk::PhysicalDevice aPhysical, const vk::DeviceCreateInfo& aCreateInfo)
: DevicePair(aPhysical, aPhysical.createDevice(aCreateInfo)),
  mCreateInfo(aCreateInfo),
  mQueueCreateInfos(aCreateInfo.pQueueCreateInfos, std::next(aCreateInfo.pQueueCreateInfos, aCreateInfo.queueCreateInfoCount)),
  mEnabledExtensions(aCreateInfo.enabledExtensionCount),
  mExtensionNamesPtrs(aCreateInfo.enabledExtensionCount)
{
    for (size_t i = 0; i < mEnabledExtensions.size(); ++i) {
        const char* src = aCreateInfo.ppEnabledExtensionNames[i];
        std::strncpy(mEnabledExtensions[i].data(), src, vk::MaxExtensionNameSize);
    }

    // Lexicographically sort extension names so they can be binary searched
    std::sort(mEnabledExtensions.begin(), mEnabledExtensions.end(), [](const auto& a, const auto& b) {
        return std::strcmp(a, b) < 0;
    });

    // Form secondary vector of `const char*`. The owned extension names themselves cannot be
    // passed to Vulkan. 
    for (const auto& name : mEnabledExtensions) {
        mExtensionNamesPtrs.push_back(name.c_str());
    }

    // Make locally owned copies of device feature structures
    if (mCreateInfo.pEnabledFeatures != nullptr) mEnabledFeatures = *mCreateInfo.pEnabledFeatures;
    const vk::BaseInStructure* createInfoHead = reinterpret_cast<const vk::BaseInStructure*>(mCreateInfo.pNext);
    for (const vk::BaseInStructure* p = createInfoHead; p != nullptr; p = p->pNext) {
        if (p->sType == vk::StructureType::ePhysicalDeviceVulkan11Features)
            mEnabledVulkan11Features = (*reinterpret_cast<const vk::PhysicalDeviceVulkan11Features*>(p));
        else if (p->sType == vk::StructureType::ePhysicalDeviceVulkan12Features)
            mEnabledVulkan12Features = (*reinterpret_cast<const vk::PhysicalDeviceVulkan12Features*>(p));
        else if (p->sType == vk::StructureType::ePhysicalDeviceVulkan13Features)
            mEnabledVulkan13Features = (*reinterpret_cast<const vk::PhysicalDeviceVulkan13Features*>(p)); 
    }

    // Replace original pointers with references to the internally owned copies
    mCreateInfo.setQueueCreateInfos(mQueueCreateInfos);
    mCreateInfo.setPpEnabledExtensionNames(mExtensionNamesPtrs.data());
    mCreateInfo.setPEnabledFeatures(mEnabledFeatures ? &mEnabledFeatures.value() : nullptr);
    
    // Nullify pNext. There is no way to safely retain the linked list after logical device creation. 
    mCreateInfo.setPNext(nullptr);
}

QueueClosure DeviceBundle::retrieveQueueClosure(uint32_t aFamily, uint32_t aIndex) const {
    #ifndef NDEBUG
    bool isValidQueue = false;
    for (const vk::DeviceQueueCreateInfo& info : mQueueCreateInfos) {
        const bool flagsZeroed = info.flags == vk::DeviceQueueCreateFlags(0u);
        if (flagsZeroed && info.queueFamilyIndex == aFamily && aIndex < info.queueCount) {
            isValidQueue = true;
            break;
        }
    }
    assert(isValidQueue);
    #endif
    
    return QueueClosure(*this, aFamily, aIndex);
}

bool DeviceBundle::extensionEnabled(const char* aExtName) const {
    return std::binary_search(mEnabledExtensions.cbegin(), mEnabledExtensions.cend(), aExtName,
        [](const char* a, const char* b){
            return std::strcmp(a, b) < 0;
    });
}


/////////////////////////////////////////////// Device Creation Helpers ///////////////////////////////////////////////

std::vector<vk::PhysicalDevice> get_filtered_and_ranked_physical_devices(
    const vk::Instance& aInstance,
    DeviceFilteringFn aFilterFn,
    DeviceRankingFn aRankFn)
{
    std::vector<vk::PhysicalDevice> unfiltered = aInstance.enumeratePhysicalDevices();
    std::sort(unfiltered.begin(), unfiltered.end(),
        [&](const vk::PhysicalDevice& a, const vk::PhysicalDevice& b) {return aRankFn(a) > aRankFn(b);});
    
    std::vector<vk::PhysicalDevice> devices;
    for (const vk::PhysicalDevice& device : aInstance.enumeratePhysicalDevices()) {
        if (aFilterFn(device)) devices.push_back(device);
    }

    return devices;
}

std::vector<uint32_t> get_supported_queue_family_indices(
    const vk::PhysicalDevice& aPhysDevice,
    vk::QueueFlags aRequired,
    const uint32_t aMinQueueCount,
    std::function<bool(uint32_t)> aFilterFn
){
    std::vector<uint32_t> indices;
    uint32_t i = 0;
    for (const vk::QueueFamilyProperties& queueProps : aPhysDevice.getQueueFamilyProperties()) {
        if ((queueProps.queueFlags & aRequired) == aRequired && queueProps.queueCount >= aMinQueueCount && aFilterFn(i)) {
            indices.push_back(i);
        }
        ++i;
    }
    return indices;
}

int rank_by_device_type(const vk::PhysicalDevice& aPhysDevice) {
    switch (aPhysDevice.getProperties().deviceType) {
        case vk::PhysicalDeviceType::eDiscreteGpu:
            return 4;
        case vk::PhysicalDeviceType::eVirtualGpu:
            return 3;
        case vk::PhysicalDeviceType::eIntegratedGpu:
            return 2;
        case vk::PhysicalDeviceType::eCpu:
            return 1;
        case vk::PhysicalDeviceType::eOther:
            return -1;
        default:
            throw std::logic_error("Unreachable default case for vk::PhysicalDeviceType hit!");
    }
}

} // end namespace vult
