// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "Utils.h"
#include <vma/vk_mem_alloc.h>

#include <iostream>

namespace vult {

/// @brief Runtime error thrown when calling functions on invalid instances of `Buffer` and its derived classes. 
///
/// NOTE: In most cases, `Buffer` and derived classes opt to safely do nothing or return an invalid value when
///       their functions are called while the object is invalid. This error is reserved for more severe cases 
///       where attempting to proceed would be totally undefined behavior or likely to cause device loss. All
///       member functions that throw this error type are documented as such.
class InvalidResourceError : public vk::Error, std::runtime_error
{
public:
    explicit InvalidResourceError( const std::string& what) : Error(), std::runtime_error(what) {}
    explicit InvalidResourceError( char const* what) : Error(), std::runtime_error(what) {}
    virtual const char* what() const noexcept override {
        return std::runtime_error::what();
    }
};

/// @brief Runtime error thrown when attempting an operation which goes out of the valid range of a buffer resource.
class OutOfRangeResourceError : public vk::Error, std::runtime_error
{
public:
    explicit OutOfRangeResourceError( const std::string& what) : Error(), std::runtime_error(what) {}
    explicit OutOfRangeResourceError( char const* what) : Error(), std::runtime_error(what) {}
    virtual const char* what() const noexcept override {
        return std::runtime_error::what();
    }
};

class MappableBuffer;
class UploadStagedBuffer;

/// @brief Minimally wraps Vulkan buffer together with it's VMA memory allocation. Moveable, not copyable. 
class Buffer
{
public:
    /// @brief Construct invalid `Buffer` object with no associated buffer or memory allocation. 
    Buffer() = default;

    /// Create, allocate, and bind memory to new buffer of the provided size and usage flags. Memory type is selected 
    /// automatically by the VMA based on your usage flags. This auto selection is highly reliable and should be overridden
    /// only in very marginal cases. 
    Buffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage);
    /// Create a new buffer using the provided create info, then allocate and bind memory to the buffer. Memory type is
    /// selected automatically by the VMA based on your buffer's creation info. This auto selection is highly reliable and
    /// should be overridden only in very marginal cases. 
    Buffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo);
    /// Constructor for maximum specificity. Create, allocate, and bind memory to a new buffer by passing the provided buffer
    /// creation info and VMA allocation create info directly to @a aAllocator. Should only be necessary for highly specific
    /// and unusual use cases. For creating host accessible buffers, use `MappableBuffer` instead.
    Buffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo);

    virtual ~Buffer() { reset(); }
    Buffer(Buffer&& aOther);
    Buffer& operator=(Buffer&& aOther);

    // Specialized move operations for staged buffers which ensure the staging buffer 
    // is freed, while the device local buffer is maintained.
    Buffer(UploadStagedBuffer&&);
    Buffer& operator=(UploadStagedBuffer&&);

    // Specialized move operations for mappable buffers which ensures that the memory is unmapped
    // during the move, as to not leave a dangling mapped pointer.
    Buffer(MappableBuffer&&);
    Buffer& operator=(MappableBuffer&&);

    /// @brief Returns true if this object represents a valid Vulkan buffer with memory bound.
    bool isValid() const { return bool(mBuffer); }    
    /// @brief Free any buffer resources, and return object to invalid state. 
    void reset();

    operator bool() const { return isValid(); }

    /// @brief Retrieve buffer handle, or VK_NULL_HANDLE if `Buffer` is invalid. 
    vk::Buffer buffer() const { return mBuffer; }
    /// @brief Retrieve memory allocation handle. 
    VmaAllocation allocation() const { return mAllocation; }
    /// @brief Size of buffer in bytes or 0 if `Buffer` is invalid. 
    /// NOTE: Actual memory allocation size may be larger than `bufferSize()`.
    vk::DeviceSize bufferSize() const { return mSize; }
    /// @brief Usage flags with which the buffer was created, or 0 if `Buffer` is invalid
    vk::BufferUsageFlags usage() const { return mUsage; }
    /// @brief Retrieve VMA allocation info structure.
    /// @throws InvalidResourceError if called on an invalid buffer.
    VmaAllocationInfo getAllocInfo() const;

    /// Releases ownership of the `vk::Buffer` wrapped by this class instance, along with its 
    /// bound memory allocation. Both handles are returned to the caller, who is now responsible
    /// for keeping track of their state and destroying/freeing both. The class instance is 
    /// reset back to the invalid state by this call.
    [[nodiscard]] std::pair<vk::Buffer, VmaAllocation> release();

    operator const vk::Buffer&() const { return mBuffer; }

protected:
    // Protected tag-guarded construct while allows derived classes to delegate init of mAllocator, mSize, and mUsage while
    // taking care of buffer creation and allocation themselves. 
    struct _DerivedInitTag{};
    explicit Buffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage, _DerivedInitTag);

    /// Return object to invalid state, without freeing any resources. Used to safely nullify
    /// the RHS of a move operation.
    void releaseReset();

    __always_inline void throwIfInvalid(const char* what = "Critical function called on invalid vult::Buffer object.") const {
        if (!isValid()) throw InvalidResourceError(what);
    }

    // Protect copy calls so that they can only be used responsibly during a move. 
    Buffer(const Buffer&) = default;
    Buffer& operator=(const Buffer&) = default;

    VmaAllocator mAllocator = VK_NULL_HANDLE;
    vk::Buffer mBuffer;
    vk::DeviceSize mSize = 0ul;
    vk::BufferUsageFlags mUsage = {};
    VmaAllocation mAllocation = VK_NULL_HANDLE;
};

/// @brief Bit-flags describing the details of a `MappableBuffer`
enum class MappableBufferFlags : VkFlags {
    /// Indicates that a mapped pointer for the buffer memory must be created upon construction,
    /// and that this memory will be persistently mapped, requiring no map or unmap operations.
    /// When not set, mapping must be done explicitly via `.map()`
    eCreateMapped = (1u << 0u), /// Imposes -> .flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT,

    /// @brief Requests that the mapped pointers for the buffer support random access read/write. 
    eRandomAccess = (1u << 1u), /// Imposes -> .flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,

    /// @brief Indicates that the mapped pointers will only be written sequentially with memcpy() like
    ///        functions or linear loops. Abiding by this restriction may make mapped access more efficient.
    eSequentialWrite = (1u << 2u), /// Imposes -> .flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT

    /// @brief Enforces a requirement that the mapped memory type be host-coherent. 
    eRequireCoherence = (1u << 3u) /// Imposes -> .requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
};

inline constexpr MappableBufferFlags operator&(MappableBufferFlags lhs, MappableBufferFlags rhs) noexcept { return MappableBufferFlags( VkFlags(lhs) & VkFlags(rhs)); }
inline constexpr MappableBufferFlags operator|(MappableBufferFlags lhs, MappableBufferFlags rhs) noexcept { return MappableBufferFlags( VkFlags(lhs) | VkFlags(rhs)); }
inline constexpr MappableBufferFlags operator^(MappableBufferFlags lhs, MappableBufferFlags rhs) noexcept { return MappableBufferFlags( VkFlags(lhs) ^ VkFlags(rhs)); }
inline constexpr MappableBufferFlags& operator&=(MappableBufferFlags& lhs, MappableBufferFlags rhs) noexcept { return lhs = (lhs & rhs); }
inline constexpr MappableBufferFlags& operator|=(MappableBufferFlags& lhs, MappableBufferFlags rhs) noexcept { return lhs = (lhs | rhs); }
inline constexpr MappableBufferFlags& operator^=(MappableBufferFlags& lhs, MappableBufferFlags rhs) noexcept { return lhs = (lhs ^ rhs); }

/// Wraps a host memory mapped Vulkan buffer, and provides convenient and safe access to the mapped buffer memory.
/// Moveable, not copyable. 
class MappableBuffer : public Buffer
{
public:
    /// @brief Construct invalid `MappableBuffer` object with no associated buffer or memory allocation. 
    MappableBuffer() = default;

    /// Create, allocate, and bind memory to a new buffer of the given size and usage flags. The 
    /// buffer is host-accessible through a mapped pointer created along with the buffer. Random
    /// access through the mapped pointer is supported 
    /// NOTE: This constructor is the easiest to call, and creates a `MappableBuffer` which is 
    ///       as permissive as possible, but which may not be the most efficient for all use 
    ///       cases. Consider using a sequential access mapped buffer when appropriate.  
    MappableBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage);
    /// Create, allocate, and bind memory to a new buffer of the given size and usage flags. The buffer
    /// is host-accessible, in accordance with the flags in @a aFlags. 
    /// NOTE: The created buffer will not initially have a host-mapped pointer unless `eCreateMapped` is provided in @a aFlags.
    MappableBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage, MappableBufferFlags aFlags);
    /// Create a new buffer using the provided create info, then allocate and bind memory to the buffer.
    /// The buffer's memory is host-accessible, in accordance with the flags in @a aFlags. 
    /// NOTE: The created buffer will not initially have a host-mapped pointer unless `eCreateMapped` is provided in @a aFlags.
    MappableBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, MappableBufferFlags aFlags);
    /// Constructor for maximum specificity. Create, allocate, and bind memory to a new buffer by passing the provided buffer
    /// creation info and VMA allocation create info directly to @a aAllocator. Should only be necessary for highly specific
    /// and unusual use cases.
    /// @throws `vk::LogicError` if the requested buffer and allocation is NOT host-accessible via mapped pointers!
    MappableBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo);

    virtual ~MappableBuffer() override { reset(); }
    MappableBuffer(MappableBuffer&& aOther);
    MappableBuffer& operator=(MappableBuffer&& aOther);

    /// @brief Free any buffer resources, and return object to invalid state. 
    void reset();

    /// @brief MappableBufferFlags with which the buffer was created. Will return 0 if buffer is invalid. 
    MappableBufferFlags flags() const { return mMappedFlags; }

    /// Returns true if this `MappableBuffer` is currently mapped, meaning that a portion of the host's address space is 
    /// currently mapped to the Vulkan buffer's memory, and that a host-accessible pointer to that memory is available
    /// for immediate use.
    bool isMapped() const { return mMappedPtr != nullptr; }

    /// Returns true if this buffer is persistently mapped, meaning it was created mapped and uses a single 
    /// host-accessible pointer which will remain valid for the lifetime of this buffer. 
    /// When true, `unmap()` has no effect and `map()` simply returns the existing pointer. 
    bool isPersistentlyMapped() const { return bool(mMappedFlags & MappableBufferFlags::eCreateMapped); }

    /// Returns a host-accessible pointer to the mapped buffer memory when the buffer is valid and mapped, and `nullptr` otherwise. 
    /// 
    /// You can call `isMapped()` to verify the returned pointer will not be null. If the buffer is not mapped, you can make
    /// it mapped by calling `map()`. 
    void* getMappedPtr() const { return mMappedPtr; }

    /// Maps the buffer into the host address space, and returns a host-accessible pointer to the mapped memory. If the buffer
    /// was already mapped, the existing pointer is simply returned. Returns `nullptr` if the buffer is invalid. 
    void* map();

    /// @brief Same as `map()`, but reinterpret casts the pointer into the templated type.
    ///        Returns `nullptr` if the buffer is invalid. 
    /// NOTE: Always returns at least a single-pointer. Example: `<float>` and `<float*>` both return `float*`. 
    template<typename T, typename PtrT = std::add_pointer_t<std::remove_pointer_t<T>>>
    PtrT mapAs() { return reinterpret_cast<PtrT>(map()); }

    /// @brief Same as `getMappedPtr()`, but reinterpret casts the pointer into the templated type.
    /// NOTE: Always returns at least a single-pointer. Example: `<float>` and `<float*>` both return `float*`. 
    template<typename T, typename PtrT = std::add_pointer_t<std::remove_pointer_t<T>>>
    PtrT getMappedPtrAs() const { return reinterpret_cast<PtrT>(getMappedPtr()); }

    /// Unmap the buffer from the host-address space. Any host-accessible pointers retrieved from this `MappableBuffer`
    /// become invalidated. If the buffer memory is not host-coherent, flushAndInvalidate() is called before unmapping.
    /// Does nothing if the buffer is invalid. 
    void unmap();

    /// @brief Returns true if this buffer's memory is host-coherent, meaning that explicit flushes and invalidations 
    ///        of the memory range are not necessary to ensure visibility of writes from either the host or device.
    bool isCoherent() const { return mIsCoherent; }

    /// Flushes host caches to ensure visibility of host writes on the device. Only necessary for buffer memory which 
    /// is NOT host coherent, and will safely do nothing if `isCoherent()` returns true. See `vkFlushMappedMemoryRanges()`
    /// in the Vulkan specification for detailed information. Does nothing if the buffer is invalid. 
    void flush();

    /// Invalidates host caches to ensure host visibility of device writes. Only necessary for buffer memory which is
    /// NOT host coherent, and will safely do nothing if `isCoherent()` returns true. See `vkInvalidateMappedMemoryRanges()`
    /// in the Vulkan specification for detailed information. Does nothing if the buffer is invalid. 
    void invalidatePages();

    /// Combination of `flush()` and `invalidate()` to ensure bidirectional visibility of writes. Race conditions do occur 
    /// if both the device and host wrote to the mapped buffer memory. Does nothing if the buffer is invalid. 
    void flushAndInvalidatePages();

    /// @brief std::ostream support for dumping contents of a mapped buffer. Writes `bufferSize()` many bytes 
    ///        to the ostream, and throws if buffer is unmapped. 
    friend std::ostream& operator<<(std::ostream&, const MappableBuffer&);

protected:
    friend class Buffer;
    friend class UploadStagedBuffer;

    /// Return object to invalid state, without freeing any resources. Used to safely nullify
    /// the RHS of a move operation.
    void releaseReset();

    // Protect copy calls so that they can only be used responsibly during a move. 
    MappableBuffer(const MappableBuffer&) = default;
    MappableBuffer& operator=(const MappableBuffer&) = default;

    MappableBufferFlags mMappedFlags = {};
    bool mIsCoherent = false;
    void* mMappedPtr = nullptr;
};

/// Wrapper providing a device local buffer which is updated via copy operations from a host-accessible staging buffer. 
/// Each class instance handles the creation of both buffers, and provides an interface for uploading data through the 
/// staging buffer. 
class UploadStagedBuffer : public Buffer
{
public:
    /// @brief Construct invalid `UploadStagedBuffer` object with no associated buffers or memory allocations. 
    UploadStagedBuffer() = default;

    /// Create, allocate, and bind memory to new device local and staging buffers of the given size, and
    /// with the device buffer supporting the supplied usage flags.
    UploadStagedBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage);
    /// Create, allocate, and bind memory to new device local and staging buffers of the given size, using the provided
    /// buffer create info to create the device local buffer. 
    /// @throws `vk::LogicError` if @a aCreateInfo does not include the `eTransferDst` flag.
    UploadStagedBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo);
    /// Constructor for maximum specificity. Create, allocate, and bind memory to a new buffer by passing the provided buffer
    /// creation info and VMA allocation create info directly to @a aAllocator. Should only be necessary for highly specific
    /// and unusual use cases.
    /// @throws `vk::LogicError` if @a aCreateInfo does not include the `eTransferDst` flag.
    UploadStagedBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo);

    virtual ~UploadStagedBuffer() { reset(); }
    UploadStagedBuffer(UploadStagedBuffer&& aOther) : UploadStagedBuffer(aOther) { aOther.releaseReset(); }
    UploadStagedBuffer& operator=(UploadStagedBuffer&& aOther);

    /// @brief Returns handle of staging buffer.
    /// WARNING: Direct access to the staging buffer is typically unecessary, and could lead to hard to debug issues.
    vk::Buffer stagingBuffer() const { return mStagingBuffer.buffer(); };

    /// @brief Copies `bufferSize()` many bytes from `aData` into the staging buffer.
    /// @throws InvalidResourceError If called while buffer is invalid. 
    void stageData(const void* aData);

    /// @brief Copies `n` many bytes from `aData` into the staging buffer.
    /// @throws InvalidResourceError If called while buffer is invalid. 
    /// @throws OutOfRangeResourceError If `n > bufferSize()`.
    void stageData(const void* aData, const vk::DeviceSize n);

    /// Records, into @a aCmdBuffer, commands to copy from the staging buffer into the device local buffer.
    /// @throws InvalidResourceError If called while buffer is invalid. 
    void recUpload(vk::CommandBuffer aCmdBuffer);

    /// Records a pipeline barrier into @a aCmdBuffer, such that any previously recorded upload to the device local buffer 
    /// will be visible to later commands in the synchronization scope specified by @a aDstStageMask and @a aDstAccessMask.
    /// @throws InvalidResourceError If called while buffer is invalid. 
    void recUploadBarrier(vk::CommandBuffer aCmdBuffer, vk::PipelineStageFlags2 aDstStageMask, vk::AccessFlags2 aDstAccessMask);

    /// @brief Immediately transfer staged buffer data to the device using the provided queue, blocking until transfer completes.
    /// @throws InvalidResourceError If called while buffer is invalid. 
    void uploadNow(QueueClosure aTransferClosure);

    /// @brief Immediately stage and then upload `this->bufferSize()` many bytes from aData, blocking until the 
    ///        copy and transfer complete. 
    /// @throws InvalidResourceError If called while buffer is invalid. 
    void uploadNow(const void* aData, QueueClosure aTransferClosure);

    /// @brief Immediately stage and then upload `n` many bytes from aData, blocking until the 
    ///        copy and transfer complete. 
    /// @throws InvalidResourceError If called while buffer is invalid.
    /// @throws OutOfRangeResourceError If `n > bufferSize()`.
    void uploadNow(const void* aData, QueueClosure aTransferClosure, const vk::DeviceSize n);

    /// Transforms this staged buffer into an unstaged buffer. The staging buffer is freed, and the device local
    /// buffer is returned. The calling staged buffer object is invalidated, and must not be used afterwards. 
    Buffer dropStage();

protected:
    friend class Buffer;

    /// Return object to invalid state, without freeing any resources. Used to safely nullify
    /// the RHS of a move operation.
    void releaseReset();

    /// Special private constructor which creates the staging buffer for both upload and download. Used by 
    /// as construction delegate by `UpDownStagedBuffer`.
    UploadStagedBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo, _DerivedInitTag);

    // Protect copy calls so that they can only be used responsibly during a move. 
    UploadStagedBuffer(const UploadStagedBuffer&) = default;
    UploadStagedBuffer& operator=(const UploadStagedBuffer&) = default;

    MappableBuffer mStagingBuffer;
};

} // namespace vult
