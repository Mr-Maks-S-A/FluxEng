#include <MemorySystem/Core.hpp>
#include <MemorySystem/VirtualMemory.hpp>

#include <algorithm>
#include <utility>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <sys/mman.h>
#    include <unistd.h>
#endif

namespace MemorySystem {

namespace {

struct SystemInfo {
    std::size_t page = 4096;
    std::size_t granularity = 4096;
};

SystemInfo query_system_info() noexcept {
    SystemInfo info;
#if defined(_WIN32)
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    info.page = system.dwPageSize;
    info.granularity = system.dwAllocationGranularity;
#else
    const long page = sysconf(_SC_PAGESIZE);
    info.page = page > 0 ? static_cast<std::size_t>(page) : 4096u;
    info.granularity = info.page;
#endif
    return info;
}

const SystemInfo& system_info() noexcept {
    static const SystemInfo info = query_system_info();
    return info;
}

} // namespace

std::size_t page_size() noexcept { return system_info().page; }

std::size_t reservation_granularity() noexcept { return system_info().granularity; }

VirtualRegion VirtualRegion::reserve(std::size_t bytes) noexcept {
    VirtualRegion region;
    if (bytes == 0) {
        return region;
    }
    const std::size_t size = align_up(bytes, reservation_granularity());
#if defined(_WIN32)
    void* base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (base == nullptr) {
        return region;
    }
#else
    void* base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        return region;
    }
#endif
    region.m_base = static_cast<std::byte*>(base);
    region.m_reserved = size;
    return region;
}

VirtualRegion::VirtualRegion(VirtualRegion&& other) noexcept
    : m_base(std::exchange(other.m_base, nullptr)), m_reserved(std::exchange(other.m_reserved, 0)) {}

VirtualRegion& VirtualRegion::operator=(VirtualRegion&& other) noexcept {
    if (this != &other) {
        release();
        m_base = std::exchange(other.m_base, nullptr);
        m_reserved = std::exchange(other.m_reserved, 0);
    }
    return *this;
}

VirtualRegion::~VirtualRegion() { release(); }

bool VirtualRegion::commit(std::size_t offset, std::size_t bytes) noexcept {
    if (m_base == nullptr || bytes == 0 || offset > m_reserved || bytes > m_reserved - offset) {
        return bytes == 0 && m_base != nullptr;
    }
    // Страницы, которые задевает диапазон.
    const std::size_t page = page_size();
    const std::size_t begin = offset / page * page;
    const std::size_t end = std::min(align_up(offset + bytes, page), m_reserved);
#if defined(_WIN32)
    return VirtualAlloc(m_base + begin, end - begin, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(m_base + begin, end - begin, PROT_READ | PROT_WRITE) == 0;
#endif
}

void VirtualRegion::decommit(std::size_t offset, std::size_t bytes) noexcept {
    if (m_base == nullptr || offset >= m_reserved) {
        return;
    }
    // Только страницы, которые диапазон покрывает целиком.
    const std::size_t page = page_size();
    const std::size_t begin = align_up(offset, page);
    const std::size_t end = std::min((offset + bytes) / page * page, m_reserved);
    if (end <= begin) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(m_base + begin, end - begin, MEM_DECOMMIT);
#else
    // MADV_DONTNEED на приватном анонимном отображении: следующее чтение вернёт нули.
    madvise(m_base + begin, end - begin, MADV_DONTNEED);
    mprotect(m_base + begin, end - begin, PROT_NONE);
#endif
}

void VirtualRegion::release() noexcept {
    if (m_base == nullptr) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(m_base, 0, MEM_RELEASE);
#else
    munmap(m_base, m_reserved);
#endif
    m_base = nullptr;
    m_reserved = 0;
}

} // namespace MemorySystem
