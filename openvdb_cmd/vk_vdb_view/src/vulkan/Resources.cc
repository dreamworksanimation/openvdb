// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "Resources.h"

namespace vult {

namespace {

// Utilities Local to this File
///////////////////////////////////////////////////////////////////////////////////////////////////

// Utility to allow small inline modifications to the const-static template allocation info structures
// used in this cc file. 
struct VmaAllocationCreateInfoBuilder : public VmaAllocationCreateInfo
{
    VmaAllocationCreateInfoBuilder& setFlags(VmaAllocationCreateFlags aFlags) { flags = aFlags; return *this; }
    VmaAllocationCreateInfoBuilder& setRequiredFlags(VkMemoryPropertyFlags aRequiredFlags) { requiredFlags = aRequiredFlags; return *this; }

    VmaAllocationCreateInfoBuilder setFlags(VmaAllocationCreateFlags aFlags) const {
        VmaAllocationCreateInfoBuilder builder = *this;
        builder.flags = aFlags; return builder;
    }
    VmaAllocationCreateInfoBuilder setRequiredFlags(VkMemoryPropertyFlags aRequiredFlags) const {
        VmaAllocationCreateInfoBuilder builder = *this;
        builder.requiredFlags = aRequiredFlags; return builder;
    }
};

} // end anonymous namespace

// Buffer
///////////////////////////////////////////////////////////////////////////////////////////////////

const static VmaAllocationCreateInfoBuilder csBufferStandardAllocInfo = {
    /*.flags=*/ 0u,
    /*.usage=*/ VMA_MEMORY_USAGE_AUTO,
    {}, {}, {}, {}, {},
    /*.priority=*/ 0.5f
};

Buffer::Buffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage)
: Buffer(aAllocator, vk::BufferCreateInfo({}, aSize, aUsage, vk::SharingMode::eExclusive), csBufferStandardAllocInfo)
{}

Buffer::Buffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo)
: Buffer(aAllocator, aCreateInfo, csBufferStandardAllocInfo)
{}

Buffer::Buffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo)
: mAllocator(aAllocator), mSize(aCreateInfo.size), mUsage(aCreateInfo.usage)
{
    if (mAllocator == VK_NULL_HANDLE) throw vk::LogicError("Buffer: Constructor passed null VMA.");

    VkResult r = vmaCreateBuffer(mAllocator, vkpp_cptr(aCreateInfo), &aAllocCreateInfo, vkpp_cptr(mBuffer), &mAllocation, nullptr);
    vk::resultCheck(vk::Result(r), "Buffer: vmaCreateBuffer() failed.");
}

Buffer::Buffer(Buffer&& aOther) : Buffer(aOther) {
    aOther.releaseReset();
}

Buffer& Buffer::operator=(Buffer&& aOther) {
    reset();
    *this = aOther;
    aOther.releaseReset();
    return *this;
}

Buffer::Buffer(UploadStagedBuffer&& aStaged) : Buffer(aStaged) {
    aStaged.mStagingBuffer.reset();
    aStaged.releaseReset();
}

Buffer& Buffer::operator=(UploadStagedBuffer&& aStaged) {
    reset();
    *this = aStaged;
    aStaged.mStagingBuffer.reset();
    aStaged.releaseReset();
    return *this;
}

Buffer::Buffer(MappableBuffer&& aMapped) : Buffer(aMapped) {
    aMapped.unmap();
    aMapped.releaseReset();
}

Buffer& Buffer::operator=(MappableBuffer&& aMapped) {
    reset();
    *this = aMapped;
    aMapped.unmap();
    aMapped.releaseReset();
    return *this;
}

void Buffer::reset() {
    if (mBuffer) {
        vmaDestroyBuffer(mAllocator, mBuffer, mAllocation);
    }
    // Placement-new to default-reconstruct `this` in place. Does not allocate. 
    new(this) Buffer;
}

VmaAllocationInfo Buffer::getAllocInfo() const {
    throwIfInvalid("Buffer::getAllocInfo called on invalid vult::Buffer object.");
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(mAllocator, mAllocation, &allocInfo);
    return allocInfo;
};

std::pair<vk::Buffer, VmaAllocation> Buffer::release() {
    std::pair<vk::Buffer, VmaAllocation> handles(mBuffer, mAllocation);
    releaseReset();
    return handles;
}

Buffer::Buffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage, _DerivedInitTag)
: mAllocator(aAllocator), mSize(aSize), mUsage(aUsage)
{}

void Buffer::releaseReset() {
    // Placement-new to default-reconstruct `this` in place. Does not allocate. 
    new(this) Buffer;
}

// MappableBuffer
///////////////////////////////////////////////////////////////////////////////////////////////////

const static VmaAllocationCreateInfoBuilder csMappableBufferStandardAllocInfo = {
    /*.flags=*/ VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    /*.usage=*/ VMA_MEMORY_USAGE_AUTO,
    {}, /*.preferredFlags=*/ VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    {}, {}, {},
    /*.priority=*/ 0.5f
};

constexpr VmaAllocationCreateFlags mappableBufferFlagsToAllocCreateFlags(MappableBufferFlags aMapped) {
    VmaAllocationCreateFlags aAlloc = 0u;
    aAlloc |= bool(aMapped & MappableBufferFlags::eCreateMapped) ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0u;
    aAlloc |= bool(aMapped & MappableBufferFlags::eRandomAccess) ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0u;
    aAlloc |= bool(aMapped & MappableBufferFlags::eSequentialWrite) ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0u;
    return aAlloc;
}

constexpr MappableBufferFlags allocCreateFlagsToMappableBufferFlags(VmaAllocationCreateFlags aAlloc) {
    MappableBufferFlags aMapped = MappableBufferFlags(0u);
    aMapped |= bool(aAlloc & VMA_ALLOCATION_CREATE_MAPPED_BIT) ? MappableBufferFlags::eCreateMapped : MappableBufferFlags(0u);
    aMapped |= bool(aAlloc & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) ? MappableBufferFlags::eRandomAccess : MappableBufferFlags(0u);
    aMapped |= bool(aAlloc & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) ? MappableBufferFlags::eSequentialWrite : MappableBufferFlags(0u);
    return aMapped;
}

constexpr VkMemoryPropertyFlags MappableBufferFlagsToPropertyFlags(MappableBufferFlags aMapped) {
    return bool(aMapped & MappableBufferFlags::eRequireCoherence) ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0u;
}


MappableBuffer::MappableBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage)
: MappableBuffer(
    aAllocator,
    vk::BufferCreateInfo({}, aSize, aUsage),
    csMappableBufferStandardAllocInfo)
{}

MappableBuffer::MappableBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage, MappableBufferFlags aFlags)
: MappableBuffer(
    aAllocator,
    vk::BufferCreateInfo({}, aSize, aUsage),
    csMappableBufferStandardAllocInfo
        .setFlags(mappableBufferFlagsToAllocCreateFlags(aFlags))
        .setRequiredFlags(MappableBufferFlagsToPropertyFlags(aFlags)))
{}

MappableBuffer::MappableBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, MappableBufferFlags aFlags)
: MappableBuffer(
    aAllocator,
    aCreateInfo,
    csMappableBufferStandardAllocInfo
        .setFlags(mappableBufferFlagsToAllocCreateFlags(aFlags))
        .setRequiredFlags(MappableBufferFlagsToPropertyFlags(aFlags)))
{}

MappableBuffer::MappableBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo)
: Buffer(aAllocator, aCreateInfo.size, aCreateInfo.usage, _DerivedInitTag()), mMappedFlags(allocCreateFlagsToMappableBufferFlags(aAllocCreateInfo.flags))
{
    if (mAllocator == VK_NULL_HANDLE) throw vk::LogicError("MappableBuffer: Constructor passed null VMA");

    constexpr VmaAllocationCreateFlags hostAccessMask = (VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    const bool usesHostAccessFlag = (aAllocCreateInfo.flags & hostAccessMask) != 0;
    const bool requiresHostVisMemory = (aAllocCreateInfo.requiredFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (!usesHostAccessFlag && !requiresHostVisMemory) {
        throw vk::LogicError("MappableBuffer: VmaAllocationCreateInfo struct provided for construction does not enforce host visiblity. "
                             "Explicit allocation creation info passed to `MappableBuffer` MUST either flag VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT "
                             "and/or VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT through `.flags`, or flag VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT "
                             "in `.requiredFlags`");
    }

    VmaAllocationInfo allocInfo;
    VkResult r = vmaCreateBuffer(mAllocator, vkpp_cptr(aCreateInfo), &aAllocCreateInfo, vkpp_cptr(mBuffer), &mAllocation, &allocInfo);
    vk::resultCheck(vk::Result(r), "MappableBuffer: vmaCreateBuffer() failed"); 

    VkMemoryPropertyFlags memoryFlags;
    vmaGetAllocationMemoryProperties(mAllocator, mAllocation, &memoryFlags);
    mIsCoherent = bool(memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (bool(aAllocCreateInfo.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        mMappedPtr = allocInfo.pMappedData;
    }

}

MappableBuffer::MappableBuffer(MappableBuffer&& aOther) : MappableBuffer(aOther) {
    aOther.releaseReset();
}

MappableBuffer& MappableBuffer::operator=(MappableBuffer&& aOther) {
    this->reset();
    *this = aOther;
    aOther.releaseReset();
    return *this;
}

void MappableBuffer::reset() {
    unmap();
    Buffer::reset();

    // This is nice for completeness, but not necessary for safety.
    // Hence it is ok that reset() is not a virtual function.
    mMappedFlags = {};
    mMappedPtr = nullptr;
}

void* MappableBuffer::map() {
    if (mMappedPtr == nullptr) {
        vk::Result r = vk::Result(vmaMapMemory(mAllocator, mAllocation, &mMappedPtr));

        // Failure should be nearly impossible given the validity checks of `MappableBuffer`, but it could 
        // still happen as a result of severe errors elsewhere which affect the VMA, Vulkan device, 
        // or arise due to race-conditions. 
        vk::resultCheck(r, "MappableBuffer: vmaMapMemory() failed");
    }

    return mMappedPtr;
}

void MappableBuffer::unmap() {
    if (mMappedPtr == nullptr || isPersistentlyMapped()) return;
    
    if (!isCoherent()) flushAndInvalidatePages();

    vmaUnmapMemory(mAllocator, mAllocation);
    mMappedPtr = nullptr;
}

void MappableBuffer::flush() {
    if (!isValid()) return;
    vmaFlushAllocation(mAllocator, mAllocation, 0ul, mSize);
}

void MappableBuffer::invalidatePages() {
    if (!isValid()) return;
    vmaInvalidateAllocation(mAllocator, mAllocation, 0ul, mSize);
}

void MappableBuffer::flushAndInvalidatePages() {
    if (!isValid()) return;
    vmaFlushAllocation(mAllocator, mAllocation, 0ul, mSize);
    vmaInvalidateAllocation(mAllocator, mAllocation, 0ul, mSize);
}

std::ostream& operator<<(std::ostream& ostream, const MappableBuffer& aBuffer) {
    #ifdef NDEBUG
        std::cerr << "Streaming vult::MappableBuffer in NDEBUG build! "
                  << "The stream output operator of vult::MappableBuffer is intended for debugging purposes only!"
        << std::endl;
    #endif
    if (!aBuffer.isMapped()) throw std::runtime_error("Attempted to stream contents of buffer while it is not mapped.");

    ostream.write(aBuffer.getMappedPtrAs<const char>(), aBuffer.bufferSize());

    return ostream;
}

void MappableBuffer::releaseReset() {
    Buffer::releaseReset();
    mMappedFlags = {};
    mMappedPtr = nullptr;
}


// UploadStagedBuffer
///////////////////////////////////////////////////////////////////////////////////////////////////

const static VmaAllocationCreateInfoBuilder csUploadStagedBufferDeviceLocalAllocInfo = {
    /*.flags=*/ 0u,
    /*.usage=*/ VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    {}, {}, {}, {}, {},
    /*.priority=*/ 0.5f
};

const static MappableBufferFlags csUploadStagingBufferStandardFlags = MappableBufferFlags::eCreateMapped
                                                                    | MappableBufferFlags::eSequentialWrite;


UploadStagedBuffer::UploadStagedBuffer(VmaAllocator aAllocator, vk::DeviceSize aSize, vk::BufferUsageFlags aUsage)
: UploadStagedBuffer(
    aAllocator,
    vk::BufferCreateInfo({}, aSize, aUsage | vk::BufferUsageFlagBits::eTransferDst),
    csUploadStagedBufferDeviceLocalAllocInfo)
{}

UploadStagedBuffer::UploadStagedBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo)
: UploadStagedBuffer(aAllocator, aCreateInfo, csUploadStagedBufferDeviceLocalAllocInfo)
{}

UploadStagedBuffer::UploadStagedBuffer(VmaAllocator aAllocator, const vk::BufferCreateInfo& aCreateInfo, const VmaAllocationCreateInfo& aAllocCreateInfo)
: mStagingBuffer(aAllocator, aCreateInfo.size, vk::BufferUsageFlagBits::eTransferSrc, csUploadStagingBufferStandardFlags),
  Buffer(aAllocator, aCreateInfo, aAllocCreateInfo)
{
    if (!bool(aCreateInfo.usage & vk::BufferUsageFlagBits::eTransferDst)) {
        throw vk::LogicError("UploadStagedBuffer: vk::BufferCreateInfo struct provided for construction does not flag `eTransferDst`, which is necessary "
                             "for copy operations from the staging buffer to the device local buffer.");
    }
}

UploadStagedBuffer::UploadStagedBuffer(
    VmaAllocator aAllocator,
    const vk::BufferCreateInfo& aCreateInfo,
    const VmaAllocationCreateInfo& aAllocCreateInfo,
    _DerivedInitTag)
: mStagingBuffer(aAllocator, aCreateInfo.size, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc, csUploadStagingBufferStandardFlags),
  Buffer(aAllocator, aCreateInfo, aAllocCreateInfo)
{}

UploadStagedBuffer& UploadStagedBuffer::operator=(UploadStagedBuffer&& aOther) {
    reset();
    *this = aOther;
    aOther.releaseReset();
    return *this;
}

void UploadStagedBuffer::stageData(const void* aData) {
    throwIfInvalid("stageData() called on invalid staged buffer.");
    const char* pData = reinterpret_cast<const char*>(aData);
    std::copy_n(pData, bufferSize(), mStagingBuffer.getMappedPtrAs<char>());
    mStagingBuffer.flush();
}

void UploadStagedBuffer::stageData(const void* aData, const vk::DeviceSize n) {
    throwIfInvalid("stageData() called on invalid staged buffer.");
    if (n > bufferSize()) throw OutOfRangeResourceError("stageData(): n exceeds buffer size.");
    const char* pData = reinterpret_cast<const char*>(aData);
    std::copy_n(pData, n, mStagingBuffer.getMappedPtrAs<char>());
    mStagingBuffer.flush();
}

void UploadStagedBuffer::recUpload(vk::CommandBuffer aCmdBuffer) {
    throwIfInvalid("recUpload() called on invalid staged buffer.");
    aCmdBuffer.copyBuffer(mStagingBuffer, buffer(), vk::BufferCopy(0ul, 0ul, bufferSize()));
}

void UploadStagedBuffer::recUploadBarrier(vk::CommandBuffer aCmdBuffer, vk::PipelineStageFlags2 aDstStageMask, vk::AccessFlags2 aDstAccessMask) {
    throwIfInvalid("recUploadBarrier() called on invalid staged buffer.");
    const vk::BufferMemoryBarrier2 bufferBarrier(
        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
        aDstStageMask, aDstAccessMask, {}, {}, mBuffer, 0u, vk::WholeSize);
    aCmdBuffer.pipelineBarrier2(vk::DependencyInfoKHR({}, {}, bufferBarrier));
}

void UploadStagedBuffer::uploadNow(QueueClosure aTransferClosure) {
    throwIfInvalid("uploadNow() called on invalid staged buffer.");
    vk::CommandBuffer cmdBuffer = aTransferClosure.beginSingleSubmitCommands();
    recUpload(cmdBuffer);
    aTransferClosure.endSingleSubmitCommandsAndFlush(cmdBuffer);
}

void UploadStagedBuffer::uploadNow(const void* aData, QueueClosure aTransferClosure) {
    throwIfInvalid("uploadNow() called on invalid staged buffer.");
    stageData(aData);
    uploadNow(aTransferClosure);
}

void UploadStagedBuffer::uploadNow(const void* aData, QueueClosure aTransferClosure, const vk::DeviceSize n) {
    throwIfInvalid("uploadNow() called on invalid staged buffer.");
    stageData(aData, n);
    uploadNow(aTransferClosure);
}

Buffer UploadStagedBuffer::dropStage() {
    return std::move(*this);
}

void UploadStagedBuffer::releaseReset() {
    Buffer::releaseReset();
    mStagingBuffer.releaseReset();
}

} // namespace vult
