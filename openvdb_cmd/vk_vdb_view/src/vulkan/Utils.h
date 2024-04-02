// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <variant>
#include <optional>
#include <filesystem>

// To ensure full symbol resolution, we must opt-in for the dynamic loader and allocate our own.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <vma/vk_mem_alloc.h>

/// @brief Helper function to retrieve a raw-pointer to a Vulkan HPP object's wrapped C type.
///        Often needed for calls into Vulkan libraries like VMA that operate on Vulkan C types.
template<typename VkType, typename NativeType = typename VkType::NativeType>
std::add_pointer_t<NativeType> vkpp_cptr(VkType& vk) {
    return reinterpret_cast<std::add_pointer_t<NativeType>>(&vk);
}

template<typename VkType, typename NativeType = typename VkType::NativeType>
const std::add_pointer_t<const NativeType> vkpp_cptr(const VkType& vk) {
    return reinterpret_cast<const std::add_pointer_t<const NativeType>>(&vk);
}

/// Vulkan utilities built on top of the Vulkan HPP headers, and also depending on the Vulkan 
/// memory allocator library.  
namespace vult {

using ::vkpp_cptr;

/// @brief Statically sized char array for storing a Vulkan extension name
class ExtensionName final : public std::array<char, vk::MaxExtensionNameSize>
{
public:
    using array_t = std::array<char, vk::MaxExtensionNameSize>;
    using array_t::array;

    const char* c_str() const { return data(); }
    operator const char*() const { return data(); }
};

/// @brief Reads SPIR-V bytecode from given filepath and returns it in a vector. 
std::vector<uint8_t> load_shader_bytecode(const std::filesystem::path& aFilepath);


//////////////////////////////////////////////// Wrappers and Scoping ////////////////////////////////////////////////

/// Unimplemented concept for a `WrappedHandle` class. Many classes defined in this utility
/// namespace implement this concept, but it doesn't make sense to enforce via inheritance.     
// template<typename HandleT>
// class WrappedHandle
// {
// public:
//     bool isValid() const noexcept { return mHandle != VK_NULL_HANDLE; }
//     void reset() { mHandle = VK_NULL_HANDLE; } 

//     operator bool() const noexcept { return isValid(); }
// protected:
//     HandleT mHandle = VK_NULL_HANDLE;
// };

/// @brief Pairing of a Vulkan physical device with a Vulkan logical device created from it.
///
/// WARNING: No validation is, or can be, done to verify that `logical` is a device created from `physical`.
struct DevicePair
{
    vk::PhysicalDevice physical;
    vk::Device logical;

    DevicePair() = default;
    DevicePair(const vk::PhysicalDevice aPhysical, const vk::Device aLogical) : physical(aPhysical), logical(aLogical) {}

    /// @brief Verify that both device handles are valid. Equivalent to boolean conversion. 
    bool isValid() const noexcept { return bool(*this); }

    /// @brief Resets pair to null handles
    void reset() { physical = VK_NULL_HANDLE; logical = VK_NULL_HANDLE; }

    operator bool() const { return bool(physical) && bool(logical); }
    bool operator==(const DevicePair& other) const { return bool(*this) && bool(other) && physical == other.physical && logical == other.logical; }
    bool operator!=(const DevicePair& other) const { return !(*this == other); }

    operator vk::PhysicalDevice&() { return physical; }
    operator vk::Device&() { return logical; }
    operator const vk::PhysicalDevice&() const { return physical; }
    operator const vk::Device&() const { return logical; }
};

/// Abstract class defining a typical Vulkan application's basic scope, in which  
/// a single Vulkan instance, physical/logical device, memory allocator, and some queues
/// are created, and then used to create and utilize all other Vulkan resources over the 
/// lifetime of the application. 
///
/// This class is the bare-bones implementation of this concept. Look to the `VulkanRuntimeScope`
/// derived class for a more fleshed out tool. 
class BasicVulkanScope
{
public:

    virtual vk::Instance getVulkanInstance() const = 0;
    virtual DevicePair getDevice() const = 0;
    virtual VmaAllocator getAllocator() const = 0;
    virtual vk::Queue getGraphicsQueue() const = 0;
    virtual vk::Queue getTransferQueue() const = 0;
    virtual vk::Queue getComputeQueue() const = 0;
    virtual vk::Queue getBigThreeQueue() const = 0;
    virtual vk::Queue getPresentationQueue() const = 0;

    bool hasVulkanInstance() const { return bool(getVulkanInstance()); }
    bool hasDevice() const { return bool(getDevice()); }
    bool hasAllocator() const { return getAllocator() == VK_NULL_HANDLE; }
    bool hasGraphicsQueue() const { return bool(getGraphicsQueue()); }
    bool hasTransferQueue() const { return bool(getTransferQueue()); }
    bool hasComputeQueue() const { return bool(getComputeQueue()); }
    bool hasBigThreeQueue() const { return bool(getBigThreeQueue()); }
    bool hasPresentationQueue() const { return bool(getPresentationQueue()); }
};

class DeviceBundle;
class QueueClosure;

/// Abstract class defining a typical Vulkan application's scope, in which a single
/// Vulkan instance, physical/logical device, memory allocator, and a set of queues
/// are created, and then used to create and utilize all other Vulkan resources over 
/// the lifetime of the application. 
///
/// `VulkanRuntimeScope` instances are meant to act as a central authority for a Vulkan
/// app's core resources. They should be initialized very early in an application's 
/// lifetime, and remain alive until the application exists. In addition to implementing
/// `BasicVulkanScope`, realizations of this class are intended to provide easy access to
/// `DeviceBundle` and `QueueClosure` instances owned by the runtime scope. 
///
/// The other feature of this abstract, is the implementation of automatic cleanup of child
/// objects when the scope comes to a close. Classes can implement `VulkanRuntimeScope::Child`,
/// and then be registered with this class. Registered instances will have their `cleanupVk()`
/// function called when the scope closes. Alternatively, `pfn_cleanupVk` function pointers 
/// can be registered, to cleanup resources not wrapped by a class. 
///
/// This functionality is not intended to be used for all Vulkan resources created during an 
/// application's lifetime, as this would be inefficient and hard to debug. Instead, this should 
/// be reserved for long lived resources who's cleanup is difficult or messy to encapsulate
/// within your core application cleanup. In particular, this is very useful for enabling the 
/// safe creation and use of `VulkanRuntimeSingleton`s. Classes which have a single global
/// instance, valid throughout the lifetime of a `VulkanRuntimeScope`. 
///
/// WARNING: Cleanup, does not typically trigger destruction of the child class itself. You must 
///          be careful to write your `cleanupVk()` functions and destructors to avoid double 
///          destruction/free. Typically this just involves checking that the handle for the 
///          resource you're destroying is non-null before calling it's destroy function, and 
///          resetting handles for all destroyed objects back to null afterwards. 
class VulkanRuntimeScope : virtual public BasicVulkanScope
{
public:
    // Attachment of child objects for auto-cleanup
    /////////////////////////////////////////////////////////////////
    struct Child {
        virtual void cleanupVk(const VulkanRuntimeScope& aScope) = 0;
    };
    using pfn_cleanupVk = void (*)(const VulkanRuntimeScope& aScope);

    virtual ~VulkanRuntimeScope() { closeScope(); }

    void registerChild(Child* aChild);
    void registerChild(pfn_cleanupVk aStrayChild);

    virtual void closeScope();

    // Advanced Vulkan handles
    /////////////////////////////////////////////////////////////////////
    virtual bool hasDeviceBundle() const = 0;
    virtual const DeviceBundle& getDeviceBundle() const = 0;

    virtual bool hasGraphicsQueueClosure() const = 0;
    virtual bool hasTransferQueueClosure() const = 0;
    virtual bool hasComputeQueueClosure() const = 0;
    virtual bool hasBigThreeQueueClosure() const = 0;
    virtual bool hasPresentationQueueClosure() const = 0;

    virtual QueueClosure& getGraphicsQueueClosure() = 0;
    virtual const QueueClosure& getGraphicsQueueClosure() const = 0;
    virtual QueueClosure& getTransferQueueClosure() = 0;
    virtual const QueueClosure& getTransferQueueClosure() const = 0;
    virtual QueueClosure& getComputeQueueClosure() = 0;
    virtual const QueueClosure& getComputeQueueClosure() const = 0;
    virtual QueueClosure& getBigThreeQueueClosure() = 0;
    virtual const QueueClosure& getBigThreeQueueClosure() const = 0;
    virtual QueueClosure& getPresentationQueueClosure() = 0;
    virtual const QueueClosure& getPresentationQueueClosure() const = 0;

    // Default definitions for `BasicVulkanScope` pure virtuals. Will attempt to return 
    // the requested handle by extracting it from the more advanced wrapped counterpart 
    // (i.e. extracting the `vk::Queue` from a `QueueClosure`). Can be overriden if this 
    // behavior is undesirable. 
    virtual DevicePair getDevice() const override;
    virtual vk::Queue getGraphicsQueue() const override;
    virtual vk::Queue getTransferQueue() const override;
    virtual vk::Queue getComputeQueue() const override;
    virtual vk::Queue getBigThreeQueue() const override;
    virtual vk::Queue getPresentationQueue() const override;

protected:
    using ChildVariant = std::variant<Child*, pfn_cleanupVk>;
    std::vector<ChildVariant> mChildren;
};

/// @brief Minor extension to `VulkanRuntimeScope` abstract class. Can be inherited by an application's 
///        core class, to make that class serve as its own Vulkan scope.
class VulkanAppScope : virtual public VulkanRuntimeScope
{
public:
    /// @brief Return `vk::ApplicationInfo` for this application
    virtual const vk::ApplicationInfo& getAppInfo() const = 0;
};


/// @brief Template adapting a type @p `T` to make it a singleton whose lifetime matches the lifetime of a `VulkanRuntimeScope`.
///
/// @tparam T must be constructable with signature `T(VulkanRuntimeScope&)` and define cleanup function invocable as 
///         `cleanupVk(const VulkanRuntimeScope&)` which will release/destroy Vulkan resources derived from the scope. 
template<typename T>
class VulkanRuntimeSingleton
{
public:
    VulkanRuntimeSingleton() = default;

    /// @brief Set the Vulkan runtime scope for this singleton, triggering creation and initialization of the singleton instance.
    static void setScope(VulkanRuntimeScope& aScope) {
        T*& instance = _instance();
        if (instance != nullptr) throw std::logic_error("VulkanRuntimeSingleton: Double initialization via `setScope()`");
        instance = new T(aScope);
        aScope.registerChild(_cleanup);
    }

    /// @brief Retrieve pointer to the singleton instance, or nullptr if it does not exist. 
    static T* getInstance() {
        return _instance();
    }

    static bool hasInstance() { return _instance() != nullptr; }

    // Prevent copying
    VulkanRuntimeSingleton<T>(const VulkanRuntimeSingleton<T>&) = delete;
    VulkanRuntimeSingleton<T>& operator=(const VulkanRuntimeSingleton<T>&) = delete;

private:
    static T*& _instance() {
        static T* sInstance = nullptr;
        return sInstance;
    }

    static void _cleanup(const VulkanRuntimeScope& aScope) {
        _instance()->cleanupVk(aScope);
        delete _instance();
        _instance() = nullptr;
    }
};

/// @brief Vulkan queue wrapper providing convenient access to queue information and shortcuts for common operations.
class QueueClosure
{
public:
    /// @brief Enum indicating whether a queue is unprotected, protected, or has unknown protection status.
    ///
    /// Use of protected queues is uncommon anyway, hence the lack of emphasis on providing a clear indication
    /// of protected status.
    enum ProtectedState : uint8_t {UNPROTECTED = 0, PROTECTED = 1, PROTECTION_UNKOWN = UINT8_MAX};

    /// @brief Default constructor. Creates an invalid `QueueClosure` instance.
    QueueClosure() = default;

    /// @brief Construct a closure by retrieving a queue directly from an initialized logical device,
    ///        using the queue family index and logical index.
    ///
    /// WARNING: This constructor can only retrieve queues via `vkGetDeviceQueue()`, and likewise cannot 
    ///          be used to retrieve a queue created with non-zero queue creation flags, such as is the 
    ///          case for a protected queue. The only way to retrieve those queues is through the static
    ///          `QueueClosure::getClosures()` or `DeviceBundle`'s `retrieveAllQueueClosures()`. 
    ///
    /// WARNING: No validation is done to ensure the provided queue family or index are valid. Setting either
    ///          incorrectly could cause construction failure or stranger issues with the closure later.
    QueueClosure(const DevicePair& aDevice, uint32_t aQueueFamily, uint32_t aIndex);
    /// @brief Construct a closure around an existing queue handle. Device and queue family must be provided. 
    ///
    /// WARNING: No validation is done to ensure the provided queue family is correct. Setting this incorrectly
    ///          will certainly cause issues. 
    QueueClosure(const DevicePair& aDevice, uint32_t aQueueFamily, vk::Queue aQueue);

    /// To maintain validity, when a QueueClosure is copied, the copy does not retain
    /// any internally managed or referenced command pools and command buffers, which may
    /// have been created `beginSingleSubmitCommands()`. Copying a QueueClosure while 
    /// a single-submit operation is in-progress must be avoided, as it could
    /// lead to undefined behavior if not done with extreme caution. 
    QueueClosure(const QueueClosure&);
    QueueClosure& operator=(const QueueClosure&);

    ~QueueClosure();

    bool isValid() const { return bool(mQueue); }
    operator bool() const { return bool(mQueue); }

    /// @brief Invalidates this closure, resetting it to an empty default state. 
    void reset() {
        flush();
        *this = QueueClosure();
    }

    void waitIdle() const { mQueue.waitIdle(); }

    /// Retrieve the full collection of queues created with a logical device, as a list of closures. `aDevice` must 
    /// represent an already created logical device, which was created using the queue create info provided by
    /// `aCreateInfo`
    ///
    /// NOTE: This is one of the only ways to retrieve protected queues and other queues with non-zero creation flags.
    static std::vector<QueueClosure> getClosures(const DevicePair& aDevice, const vk::ArrayProxy<vk::DeviceQueueCreateInfo>& aCreateInfos);

    bool doesGraphics() const { return bool(queueFlags() & vk::QueueFlagBits::eGraphics); }
    bool doesCompute() const { return bool(queueFlags() & vk::QueueFlagBits::eCompute); }
    bool doesTransfer() const { return bool(queueFlags() & vk::QueueFlagBits::eTransfer); }
    bool doesSparseBinding() const { return bool(queueFlags() & vk::QueueFlagBits::eSparseBinding); }
    bool doesPresentation() const;

    /// @brief True if this queue supports graphics, compute, and transfer. 
    ///        Vulkan guarantees at least one such queue family must exist on any device.
    bool doesBigThree() const { return doesGraphics() && doesCompute() && doesTransfer(); }

    /// @brief True if this queue supports graphics, compute, transfer, and presentation. 
    bool doesBigThreeAndPresentation() const {return doesBigThree() && doesPresentation();}

    bool canBeProtected() const { return bool(queueFlags() & vk::QueueFlagBits::eProtected); }
    /// Possibly indicates whether or not this queue is protected. Usually this will just return
    /// `PROTECTION_UNKOWN`, since the closure can only know for sure if constructed via the `getClosures()`
    /// static call. Protected queues are pretty niche, so you probably know if you're using them. 
    ProtectedState isProtected() const { return mIsProtected; }

    bool doesVideoDecode() const { return bool(queueFlags() & vk::QueueFlagBits::eVideoDecodeKHR); }

    vk::Queue getQueue() const { return mQueue; }
    uint32_t queueFamily() const { return mFamily; }
    vk::QueueFlags queueFlags() const { return familyProperties().queueFlags; }
    vk::QueueFamilyProperties familyProperties() const { return familyProperties2().queueFamilyProperties; }
    vk::QueueFamilyProperties2 familyProperties2() const { return mParentDevice.physical.getQueueFamilyProperties2()[mFamily]; }

    /// If known, returns index of queue within the array of all queues created from this queue
    /// family on the current device. This is unknown if the closure was created from an existing
    /// handle, in which case this function returns `UINT32_MAX`.
    uint32_t logicalIndex() const { return mLogicalIndex; }

    /// Start recording commands into a transient command buffer which will be submitted to this queue once. 
    /// A command buffer handle is returned, which the caller can then use to record their commands into. Once you've
    /// recorded what you need, call one of the `endSingleSubmitCommands(...)` functions to stop recording and execute
    /// the commands through this queue.
    ///
    /// Unless a @a aCustomPool is provided, the returned command buffer is internally allocated and managed by the 
    /// closure.
    ///
    /// These types of single-submit operations are commonly needed for one-off or infrequent operations 
    /// which require queue submission. The most typical example is for loading assets onto the GPU, which
    /// typically involves using transfer commands to copy data between staging buffers and device memory.
    ///
    /// @param aCustomPool [Optional] Specify a command pool from which the returned command buffer should be allocated.
    ///                    When provided, the caller is responsible for managing command buffer lifetime.
    [[nodiscard]] vk::CommandBuffer beginSingleSubmitCommands(vk::CommandPool aCustomPool = VK_NULL_HANDLE);

    /// Ends recording on the single-submit command buffer, submits it to the queue, then fully flushes
    /// the queue by waiting for it to go idle. This call blocks, guaranteeing that the submitted commands,
    /// as well as any prior work submitted to the queue, will have been completed before this function returns.
    ///
    /// @param aCmdBuffer Command buffer handle returned by last call to `beginSingleSubmitCommands()`. Unless
    ///        this buffer was allocated from a custom command pool, then it will be freed by this function, and
    ///        the caller should discard any handles referencing it. 
    ///
    /// @throws vk::LogicError If @a aCmdBuffer is not the command buffer returned by the preceding call to `beginSingleSubmitCommands(...)`
    /// @throws vk::SystemError If the queue submission fails, which indicates an out of memory situation or device loss
    void endSingleSubmitCommandsAndFlush(vk::CommandBuffer& aCmdBuffer);

    /// Explicitly synchronized variant of `endSingleSubmitCommands()`. Ends recording on the command buffer, and then
    /// submits it to the queue along with the provided fence, semaphores, and destination mask. This call does not wait
    /// for the submission to complete. Instead the caller must confirm via the sync primitives that the submission has 
    /// executed. 
    ///
    /// After the submission signals completion, the caller should call `signalSingleSubmitCommandsComplete()` to let the
    /// closure know that it is safe to cleanup internal objects. Failure to do so will not break the closure, but may delay
    /// future single-submit calls.
    ///
    /// @param aCmdBuffer Command buffer handle returned by last call to `beginSingleSubmitCommands()`. Unless
    ///        this buffer was allocated from a custom command pool, then it will be freed by this function, and
    ///        the caller should discard any handles referencing it. 
    /// @param aFence Fence to be signalled when the submission completes. If the caller is confident they can synchronize 
    ///               correctly without a fence, this can be passed `VK_NULL_HANDLE` without consequence
    /// @param aWaitSemaphores [Optional] Collection of semaphores supplied as `VkSubmitInfo`'s wait semaphores
    /// @param aWaitDstMasks [Optional] Collection of stage masks supplied as `VkSubmitInfo`'s wait destination masks
    /// @param aSignalSemaphores [Optional] Collection of semaphores supplied as `VkSubmitInfo`'s signal semaphores
    ///
    /// @throws vk::LogicError If @a aCmdBuffer is not the command buffer returned by the preceding call to `beginSingleSubmitCommands(...)`
    /// @throws vk::SystemError If the queue submission fails, which indicates an out of memory situation or device loss
    void endSingleSubmitCommands(
        vk::CommandBuffer& aCmdBuffer, vk::Fence aFence,
        const vk::ArrayProxy<vk::Semaphore>& aWaitSemaphores = {},
        const vk::ArrayProxy<vk::PipelineStageFlags> aWaitDstMasks = {},
        const vk::ArrayProxy<vk::Semaphore>& aSignalSemaphores = {});

    void signalSingleSubmitCommandsComplete();

    operator vk::Queue() const { return mQueue; }

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    /// @brief Causes stack overflow.
    /// WARNING: !Causes stack overflow!
    [[deprecated("Too unstable")]] [[noreturn]] void queueContinuum() volatile { queueContinuum(); }
    #pragma GCC diagnostic pop

protected:
    void flush();

    vk::Queue mQueue = VK_NULL_HANDLE;
    uint32_t mFamily = UINT32_MAX;
    uint32_t mLogicalIndex = UINT32_MAX;

    DevicePair mParentDevice;
    ProtectedState mIsProtected = PROTECTION_UNKOWN;

    bool mCommandPoolIsInternal = false;
    vk::CommandPool mCommandPool = {};
    vk::CommandBuffer mCmdBuffer = {};
};

namespace detail {

    /// Internal class which owns a contiguously allocated Vulkan structure chain, with all compile time
    /// type information erased. RTT information is retained via the Vulkan struct type enum values. 
    class TypeErasedStructureChain {
    public:
        TypeErasedStructureChain() = default;

        /// Construct type erased copy of arbitrary Vulkan structure chain
        template<typename ... ChainElements>
        TypeErasedStructureChain(const vk::StructureChain<ChainElements...>& aChain)
        : mStructTypes {ChainElements::structureType ...,},
          mChainBlob(sizeof(vk::StructureChain<ChainElements...>))
        {
            const uint8_t* srcStart = reinterpret_cast<const uint8_t*>(&aChain);
            std::copy(srcStart, std::next(srcStart, mChainBlob.size()), mChainBlob.begin());
        }

        bool isValid() const { return !mStructTypes.empty(); }

        size_t chainLength() const { return mStructTypes.size(); }
        size_t chainMemorySize() const { return mChainBlob.size(); }
        const std::vector<vk::StructureType>& structTypes() const { return mStructTypes; }

        const void* voidPtr() const { return mChainBlob.data(); }

        template<typename ... ChainElements>
        const vk::StructureChain<ChainElements...>& restore() const {
            static_assert(sizeof...(ChainElements) > 0, "Cannot restore zero-length structure chain!");
            constexpr std::array<vk::StructureType, sizeof...(ChainElements)> ceRestoreTypes {ChainElements::structureType ...,};
            if (!std::equal(mStructTypes.begin(), mStructTypes.end(), ceRestoreTypes.begin(), ceRestoreTypes.end())) {
                throw std::logic_error(
                    "vult::detail::TypeErasedStructureChain: StructureChain element types requested for `restore()` do not match the element "
                    "types of the type erased structure chain! Cannot cast safely!");
            }
            return reinterpret_cast<const vk::StructureChain<ChainElements...>&>(mChainBlob[0]);
        }

        template <typename HeadStruct>
        const HeadStruct& restoreHead() const {
            if (!isValid()) throw std::logic_error("vult::detail::TypeErasedStructureChain: Attempting to restore head structure of zero-length chain!");
            if (HeadStruct::structureType != mStructTypes[0]) {
                throw std::logic_error("vult::detail::TypeErasedStructureChain: HeadStruct type for `restoreHead()` does not match the first element "
                                       "type of the type erased structure chain! Cannot cast safely!");
            }
            return reinterpret_cast<const HeadStruct&>(mChainBlob[0]);
        }

        /// @brief Return pointer to first struct of type @a Struct in chain, or nullptr if none exists.
        ///        Requires linear search through chain. 
        template <typename Struct>
        const Struct* retrieveStruct() const {
            if (!isValid()) return nullptr;
            const vk::BaseInStructure* pStart = reinterpret_cast<const vk::BaseInStructure*>(mChainBlob.data());
            for (const vk::BaseInStructure* p = pStart; p != nullptr; p = p->pNext) {
                if (p->sType == Struct::structureType) 
                    return reinterpret_cast<const Struct*>(p);
            }
            return nullptr;
        }

    private:
        std::vector<vk::StructureType> mStructTypes;
        std::vector<uint8_t> mChainBlob;
    };
}


/// Wrapper bundling a Vulkan physical device, logical device, and contextual information
/// about their properties and creation. 
///
/// WARNING: Because device creation info cannot be retrieved after use, the only place a `DeviceBundle`
///          instance can be created, is right at the time of logical instance creation. A bundle should 
///          be created right after device creation, and then stashed and redistributed by some central
///          authority like a `VulkanRuntimeScope`. 
///
/// WARNING: Avoid passing `DeviceBundle` instances around by value, or maintaining additional copies
///          unless necessary. To safely provide convenient access to device information, `DeviceBundle`
///          instances internally store potentially large arrays of Vulkan structures, making each 
///          instance's footprint and copy cost non-trivial. Prefer `DevicePair` whenever just device 
///          handles are needed.
///
/// WARNING: There is currently no way to safely retain `pNext` structures provided in the logical device
///          create info. If you will need access to these structures, you will have to retain copies
///          manually, or stash them on the `DeviceBundle` instance by constructing the bundle using the
///          templated constructor accepting `vk::StructureChain`
class DeviceBundle : public DevicePair
{
public:
    /// @brief Default construct. Creates an **invalid** `DeviceBundle`!
    DeviceBundle() = default;

    /// @brief Uses the provided physical device and create info to instantiate a new Vulkan logical device
    ///        which is then bundled along with `aPhysical` and `aCreateInfo`. 
    ///
    /// @warning Extended creation info structures linked to `aCreateInfo` via the `pNext` pointer cannot be
    ///          retained by the `DeviceBundle` instance, and will not be accessible through this class after
    ///          logical device creation. If you need access to that information later, either save your own
    ///          copies, or use the templated `DeviceBundle` constructor instead. 
    DeviceBundle(const vk::PhysicalDevice aPhysical, const vk::DeviceCreateInfo& aCreateInfo);

    /// Takes a physical device and a structure chain starting with `vk::DeviceCreateInfo`, and uses it to 
    /// create a new Vulkan logical device, which is then bundled along with `aPhysical` all creation info
    /// provided by the chain.
    ///
    /// WARNING: Notice that `DeviceBundle` is not a class template! The templated type of `aCreateChain` gets erased after it is 
    ///          consumed by this constructor. If you need to access any member of the chain other than `vk::DeviceCreateInfo` or
    ///          `PhysicalDeviceVulkanXxFeatures`, you must be prepared to provide the exact same sequence of `ChainElements...`
    ///          parameters to the `getCreationChain()` function. Using a type alias to keep track of the exact `vk::StructureChain`
    ///          instantiation is recommended. 
    template <typename Base = vk::DeviceCreateInfo, typename ... ChainElements>
    DeviceBundle(const vk::PhysicalDevice aPhysical, const vk::StructureChain<vk::DeviceCreateInfo, ChainElements...>& aCreateChain);

    /// @brief Destroys logical device and resets DeviceBundle to default invalid state. 
    void destroy() { logical.destroy(); reset(); }

    /// @brief Retrieve a specific queue from the device as a `QueueClosure`. Safety checks included for debug builds.
    ///
    /// WARNING: This function cannot be used to retrieve protected queues or other queues with non-zero creation flags.
    QueueClosure retrieveQueueClosure(uint32_t aFamily, uint32_t aIndex) const;

    /// @brief Retrieve a list of `QueueClosure` objects representing all the queues created with this device. 
    ///
    /// NOTE: This is one of the only ways to retrieve protected queues and other queues with non-zero creation flags.
    std::vector<QueueClosure> retrieveAllQueueClosures() const { return QueueClosure::getClosures(*this, mQueueCreateInfos); }

    /// @brief Returns true if the extension with the given name is enabled.
    bool extensionEnabled(const char* aExtName) const;

    const vk::DeviceCreateInfo& getCreateInfo() const { return mCreateInfo; }
    const std::vector<vk::DeviceQueueCreateInfo>& getQueueCreateInfos() const { return mQueueCreateInfos; };
    const std::vector<ExtensionName>& getEnabledExtensions() const { return mEnabledExtensions; }
    /// @brief Returns physical device features struct if one was supplied during device creation, and returns `std::nullopt` otherwise
    const std::optional<vk::PhysicalDeviceFeatures>& getVulkanFeatures() const { return mEnabledFeatures; }
    /// @brief Returns physical device 1.1 features struct if one was supplied during device creation, and returns `std::nullopt` otherwise
    const std::optional<vk::PhysicalDeviceVulkan11Features>& getVulkan11Features() const { return mEnabledVulkan11Features;}
    /// @brief Returns physical device 1.2 features struct if one was supplied during device creation, and returns `std::nullopt` otherwise
    const std::optional<vk::PhysicalDeviceVulkan12Features>& getVulkan12Features() const { return mEnabledVulkan12Features;}
    /// @brief Returns physical device 1.3 features struct if one was supplied during device creation, and returns `std::nullopt` otherwise
    const std::optional<vk::PhysicalDeviceVulkan13Features>& getVulkan13Features() const { return mEnabledVulkan13Features;}

    /// @brief Returns true if this `DeviceBundle` had it's full creation info structure chain stashed and available for inspection. 
    bool hasCreationChain() const { return mCreateChain.isValid(); }

    /// Retrieves a cref to the chain of Vulkan structures used to create this logical device. For this function to work, the device 
    /// __must__ have been created using the templated `DeviceBundle` structure chain based constructor, __and__ the sequence of types
    /// provided as template parameters much __exactly__ match the type parameters of the `vk::StructureChain<>` passed to that constructor.
    ///
    /// @throws std::logic_error If this `DeviceBundle` was not created using a structure chain __or__ if the signature of the structure
    ///         chain requested from this function does not match the original structure chain. 
    template <typename Base = vk::DeviceCreateInfo, typename ... ChainElements>
    const vk::StructureChain<vk::DeviceCreateInfo, ChainElements...>& getCreationChain() const {
        static_assert(std::is_same_v<Base, vk::DeviceCreateInfo>, "First element of structure chain must be `vk::DeviceCreateInfo`!");
        if (!mCreateChain.isValid()) throw std::logic_error("Attempted to get creation chain of DeviceBundle that has none!");
        return mCreateChain.restore<vk::DeviceCreateInfo, ChainElements...>();
    }

    /// Retrieve pointer to a specific structure from the stashed logical device creation info structure chain. 
    /// Returns `nullptr` if there is no stashed structure chain, or if the structure chain does not contain the 
    /// requested structure. 
    template <typename Struct>
    const Struct* getCreationStruct() const {
        return mCreateChain.retrieveStruct<Struct>();
    }

    operator vk::PhysicalDevice&() { return physical; }
    operator vk::Device&() { return logical; }
    operator const vk::PhysicalDevice&() const { return physical; }
    operator const vk::Device&() const { return logical; }

protected:
    vk::DeviceCreateInfo mCreateInfo;
    std::vector<vk::DeviceQueueCreateInfo> mQueueCreateInfos;
    std::vector<ExtensionName> mEnabledExtensions;
    std::optional<vk::PhysicalDeviceFeatures> mEnabledFeatures = std::nullopt;
    std::optional<vk::PhysicalDeviceVulkan11Features> mEnabledVulkan11Features = std::nullopt;
    std::optional<vk::PhysicalDeviceVulkan12Features> mEnabledVulkan12Features = std::nullopt;
    std::optional<vk::PhysicalDeviceVulkan13Features> mEnabledVulkan13Features = std::nullopt;

    detail::TypeErasedStructureChain mCreateChain;
private:
    std::vector<const char*> mExtensionNamesPtrs;
};

template <typename Base, typename ... ChainElements>
DeviceBundle::DeviceBundle(const vk::PhysicalDevice aPhysical, const vk::StructureChain<vk::DeviceCreateInfo, ChainElements...>& aCreateChain)
: DeviceBundle(aPhysical, std::get<0>(aCreateChain))
{
    static_assert(std::is_same_v<Base, vk::DeviceCreateInfo>, "First element of `aCreateChain` structure chain must be `vk::DeviceCreateInfo`!");
    mCreateChain = detail::TypeErasedStructureChain(aCreateChain);
    mCreateInfo.setPNext(mCreateChain.isValid() ? mCreateChain.voidPtr() : nullptr);
}


/// @brief Singleton class providing access to global Vulkan resources
///
/// This class is intended to provide a means for Vulkan applications to easily access the handles
/// of their core Vulkan API objects from anywhere. Core objects being the Vulkan instance, chosen
/// physical device, derived logical device, and queues. All of which have nearly static lifetimes.
///
/// This class is essentially just a global reference to a `VulkanRuntimeScope` instance, which is 
/// initialized by the call to `setScope()`. Only be a single global scope throughout the
/// application's lifetime, so the scope cannot be replaced or unset. The `VulkanRuntimeScope`
/// instance used as a global scope is the responsibility of the developer to design, initialize,
/// and manage the lifetime of. It should be initialized early in the application's lifetime, and 
/// live until just before exit. It often works well to have a central "application" class
/// instantiated by `int main(...)` which implements `VulkanRuntimeScope`, and is then set as the
/// global Vulkan scope. 
/// 
/// @warning This class is not thread safe, and the scope should be set prior to any multithreading.
///          Once the scope is set, calls themselves are safe, but usage of the returned handles
///          often require synchronization to ensure thread safety.
/// @warning This is not a catch all solution, The same warnings from `BasicVulkanScope`'s doc-comment
///          applies. Furthermore, know that not all Vulkan applications benefit from using this type
///          of design. Vulkan applications using lots of host multithreading and/or asynchrony
///          should probably implement their own construct for managing these resources. 
class GlobalVulkanRuntimeScope
: public VulkanRuntimeSingleton<GlobalVulkanRuntimeScope>,
  virtual public VulkanRuntimeScope,
  virtual public VulkanRuntimeScope::Child
{
public:
    virtual void cleanupVk(const VulkanRuntimeScope&) override {};

    virtual vk::Instance getVulkanInstance() const override { return mScope.getVulkanInstance(); }
    virtual DevicePair getDevice() const override { return mScope.getDevice(); }
    virtual VmaAllocator getAllocator() const override { return mScope.getAllocator(); }
    virtual vk::Queue getGraphicsQueue() const override { return mScope.getGraphicsQueue(); }
    virtual vk::Queue getTransferQueue() const override { return mScope.getTransferQueue(); }
    virtual vk::Queue getComputeQueue() const override { return mScope.getComputeQueue(); }
    virtual vk::Queue getBigThreeQueue() const override { return mScope.getBigThreeQueue(); }
    virtual vk::Queue getPresentationQueue() const override { return mScope.getPresentationQueue(); }
    virtual bool hasDeviceBundle() const override { return mScope.hasDeviceBundle(); }
    virtual const DeviceBundle& getDeviceBundle() const override { return mScope.getDeviceBundle(); }

    virtual bool hasGraphicsQueueClosure() const override { return mScope.hasGraphicsQueueClosure(); }
    virtual bool hasTransferQueueClosure() const override { return mScope.hasTransferQueueClosure(); }
    virtual bool hasComputeQueueClosure() const override { return mScope.hasComputeQueueClosure(); }
    virtual bool hasBigThreeQueueClosure() const override { return mScope.hasBigThreeQueueClosure(); }
    virtual bool hasPresentationQueueClosure() const override { return mScope.hasPresentationQueueClosure(); }

    virtual QueueClosure& getGraphicsQueueClosure() override { return mScope.getGraphicsQueueClosure(); }
    virtual const QueueClosure& getGraphicsQueueClosure() const override { return mScope.getGraphicsQueueClosure(); }
    virtual QueueClosure& getTransferQueueClosure() override { return mScope.getTransferQueueClosure(); }
    virtual const QueueClosure& getTransferQueueClosure() const override { return mScope.getTransferQueueClosure(); }
    virtual QueueClosure& getComputeQueueClosure() override { return mScope.getComputeQueueClosure(); }
    virtual const QueueClosure& getComputeQueueClosure() const override { return mScope.getComputeQueueClosure(); }
    virtual QueueClosure& getBigThreeQueueClosure() override { return mScope.getBigThreeQueueClosure(); }
    virtual const QueueClosure& getBigThreeQueueClosure() const override { return mScope.getBigThreeQueueClosure(); }
    virtual QueueClosure& getPresentationQueueClosure() override { return mScope.getPresentationQueueClosure(); }
    virtual const QueueClosure& getPresentationQueueClosure() const override { return mScope.getPresentationQueueClosure(); }

    // Prevent copying
    GlobalVulkanRuntimeScope(const GlobalVulkanRuntimeScope&) = delete;
    GlobalVulkanRuntimeScope& operator=(const GlobalVulkanRuntimeScope&) = delete;
    
private:
    friend class VulkanRuntimeSingleton<GlobalVulkanRuntimeScope>;
    GlobalVulkanRuntimeScope(VulkanRuntimeScope& aScope) : mScope(aScope) {}

    VulkanRuntimeScope& mScope;
};


/////////////////////////////////////////////// Device Creation Helpers ///////////////////////////////////////////////

/// @brief std::function type taking a const-reference to `vk::PhysicalDevice` and returning an `bool` indicating whether
///        or not it meets the requirements of the caller. 
using DeviceFilteringFn = std::function<bool(const vk::PhysicalDevice&)>;

/// @brief std::function type taking a const-reference to `vk::PhysicalDevice` and returning an `int` indicating
///        its ranking relative to other devices. 
using DeviceRankingFn = std::function<int(const vk::PhysicalDevice&)>;

/// @brief Naive ranking by device type. For a description of device types see
///
/// Ranking simply maps device types to values:
///     * discrete GPU -> 4
///     * virtual GPU -> 3
///     * integrated GPU -> 2
///     * CPU -> 1
///     * other -> -1
int rank_by_device_type(const vk::PhysicalDevice& aPhysDevice);

/// @brief Default device filter, which accepts any device.
inline bool no_device_filter(const vk::PhysicalDevice&) { return true; }

/// @brief Retrieve list of all Vulkan physical devices, optionally filtered by @a aFilterFn, and ordered according to 
///        either a generic or custom ranking scheme. Sorting by rank is done prior to filtering, so side-effects 
///        of @a aFilterFn may be used to stash information about accepted devices without risk of reordering.
std::vector<vk::PhysicalDevice> get_filtered_and_ranked_physical_devices(
    const vk::Instance& aInstance,
    DeviceFilteringFn aFilterFn = no_device_filter,
    DeviceRankingFn aRankFn = rank_by_device_type);

/// @brief Scan all device queue families for those supporting the flags in `aRequired` and at least `aMinQueueCount` queues.
///
/// @param aRequired Bitmask of required queue flags
/// @param aMinQueueCount [Default = 1] Minimum number of queues each family must support
/// @param aFilterFn [Optional] Custom function taking queue family index and returning true if acceptable.
/// @returns Vector of indices of queue families supporting the requested flags and count. 
std::vector<uint32_t> get_supported_queue_family_indices(
    const vk::PhysicalDevice& aPhysDevice,
    vk::QueueFlags aRequired,
    const uint32_t aMinQueueCount = 1,
    std::function<bool(uint32_t)> aFilterFn = [](uint32_t){ return true; });

} // namespace vult
