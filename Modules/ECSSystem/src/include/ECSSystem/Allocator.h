#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <vector>
#include <utility>

namespace FluxECS {

    template <typename T> 
    class PoolAllocator {
    public:
        explicit PoolAllocator(std::size_t blockCapacity = 64) 
            : m_blockCapacity(blockCapacity) { 
            allocate_new_block(); 
        }

        ~PoolAllocator() {
            for (void* b : m_blocks) {
                ::operator delete(b);
            }
        }

        T* allocate() {
            if (m_freeIndices.empty()) {
                allocate_new_block();
            }
            std::size_t idx = m_freeIndices.back();
            m_freeIndices.pop_back();

            auto [block_ptr, offset] = locate(idx);
            void* place = static_cast<char*>(block_ptr) + offset * sizeof(T);
            return new (place) T(); // Конструируем объект
        }

        void deallocate(T* ptr) {
            if (!ptr) return;
            ptr->~T(); // Вызываем деструктор
            std::size_t idx = index_of(ptr);
            m_freeIndices.push_back(idx);
        }

    private:
        void allocate_new_block() {
            void* block = ::operator new(sizeof(T) * m_blockCapacity);
            m_blocks.push_back(block);

            std::size_t startIdx = (m_blocks.size() - 1) * m_blockCapacity;
            for (std::size_t i = 0; i < m_blockCapacity; ++i) {
                m_freeIndices.push_back(startIdx + i);
            }
        }

        std::pair<void*, std::size_t> locate(std::size_t idx) const {
            std::size_t block_idx = idx / m_blockCapacity;
            std::size_t offset = idx % m_blockCapacity;
            assert(block_idx < m_blocks.size());
            return {m_blocks[block_idx], offset};
        }

        std::size_t index_of(T* ptr) const {
            for (std::size_t b = 0; b < m_blocks.size(); ++b) {
                char* block = static_cast<char*>(m_blocks[b]);
                std::ptrdiff_t diff = reinterpret_cast<char*>(ptr) - block;
                if (diff >= 0 && static_cast<std::size_t>(diff) < sizeof(T) * m_blockCapacity) {
                    std::size_t offset = static_cast<std::size_t>(diff) / sizeof(T);
                    return b * m_blockCapacity + offset;
                }
            }
            assert(false && "Pointer is not from this pool!");
            return static_cast<std::size_t>(-1);
        }

        std::vector<void*> m_blocks;
        std::vector<std::size_t> m_freeIndices;
        std::size_t m_blockCapacity;
    };

} // namespace FluxECS