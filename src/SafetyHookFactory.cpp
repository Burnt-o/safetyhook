#include <algorithm>
#include <cassert>
#include <functional>

#include <Windows.h>

#include "SafetyHookFactory.hpp"

uintptr_t SafetyHookFactory::allocate(size_t size) {
    return allocate_near(0, size, 0xFFFF'FFFF'FFFF'FFFF);
}

uintptr_t SafetyHookFactory::allocate_near(uintptr_t desired_address, size_t size, size_t max_distance) {
    std::scoped_lock _{m_mux};

    // First search through our list of allocations for a free block that is large enough.
    for (auto& allocation : m_allocations) {
        if (allocation->size < size) {
            continue;
        }

        for (auto node = allocation->free_list.get(); node != nullptr; node = node->next.get()) {
            if (node->size < size) {
                continue;
            }

            auto address = node->address;

            // Make sure this node is close enough.
            auto delta = (desired_address > address) ? desired_address - address : address - desired_address;

            if (delta > max_distance) {
                continue;
            }

            node->address += size;
            node->size -= size;

            return address;
        }
    }

    // If we didn't find a free block, we need to allocate a new one.
    auto allocation_size = ((size + 0x1000 - 1) / 0x1000) * 0x1000;
    auto nearby_address = find_memory_near(desired_address, allocation_size, max_distance);

    if (nearby_address == 0) {
        return 0;
    }

    auto allocation_address = (uintptr_t)VirtualAlloc(
        (LPVOID)nearby_address, allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (allocation_address == 0) {
        return 0;
    }

    auto& allocation = m_allocations.emplace_back(new MemoryAllocation);

    allocation->address = allocation_address;
    allocation->size = allocation_size;
    allocation->free_list = std::make_unique<FreeListNode>();
    allocation->free_list->address = allocation_address + size;
    allocation->free_list->size = allocation_size - size;

    return allocation_address;
}

void SafetyHookFactory::free(uintptr_t address, size_t size) {
    std::scoped_lock _{m_mux};

    for (auto& allocation : m_allocations) {
        if (allocation->address > address || allocation->address + allocation->size < address) {
            continue;
        }

        struct OnExit {
            std::function<void()> fn{};
            ~OnExit() { fn(); }
        } on_exit{[&allocation, this] { combine_adjacent_freenodes(*allocation); }};

        for (auto node = allocation->free_list.get(), prev = (FreeListNode*)0; node != nullptr;
             node = node->next.get()) {
            // Expand adjacent freenode (coming after).
            if (node->address == address + size) {
                node->address -= size;
                node->size += size;
                return;
            }

            // Expand adjacent freenode (coming before).
            if (node->address + node->size == address) {
                node->size += size;
                return;
            }

            // Expand containing freenode.
            if (node->address == address) {
                if (node->size < size) {
                    node->size = size;
                }

                return;
            }

            // Add new freenode.
            if (node->address > address) {
                auto free_node = std::make_unique<FreeListNode>();

                free_node->address = address;
                free_node->size = size;

                if (prev == nullptr) {
                    free_node->next.swap(allocation->free_list);
                    allocation->free_list.swap(free_node);
                } else {
                    free_node->next.swap(prev->next);
                    prev->next.swap(free_node);
                }

                return;
            }
        }

        assert(0);
    }
}

void SafetyHookFactory::combine_adjacent_freenodes(MemoryAllocation& allocation) {
    for (auto prev = allocation.free_list.get(), node = prev; node != nullptr; node = node->next.get()) {
        if (prev->address + prev->size == node->address) {
            prev->size += node->size;
            prev->next.swap(node->next);
            node->next.reset();
            node = prev;
        } else {
            prev = node;
        }
    }
}

uintptr_t SafetyHookFactory::find_memory_near(uintptr_t desired_address, size_t size, size_t max_distance) {
    SYSTEM_INFO si{};

    GetSystemInfo(&si);

    auto search_start = std::min(desired_address, desired_address - max_distance);
    auto search_end = std::max(desired_address, desired_address + max_distance);
    search_start = std::max(search_start, (uintptr_t)si.lpMinimumApplicationAddress);
    search_end = std::min(search_end, (uintptr_t)si.lpMaximumApplicationAddress);
    desired_address = std::clamp(((desired_address + 0x1000 - 1) / 0x1000) * 0x1000, search_start, search_end);

    MEMORY_BASIC_INFORMATION mbi{};

    // Search backwards from the desired_address.
    for (auto p = desired_address; p > search_start; p -= 0x1000) {
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        if (mbi.State == MEM_FREE) {
            return p;
        }
    }

    // Search forwards from the desired_address.
    for (auto p = desired_address; p < search_end; p += mbi.RegionSize) {
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        if (mbi.State == MEM_FREE) {
            return p;
        }
    }

    return 0;
}

SafetyHookFactory::MemoryAllocation::~MemoryAllocation() {
    VirtualFree((LPVOID)address, 0, MEM_RELEASE);
}
