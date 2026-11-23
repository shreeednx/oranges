// Guest memory as Vulkan buffers: each NVN memory pool's storage (guest memory, from nvnMemoryPoolInitialize) is
// imported with VK_EXT_external_memory_host, and a buffer over it serves every buffer use inside the pool (vertex,
// index, uniform, storage, copies). The GPU reads the game's memory as it is and its writes land there, so the CPU
// sees them without copies once a submit has finished.
//
// Virtual pools (above 4 GB, a page table over physical pools) are not imported: a buffer there is not found.
#include <map>

#include "vk.h"

namespace ovk {
namespace {

struct Pool {
    uint64_t gpu = 0, size = 0;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
};
std::map<uint64_t, Pool> pools;   // by GPU address

void drop(Pool& p) {
    vkDeviceWaitIdle(g.dev);
    vkDestroyBuffer(g.dev, p.buf, nullptr);
    vkFreeMemory(g.dev, p.mem, nullptr);
}

}  // namespace

void pool_created(uint64_t gpu, uint64_t size) {
    if (!g.dev || !size || gpu >= (1ull << 32)) return;
    std::lock_guard lk(big_lock);
    // (a pool where older ones were: they are gone, their memory reused)
    for (auto it = pools.begin(); it != pools.end();) {
        if (it->second.gpu < gpu + size && gpu < it->second.gpu + it->second.size) {
            drop(it->second);
            it = pools.erase(it);
        } else {
            ++it;
        }
    }
    uint8_t* host = nvn_gpu_host(gpu);
    const uint64_t a = g.host_align;
    if (!host || (reinterpret_cast<uintptr_t>(host) & (a - 1)) || (size & (a - 1))) {
        vk_log_limited("pool", 16, "vk: memory pool %#llx + %#llx is not aligned to %#llx: not imported", (unsigned long long)gpu,
                       (unsigned long long)size, (unsigned long long)a);
        return;
    }
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (g.x.vkGetMemoryHostPointerPropertiesEXT(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp) != VK_SUCCESS ||
        !hp.memoryTypeBits) {
        vk_log_limited("pool", 16, "vk: memory pool %#llx: the driver takes no host memory there", (unsigned long long)gpu);
        return;
    }
    Pool p;
    p.gpu = gpu, p.size = size;
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = memory_type(hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (const VkResult r = vkAllocateMemory(g.dev, &ai, nullptr, &p.mem); r != VK_SUCCESS) {
        vk_log_limited("pool", 16, "vk: memory pool %#llx + %#llx: import failed (%d)", (unsigned long long)gpu,
                       (unsigned long long)size, (int)r);
        return;
    }
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &p.buf) != VK_SUCCESS) {
        vkFreeMemory(g.dev, p.mem, nullptr);
        return;
    }
    vkBindBufferMemory(g.dev, p.buf, p.mem, 0);
    pools[gpu] = p;
    static int logged;
    if (logged++ < 400) hos_log("vk: memory pool %#llx + %#llx imported", (unsigned long long)gpu, (unsigned long long)size);
}

BufRef buffer_at(uint64_t gpu, uint64_t size) {
    auto it = pools.upper_bound(gpu);
    if (it == pools.begin()) return {};
    --it;
    const Pool& p = it->second;
    if (gpu < p.gpu || gpu + size > p.gpu + p.size) return {};
    return {p.buf, gpu - p.gpu, p.gpu + p.size - gpu};
}

}  // namespace ovk
