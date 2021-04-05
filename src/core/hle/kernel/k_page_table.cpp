// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/assert.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/kernel/k_address_space_info.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_memory_block_manager.h"
#include "core/hle/kernel/k_page_linked_list.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_system_control.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"

namespace Kernel {

namespace {

constexpr std::size_t GetAddressSpaceWidthFromType(FileSys::ProgramAddressSpaceType as_type) {
    switch (as_type) {
    case FileSys::ProgramAddressSpaceType::Is32Bit:
    case FileSys::ProgramAddressSpaceType::Is32BitNoMap:
        return 32;
    case FileSys::ProgramAddressSpaceType::Is36Bit:
        return 36;
    case FileSys::ProgramAddressSpaceType::Is39Bit:
        return 39;
    default:
        UNREACHABLE();
        return {};
    }
}

constexpr u64 GetAddressInRange(const KMemoryInfo& info, VAddr addr) {
    if (info.GetAddress() < addr) {
        return addr;
    }
    return info.GetAddress();
}

constexpr std::size_t GetSizeInRange(const KMemoryInfo& info, VAddr start, VAddr end) {
    std::size_t size{info.GetSize()};
    if (info.GetAddress() < start) {
        size -= start - info.GetAddress();
    }
    if (info.GetEndAddress() > end) {
        size -= info.GetEndAddress() - end;
    }
    return size;
}

} // namespace

KPageTable::KPageTable(Core::System& system) : system{system} {}

ResultCode KPageTable::InitializeForProcess(FileSys::ProgramAddressSpaceType as_type,
                                            bool enable_aslr, VAddr code_addr,
                                            std::size_t code_size, KMemoryManager::Pool pool) {

    const auto GetSpaceStart = [this](KAddressSpaceInfo::Type type) {
        return KAddressSpaceInfo::GetAddressSpaceStart(address_space_width, type);
    };
    const auto GetSpaceSize = [this](KAddressSpaceInfo::Type type) {
        return KAddressSpaceInfo::GetAddressSpaceSize(address_space_width, type);
    };

    //  Set our width and heap/alias sizes
    address_space_width = GetAddressSpaceWidthFromType(as_type);
    const VAddr start = 0;
    const VAddr end{1ULL << address_space_width};
    std::size_t alias_region_size{GetSpaceSize(KAddressSpaceInfo::Type::Alias)};
    std::size_t heap_region_size{GetSpaceSize(KAddressSpaceInfo::Type::Heap)};

    ASSERT(start <= code_addr);
    ASSERT(code_addr < code_addr + code_size);
    ASSERT(code_addr + code_size - 1 <= end - 1);

    // Adjust heap/alias size if we don't have an alias region
    if (as_type == FileSys::ProgramAddressSpaceType::Is32BitNoMap) {
        heap_region_size += alias_region_size;
        alias_region_size = 0;
    }

    // Set code regions and determine remaining
    constexpr std::size_t RegionAlignment{2 * 1024 * 1024};
    VAddr process_code_start{};
    VAddr process_code_end{};
    std::size_t stack_region_size{};
    std::size_t kernel_map_region_size{};

    if (address_space_width == 39) {
        alias_region_size = GetSpaceSize(KAddressSpaceInfo::Type::Alias);
        heap_region_size = GetSpaceSize(KAddressSpaceInfo::Type::Heap);
        stack_region_size = GetSpaceSize(KAddressSpaceInfo::Type::Stack);
        kernel_map_region_size = GetSpaceSize(KAddressSpaceInfo::Type::MapSmall);
        code_region_start = GetSpaceStart(KAddressSpaceInfo::Type::Map39Bit);
        code_region_end = code_region_start + GetSpaceSize(KAddressSpaceInfo::Type::Map39Bit);
        alias_code_region_start = code_region_start;
        alias_code_region_end = code_region_end;
        process_code_start = Common::AlignDown(code_addr, RegionAlignment);
        process_code_end = Common::AlignUp(code_addr + code_size, RegionAlignment);
    } else {
        stack_region_size = 0;
        kernel_map_region_size = 0;
        code_region_start = GetSpaceStart(KAddressSpaceInfo::Type::MapSmall);
        code_region_end = code_region_start + GetSpaceSize(KAddressSpaceInfo::Type::MapSmall);
        stack_region_start = code_region_start;
        alias_code_region_start = code_region_start;
        alias_code_region_end = GetSpaceStart(KAddressSpaceInfo::Type::MapLarge) +
                                GetSpaceSize(KAddressSpaceInfo::Type::MapLarge);
        stack_region_end = code_region_end;
        kernel_map_region_start = code_region_start;
        kernel_map_region_end = code_region_end;
        process_code_start = code_region_start;
        process_code_end = code_region_end;
    }

    // Set other basic fields
    is_aslr_enabled = enable_aslr;
    address_space_start = start;
    address_space_end = end;
    is_kernel = false;

    // Determine the region we can place our undetermineds in
    VAddr alloc_start{};
    std::size_t alloc_size{};
    if ((process_code_start - code_region_start) >= (end - process_code_end)) {
        alloc_start = code_region_start;
        alloc_size = process_code_start - code_region_start;
    } else {
        alloc_start = process_code_end;
        alloc_size = end - process_code_end;
    }
    const std::size_t needed_size{
        (alias_region_size + heap_region_size + stack_region_size + kernel_map_region_size)};
    if (alloc_size < needed_size) {
        UNREACHABLE();
        return ResultOutOfMemory;
    }

    const std::size_t remaining_size{alloc_size - needed_size};

    // Determine random placements for each region
    std::size_t alias_rnd{}, heap_rnd{}, stack_rnd{}, kmap_rnd{};
    if (enable_aslr) {
        alias_rnd = KSystemControl::GenerateRandomRange(0, remaining_size / RegionAlignment) *
                    RegionAlignment;
        heap_rnd = KSystemControl::GenerateRandomRange(0, remaining_size / RegionAlignment) *
                   RegionAlignment;
        stack_rnd = KSystemControl::GenerateRandomRange(0, remaining_size / RegionAlignment) *
                    RegionAlignment;
        kmap_rnd = KSystemControl::GenerateRandomRange(0, remaining_size / RegionAlignment) *
                   RegionAlignment;
    }

    // Setup heap and alias regions
    alias_region_start = alloc_start + alias_rnd;
    alias_region_end = alias_region_start + alias_region_size;
    heap_region_start = alloc_start + heap_rnd;
    heap_region_end = heap_region_start + heap_region_size;

    if (alias_rnd <= heap_rnd) {
        heap_region_start += alias_region_size;
        heap_region_end += alias_region_size;
    } else {
        alias_region_start += heap_region_size;
        alias_region_end += heap_region_size;
    }

    // Setup stack region
    if (stack_region_size) {
        stack_region_start = alloc_start + stack_rnd;
        stack_region_end = stack_region_start + stack_region_size;

        if (alias_rnd < stack_rnd) {
            stack_region_start += alias_region_size;
            stack_region_end += alias_region_size;
        } else {
            alias_region_start += stack_region_size;
            alias_region_end += stack_region_size;
        }

        if (heap_rnd < stack_rnd) {
            stack_region_start += heap_region_size;
            stack_region_end += heap_region_size;
        } else {
            heap_region_start += stack_region_size;
            heap_region_end += stack_region_size;
        }
    }

    // Setup kernel map region
    if (kernel_map_region_size) {
        kernel_map_region_start = alloc_start + kmap_rnd;
        kernel_map_region_end = kernel_map_region_start + kernel_map_region_size;

        if (alias_rnd < kmap_rnd) {
            kernel_map_region_start += alias_region_size;
            kernel_map_region_end += alias_region_size;
        } else {
            alias_region_start += kernel_map_region_size;
            alias_region_end += kernel_map_region_size;
        }

        if (heap_rnd < kmap_rnd) {
            kernel_map_region_start += heap_region_size;
            kernel_map_region_end += heap_region_size;
        } else {
            heap_region_start += kernel_map_region_size;
            heap_region_end += kernel_map_region_size;
        }

        if (stack_region_size) {
            if (stack_rnd < kmap_rnd) {
                kernel_map_region_start += stack_region_size;
                kernel_map_region_end += stack_region_size;
            } else {
                stack_region_start += kernel_map_region_size;
                stack_region_end += kernel_map_region_size;
            }
        }
    }

    // Set heap members
    current_heap_end = heap_region_start;
    max_heap_size = 0;
    max_physical_memory_size = 0;

    // Ensure that we regions inside our address space
    auto IsInAddressSpace = [&](VAddr addr) {
        return address_space_start <= addr && addr <= address_space_end;
    };
    ASSERT(IsInAddressSpace(alias_region_start));
    ASSERT(IsInAddressSpace(alias_region_end));
    ASSERT(IsInAddressSpace(heap_region_start));
    ASSERT(IsInAddressSpace(heap_region_end));
    ASSERT(IsInAddressSpace(stack_region_start));
    ASSERT(IsInAddressSpace(stack_region_end));
    ASSERT(IsInAddressSpace(kernel_map_region_start));
    ASSERT(IsInAddressSpace(kernel_map_region_end));

    // Ensure that we selected regions that don't overlap
    const VAddr alias_start{alias_region_start};
    const VAddr alias_last{alias_region_end - 1};
    const VAddr heap_start{heap_region_start};
    const VAddr heap_last{heap_region_end - 1};
    const VAddr stack_start{stack_region_start};
    const VAddr stack_last{stack_region_end - 1};
    const VAddr kmap_start{kernel_map_region_start};
    const VAddr kmap_last{kernel_map_region_end - 1};
    ASSERT(alias_last < heap_start || heap_last < alias_start);
    ASSERT(alias_last < stack_start || stack_last < alias_start);
    ASSERT(alias_last < kmap_start || kmap_last < alias_start);
    ASSERT(heap_last < stack_start || stack_last < heap_start);
    ASSERT(heap_last < kmap_start || kmap_last < heap_start);

    current_heap_addr = heap_region_start;
    heap_capacity = 0;
    physical_memory_usage = 0;
    memory_pool = pool;

    page_table_impl.Resize(address_space_width, PageBits);

    return InitializeMemoryLayout(start, end);
}

ResultCode KPageTable::MapProcessCode(VAddr addr, std::size_t num_pages, KMemoryState state,
                                      KMemoryPermission perm) {
    std::lock_guard lock{page_table_lock};

    const u64 size{num_pages * PageSize};

    if (!CanContain(addr, size, state)) {
        return ResultInvalidCurrentMemory;
    }

    if (IsRegionMapped(addr, size)) {
        return ResultInvalidCurrentMemory;
    }

    KPageLinkedList page_linked_list;
    CASCADE_CODE(
        system.Kernel().MemoryManager().Allocate(page_linked_list, num_pages, memory_pool));
    CASCADE_CODE(Operate(addr, num_pages, page_linked_list, OperationType::MapGroup));

    block_manager->Update(addr, num_pages, state, perm);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::MapProcessCodeMemory(VAddr dst_addr, VAddr src_addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    const std::size_t num_pages{size / PageSize};

    KMemoryState state{};
    KMemoryPermission perm{};
    CASCADE_CODE(CheckMemoryState(&state, &perm, nullptr, src_addr, size, KMemoryState::All,
                                  KMemoryState::Normal, KMemoryPermission::Mask,
                                  KMemoryPermission::ReadAndWrite, KMemoryAttribute::Mask,
                                  KMemoryAttribute::None, KMemoryAttribute::IpcAndDeviceMapped));

    if (IsRegionMapped(dst_addr, size)) {
        return ResultInvalidCurrentMemory;
    }

    KPageLinkedList page_linked_list;
    AddRegionToPages(src_addr, num_pages, page_linked_list);

    {
        auto block_guard = detail::ScopeExit(
            [&] { Operate(src_addr, num_pages, perm, OperationType::ChangePermissions); });

        CASCADE_CODE(Operate(src_addr, num_pages, KMemoryPermission::None,
                             OperationType::ChangePermissions));
        CASCADE_CODE(MapPages(dst_addr, page_linked_list, KMemoryPermission::None));

        block_guard.Cancel();
    }

    block_manager->Update(src_addr, num_pages, state, KMemoryPermission::None,
                          KMemoryAttribute::Locked);
    block_manager->Update(dst_addr, num_pages, KMemoryState::AliasCode);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::UnmapProcessCodeMemory(VAddr dst_addr, VAddr src_addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    if (!size) {
        return RESULT_SUCCESS;
    }

    const std::size_t num_pages{size / PageSize};

    CASCADE_CODE(CheckMemoryState(nullptr, nullptr, nullptr, src_addr, size, KMemoryState::All,
                                  KMemoryState::Normal, KMemoryPermission::None,
                                  KMemoryPermission::None, KMemoryAttribute::Mask,
                                  KMemoryAttribute::Locked, KMemoryAttribute::IpcAndDeviceMapped));

    KMemoryState state{};
    CASCADE_CODE(CheckMemoryState(
        &state, nullptr, nullptr, dst_addr, PageSize, KMemoryState::FlagCanCodeAlias,
        KMemoryState::FlagCanCodeAlias, KMemoryPermission::None, KMemoryPermission::None,
        KMemoryAttribute::Mask, KMemoryAttribute::None, KMemoryAttribute::IpcAndDeviceMapped));
    CASCADE_CODE(CheckMemoryState(dst_addr, size, KMemoryState::All, state, KMemoryPermission::None,
                                  KMemoryPermission::None, KMemoryAttribute::Mask,
                                  KMemoryAttribute::None));
    CASCADE_CODE(Operate(dst_addr, num_pages, KMemoryPermission::None, OperationType::Unmap));

    block_manager->Update(dst_addr, num_pages, KMemoryState::Free);
    block_manager->Update(src_addr, num_pages, KMemoryState::Normal,
                          KMemoryPermission::ReadAndWrite);

    return RESULT_SUCCESS;
}

void KPageTable::MapPhysicalMemory(KPageLinkedList& page_linked_list, VAddr start, VAddr end) {
    auto node{page_linked_list.Nodes().begin()};
    PAddr map_addr{node->GetAddress()};
    std::size_t src_num_pages{node->GetNumPages()};

    block_manager->IterateForRange(start, end, [&](const KMemoryInfo& info) {
        if (info.state != KMemoryState::Free) {
            return;
        }

        std::size_t dst_num_pages{GetSizeInRange(info, start, end) / PageSize};
        VAddr dst_addr{GetAddressInRange(info, start)};

        while (dst_num_pages) {
            if (!src_num_pages) {
                node = std::next(node);
                map_addr = node->GetAddress();
                src_num_pages = node->GetNumPages();
            }

            const std::size_t num_pages{std::min(src_num_pages, dst_num_pages)};
            Operate(dst_addr, num_pages, KMemoryPermission::ReadAndWrite, OperationType::Map,
                    map_addr);

            dst_addr += num_pages * PageSize;
            map_addr += num_pages * PageSize;
            src_num_pages -= num_pages;
            dst_num_pages -= num_pages;
        }
    });
}

ResultCode KPageTable::MapPhysicalMemory(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    std::size_t mapped_size{};
    const VAddr end_addr{addr + size};

    block_manager->IterateForRange(addr, end_addr, [&](const KMemoryInfo& info) {
        if (info.state != KMemoryState::Free) {
            mapped_size += GetSizeInRange(info, addr, end_addr);
        }
    });

    if (mapped_size == size) {
        return RESULT_SUCCESS;
    }

    const std::size_t remaining_size{size - mapped_size};
    const std::size_t remaining_pages{remaining_size / PageSize};

    // Reserve the memory from the process resource limit.
    KScopedResourceReservation memory_reservation(
        system.Kernel().CurrentProcess()->GetResourceLimit(), LimitableResource::PhysicalMemory,
        remaining_size);
    if (!memory_reservation.Succeeded()) {
        LOG_ERROR(Kernel, "Could not reserve remaining {:X} bytes", remaining_size);
        return ResultResourceLimitedExceeded;
    }

    KPageLinkedList page_linked_list;

    CASCADE_CODE(
        system.Kernel().MemoryManager().Allocate(page_linked_list, remaining_pages, memory_pool));

    // We succeeded, so commit the memory reservation.
    memory_reservation.Commit();

    MapPhysicalMemory(page_linked_list, addr, end_addr);

    physical_memory_usage += remaining_size;

    const std::size_t num_pages{size / PageSize};
    block_manager->Update(addr, num_pages, KMemoryState::Free, KMemoryPermission::None,
                          KMemoryAttribute::None, KMemoryState::Normal,
                          KMemoryPermission::ReadAndWrite, KMemoryAttribute::None);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::UnmapPhysicalMemory(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    const VAddr end_addr{addr + size};
    ResultCode result{RESULT_SUCCESS};
    std::size_t mapped_size{};

    // Verify that the region can be unmapped
    block_manager->IterateForRange(addr, end_addr, [&](const KMemoryInfo& info) {
        if (info.state == KMemoryState::Normal) {
            if (info.attribute != KMemoryAttribute::None) {
                result = ResultInvalidCurrentMemory;
                return;
            }
            mapped_size += GetSizeInRange(info, addr, end_addr);
        } else if (info.state != KMemoryState::Free) {
            result = ResultInvalidCurrentMemory;
        }
    });

    if (result.IsError()) {
        return result;
    }

    if (!mapped_size) {
        return RESULT_SUCCESS;
    }

    CASCADE_CODE(UnmapMemory(addr, size));

    auto process{system.Kernel().CurrentProcess()};
    process->GetResourceLimit()->Release(LimitableResource::PhysicalMemory, mapped_size);
    physical_memory_usage -= mapped_size;

    return RESULT_SUCCESS;
}

ResultCode KPageTable::UnmapMemory(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    const VAddr end_addr{addr + size};
    ResultCode result{RESULT_SUCCESS};
    KPageLinkedList page_linked_list;

    // Unmap each region within the range
    block_manager->IterateForRange(addr, end_addr, [&](const KMemoryInfo& info) {
        if (info.state == KMemoryState::Normal) {
            const std::size_t block_size{GetSizeInRange(info, addr, end_addr)};
            const std::size_t block_num_pages{block_size / PageSize};
            const VAddr block_addr{GetAddressInRange(info, addr)};

            AddRegionToPages(block_addr, block_size / PageSize, page_linked_list);

            if (result = Operate(block_addr, block_num_pages, KMemoryPermission::None,
                                 OperationType::Unmap);
                result.IsError()) {
                return;
            }
        }
    });

    if (result.IsError()) {
        return result;
    }

    const std::size_t num_pages{size / PageSize};
    system.Kernel().MemoryManager().Free(page_linked_list, num_pages, memory_pool);

    block_manager->Update(addr, num_pages, KMemoryState::Free);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::Map(VAddr dst_addr, VAddr src_addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    KMemoryState src_state{};
    CASCADE_CODE(CheckMemoryState(
        &src_state, nullptr, nullptr, src_addr, size, KMemoryState::FlagCanAlias,
        KMemoryState::FlagCanAlias, KMemoryPermission::Mask, KMemoryPermission::ReadAndWrite,
        KMemoryAttribute::Mask, KMemoryAttribute::None, KMemoryAttribute::IpcAndDeviceMapped));

    if (IsRegionMapped(dst_addr, size)) {
        return ResultInvalidCurrentMemory;
    }

    KPageLinkedList page_linked_list;
    const std::size_t num_pages{size / PageSize};

    AddRegionToPages(src_addr, num_pages, page_linked_list);

    {
        auto block_guard = detail::ScopeExit([&] {
            Operate(src_addr, num_pages, KMemoryPermission::ReadAndWrite,
                    OperationType::ChangePermissions);
        });

        CASCADE_CODE(Operate(src_addr, num_pages, KMemoryPermission::None,
                             OperationType::ChangePermissions));
        CASCADE_CODE(MapPages(dst_addr, page_linked_list, KMemoryPermission::ReadAndWrite));

        block_guard.Cancel();
    }

    block_manager->Update(src_addr, num_pages, src_state, KMemoryPermission::None,
                          KMemoryAttribute::Locked);
    block_manager->Update(dst_addr, num_pages, KMemoryState::Stack,
                          KMemoryPermission::ReadAndWrite);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::Unmap(VAddr dst_addr, VAddr src_addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    KMemoryState src_state{};
    CASCADE_CODE(CheckMemoryState(
        &src_state, nullptr, nullptr, src_addr, size, KMemoryState::FlagCanAlias,
        KMemoryState::FlagCanAlias, KMemoryPermission::Mask, KMemoryPermission::None,
        KMemoryAttribute::Mask, KMemoryAttribute::Locked, KMemoryAttribute::IpcAndDeviceMapped));

    KMemoryPermission dst_perm{};
    CASCADE_CODE(CheckMemoryState(nullptr, &dst_perm, nullptr, dst_addr, size, KMemoryState::All,
                                  KMemoryState::Stack, KMemoryPermission::None,
                                  KMemoryPermission::None, KMemoryAttribute::Mask,
                                  KMemoryAttribute::None, KMemoryAttribute::IpcAndDeviceMapped));

    KPageLinkedList src_pages;
    KPageLinkedList dst_pages;
    const std::size_t num_pages{size / PageSize};

    AddRegionToPages(src_addr, num_pages, src_pages);
    AddRegionToPages(dst_addr, num_pages, dst_pages);

    if (!dst_pages.IsEqual(src_pages)) {
        return ResultInvalidMemoryRange;
    }

    {
        auto block_guard = detail::ScopeExit([&] { MapPages(dst_addr, dst_pages, dst_perm); });

        CASCADE_CODE(Operate(dst_addr, num_pages, KMemoryPermission::None, OperationType::Unmap));
        CASCADE_CODE(Operate(src_addr, num_pages, KMemoryPermission::ReadAndWrite,
                             OperationType::ChangePermissions));

        block_guard.Cancel();
    }

    block_manager->Update(src_addr, num_pages, src_state, KMemoryPermission::ReadAndWrite);
    block_manager->Update(dst_addr, num_pages, KMemoryState::Free);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::MapPages(VAddr addr, const KPageLinkedList& page_linked_list,
                                KMemoryPermission perm) {
    VAddr cur_addr{addr};

    for (const auto& node : page_linked_list.Nodes()) {
        if (const auto result{
                Operate(cur_addr, node.GetNumPages(), perm, OperationType::Map, node.GetAddress())};
            result.IsError()) {
            const std::size_t num_pages{(addr - cur_addr) / PageSize};

            ASSERT(Operate(addr, num_pages, KMemoryPermission::None, OperationType::Unmap)
                       .IsSuccess());

            return result;
        }

        cur_addr += node.GetNumPages() * PageSize;
    }

    return RESULT_SUCCESS;
}

ResultCode KPageTable::MapPages(VAddr addr, KPageLinkedList& page_linked_list, KMemoryState state,
                                KMemoryPermission perm) {
    std::lock_guard lock{page_table_lock};

    const std::size_t num_pages{page_linked_list.GetNumPages()};
    const std::size_t size{num_pages * PageSize};

    if (!CanContain(addr, size, state)) {
        return ResultInvalidCurrentMemory;
    }

    if (IsRegionMapped(addr, num_pages * PageSize)) {
        return ResultInvalidCurrentMemory;
    }

    CASCADE_CODE(MapPages(addr, page_linked_list, perm));

    block_manager->Update(addr, num_pages, state, perm);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::SetCodeMemoryPermission(VAddr addr, std::size_t size,
                                               KMemoryPermission perm) {

    std::lock_guard lock{page_table_lock};

    KMemoryState prev_state{};
    KMemoryPermission prev_perm{};

    CASCADE_CODE(CheckMemoryState(
        &prev_state, &prev_perm, nullptr, addr, size, KMemoryState::FlagCode,
        KMemoryState::FlagCode, KMemoryPermission::None, KMemoryPermission::None,
        KMemoryAttribute::Mask, KMemoryAttribute::None, KMemoryAttribute::IpcAndDeviceMapped));

    KMemoryState state{prev_state};

    // Ensure state is mutable if permission allows write
    if ((perm & KMemoryPermission::Write) != KMemoryPermission::None) {
        if (prev_state == KMemoryState::Code) {
            state = KMemoryState::CodeData;
        } else if (prev_state == KMemoryState::AliasCode) {
            state = KMemoryState::AliasCodeData;
        } else {
            UNREACHABLE();
        }
    }

    // Return early if there is nothing to change
    if (state == prev_state && perm == prev_perm) {
        return RESULT_SUCCESS;
    }

    if ((prev_perm & KMemoryPermission::Execute) != (perm & KMemoryPermission::Execute)) {
        // Memory execution state is changing, invalidate CPU cache range
        system.InvalidateCpuInstructionCacheRange(addr, size);
    }

    const std::size_t num_pages{size / PageSize};
    const OperationType operation{(perm & KMemoryPermission::Execute) != KMemoryPermission::None
                                      ? OperationType::ChangePermissionsAndRefresh
                                      : OperationType::ChangePermissions};

    CASCADE_CODE(Operate(addr, num_pages, perm, operation));

    block_manager->Update(addr, num_pages, state, perm);

    return RESULT_SUCCESS;
}

KMemoryInfo KPageTable::QueryInfoImpl(VAddr addr) {
    std::lock_guard lock{page_table_lock};

    return block_manager->FindBlock(addr).GetMemoryInfo();
}

KMemoryInfo KPageTable::QueryInfo(VAddr addr) {
    if (!Contains(addr, 1)) {
        return {address_space_end,       0 - address_space_end,  KMemoryState::Inaccessible,
                KMemoryPermission::None, KMemoryAttribute::None, KMemoryPermission::None};
    }

    return QueryInfoImpl(addr);
}

ResultCode KPageTable::ReserveTransferMemory(VAddr addr, std::size_t size, KMemoryPermission perm) {
    std::lock_guard lock{page_table_lock};

    KMemoryState state{};
    KMemoryAttribute attribute{};

    CASCADE_CODE(CheckMemoryState(
        &state, nullptr, &attribute, addr, size,
        KMemoryState::FlagCanTransfer | KMemoryState::FlagReferenceCounted,
        KMemoryState::FlagCanTransfer | KMemoryState::FlagReferenceCounted, KMemoryPermission::Mask,
        KMemoryPermission::ReadAndWrite, KMemoryAttribute::Mask, KMemoryAttribute::None,
        KMemoryAttribute::IpcAndDeviceMapped));

    block_manager->Update(addr, size / PageSize, state, perm, attribute | KMemoryAttribute::Locked);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::ResetTransferMemory(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    KMemoryState state{};

    CASCADE_CODE(
        CheckMemoryState(&state, nullptr, nullptr, addr, size,
                         KMemoryState::FlagCanTransfer | KMemoryState::FlagReferenceCounted,
                         KMemoryState::FlagCanTransfer | KMemoryState::FlagReferenceCounted,
                         KMemoryPermission::None, KMemoryPermission::None, KMemoryAttribute::Mask,
                         KMemoryAttribute::Locked, KMemoryAttribute::IpcAndDeviceMapped));

    block_manager->Update(addr, size / PageSize, state, KMemoryPermission::ReadAndWrite);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::SetMemoryAttribute(VAddr addr, std::size_t size, KMemoryAttribute mask,
                                          KMemoryAttribute value) {
    std::lock_guard lock{page_table_lock};

    KMemoryState state{};
    KMemoryPermission perm{};
    KMemoryAttribute attribute{};

    CASCADE_CODE(CheckMemoryState(
        &state, &perm, &attribute, addr, size, KMemoryState::FlagCanChangeAttribute,
        KMemoryState::FlagCanChangeAttribute, KMemoryPermission::None, KMemoryPermission::None,
        KMemoryAttribute::LockedAndIpcLocked, KMemoryAttribute::None,
        KMemoryAttribute::DeviceSharedAndUncached));

    attribute = attribute & ~mask;
    attribute = attribute | (mask & value);

    block_manager->Update(addr, size / PageSize, state, perm, attribute);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::SetHeapCapacity(std::size_t new_heap_capacity) {
    std::lock_guard lock{page_table_lock};
    heap_capacity = new_heap_capacity;
    return RESULT_SUCCESS;
}

ResultVal<VAddr> KPageTable::SetHeapSize(std::size_t size) {

    if (size > heap_region_end - heap_region_start) {
        return ResultOutOfMemory;
    }

    const u64 previous_heap_size{GetHeapSize()};

    UNIMPLEMENTED_IF_MSG(previous_heap_size > size, "Heap shrink is unimplemented");

    // Increase the heap size
    {
        std::lock_guard lock{page_table_lock};

        const u64 delta{size - previous_heap_size};

        // Reserve memory for the heap extension.
        KScopedResourceReservation memory_reservation(
            system.Kernel().CurrentProcess()->GetResourceLimit(), LimitableResource::PhysicalMemory,
            delta);

        if (!memory_reservation.Succeeded()) {
            LOG_ERROR(Kernel, "Could not reserve heap extension of size {:X} bytes", delta);
            return ResultResourceLimitedExceeded;
        }

        KPageLinkedList page_linked_list;
        const std::size_t num_pages{delta / PageSize};

        CASCADE_CODE(
            system.Kernel().MemoryManager().Allocate(page_linked_list, num_pages, memory_pool));

        if (IsRegionMapped(current_heap_addr, delta)) {
            return ResultInvalidCurrentMemory;
        }

        CASCADE_CODE(
            Operate(current_heap_addr, num_pages, page_linked_list, OperationType::MapGroup));

        // Succeeded in allocation, commit the resource reservation
        memory_reservation.Commit();

        block_manager->Update(current_heap_addr, num_pages, KMemoryState::Normal,
                              KMemoryPermission::ReadAndWrite);

        current_heap_addr = heap_region_start + size;
    }

    return MakeResult<VAddr>(heap_region_start);
}

ResultVal<VAddr> KPageTable::AllocateAndMapMemory(std::size_t needed_num_pages, std::size_t align,
                                                  bool is_map_only, VAddr region_start,
                                                  std::size_t region_num_pages, KMemoryState state,
                                                  KMemoryPermission perm, PAddr map_addr) {
    std::lock_guard lock{page_table_lock};

    if (!CanContain(region_start, region_num_pages * PageSize, state)) {
        return ResultInvalidCurrentMemory;
    }

    if (region_num_pages <= needed_num_pages) {
        return ResultOutOfMemory;
    }

    const VAddr addr{
        AllocateVirtualMemory(region_start, region_num_pages, needed_num_pages, align)};
    if (!addr) {
        return ResultOutOfMemory;
    }

    if (is_map_only) {
        CASCADE_CODE(Operate(addr, needed_num_pages, perm, OperationType::Map, map_addr));
    } else {
        KPageLinkedList page_group;
        CASCADE_CODE(
            system.Kernel().MemoryManager().Allocate(page_group, needed_num_pages, memory_pool));
        CASCADE_CODE(Operate(addr, needed_num_pages, page_group, OperationType::MapGroup));
    }

    block_manager->Update(addr, needed_num_pages, state, perm);

    return MakeResult<VAddr>(addr);
}

ResultCode KPageTable::LockForDeviceAddressSpace(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    KMemoryPermission perm{};
    if (const ResultCode result{CheckMemoryState(
            nullptr, &perm, nullptr, addr, size, KMemoryState::FlagCanChangeAttribute,
            KMemoryState::FlagCanChangeAttribute, KMemoryPermission::None, KMemoryPermission::None,
            KMemoryAttribute::LockedAndIpcLocked, KMemoryAttribute::None,
            KMemoryAttribute::DeviceSharedAndUncached)};
        result.IsError()) {
        return result;
    }

    block_manager->UpdateLock(
        addr, size / PageSize,
        [](KMemoryBlockManager::iterator block, KMemoryPermission perm) {
            block->ShareToDevice(perm);
        },
        perm);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::UnlockForDeviceAddressSpace(VAddr addr, std::size_t size) {
    std::lock_guard lock{page_table_lock};

    KMemoryPermission perm{};
    if (const ResultCode result{CheckMemoryState(
            nullptr, &perm, nullptr, addr, size, KMemoryState::FlagCanChangeAttribute,
            KMemoryState::FlagCanChangeAttribute, KMemoryPermission::None, KMemoryPermission::None,
            KMemoryAttribute::LockedAndIpcLocked, KMemoryAttribute::None,
            KMemoryAttribute::DeviceSharedAndUncached)};
        result.IsError()) {
        return result;
    }

    block_manager->UpdateLock(
        addr, size / PageSize,
        [](KMemoryBlockManager::iterator block, KMemoryPermission perm) {
            block->UnshareToDevice(perm);
        },
        perm);

    return RESULT_SUCCESS;
}

ResultCode KPageTable::InitializeMemoryLayout(VAddr start, VAddr end) {
    block_manager = std::make_unique<KMemoryBlockManager>(start, end);

    return RESULT_SUCCESS;
}

bool KPageTable::IsRegionMapped(VAddr address, u64 size) {
    return CheckMemoryState(address, size, KMemoryState::All, KMemoryState::Free,
                            KMemoryPermission::Mask, KMemoryPermission::None,
                            KMemoryAttribute::Mask, KMemoryAttribute::None,
                            KMemoryAttribute::IpcAndDeviceMapped)
        .IsError();
}

bool KPageTable::IsRegionContiguous(VAddr addr, u64 size) const {
    auto start_ptr = system.Memory().GetPointer(addr);
    for (u64 offset{}; offset < size; offset += PageSize) {
        if (start_ptr != system.Memory().GetPointer(addr + offset)) {
            return false;
        }
        start_ptr += PageSize;
    }
    return true;
}

void KPageTable::AddRegionToPages(VAddr start, std::size_t num_pages,
                                  KPageLinkedList& page_linked_list) {
    VAddr addr{start};
    while (addr < start + (num_pages * PageSize)) {
        const PAddr paddr{GetPhysicalAddr(addr)};
        if (!paddr) {
            UNREACHABLE();
        }
        page_linked_list.AddBlock(paddr, 1);
        addr += PageSize;
    }
}

VAddr KPageTable::AllocateVirtualMemory(VAddr start, std::size_t region_num_pages,
                                        u64 needed_num_pages, std::size_t align) {
    if (is_aslr_enabled) {
        UNIMPLEMENTED();
    }
    return block_manager->FindFreeArea(start, region_num_pages, needed_num_pages, align, 0,
                                       IsKernel() ? 1 : 4);
}

ResultCode KPageTable::Operate(VAddr addr, std::size_t num_pages, const KPageLinkedList& page_group,
                               OperationType operation) {
    std::lock_guard lock{page_table_lock};

    ASSERT(Common::IsAligned(addr, PageSize));
    ASSERT(num_pages > 0);
    ASSERT(num_pages == page_group.GetNumPages());

    for (const auto& node : page_group.Nodes()) {
        const std::size_t size{node.GetNumPages() * PageSize};

        switch (operation) {
        case OperationType::MapGroup:
            system.Memory().MapMemoryRegion(page_table_impl, addr, size, node.GetAddress());
            break;
        default:
            UNREACHABLE();
        }

        addr += size;
    }

    return RESULT_SUCCESS;
}

ResultCode KPageTable::Operate(VAddr addr, std::size_t num_pages, KMemoryPermission perm,
                               OperationType operation, PAddr map_addr) {
    std::lock_guard lock{page_table_lock};

    ASSERT(num_pages > 0);
    ASSERT(Common::IsAligned(addr, PageSize));
    ASSERT(ContainsPages(addr, num_pages));

    switch (operation) {
    case OperationType::Unmap:
        system.Memory().UnmapRegion(page_table_impl, addr, num_pages * PageSize);
        break;
    case OperationType::Map: {
        ASSERT(map_addr);
        ASSERT(Common::IsAligned(map_addr, PageSize));
        system.Memory().MapMemoryRegion(page_table_impl, addr, num_pages * PageSize, map_addr);
        break;
    }
    case OperationType::ChangePermissions:
    case OperationType::ChangePermissionsAndRefresh:
        break;
    default:
        UNREACHABLE();
    }
    return RESULT_SUCCESS;
}

constexpr VAddr KPageTable::GetRegionAddress(KMemoryState state) const {
    switch (state) {
    case KMemoryState::Free:
    case KMemoryState::Kernel:
        return address_space_start;
    case KMemoryState::Normal:
        return heap_region_start;
    case KMemoryState::Ipc:
    case KMemoryState::NonSecureIpc:
    case KMemoryState::NonDeviceIpc:
        return alias_region_start;
    case KMemoryState::Stack:
        return stack_region_start;
    case KMemoryState::Io:
    case KMemoryState::Static:
    case KMemoryState::ThreadLocal:
        return kernel_map_region_start;
    case KMemoryState::Shared:
    case KMemoryState::AliasCode:
    case KMemoryState::AliasCodeData:
    case KMemoryState::Transferred:
    case KMemoryState::SharedTransferred:
    case KMemoryState::SharedCode:
    case KMemoryState::GeneratedCode:
    case KMemoryState::CodeOut:
        return alias_code_region_start;
    case KMemoryState::Code:
    case KMemoryState::CodeData:
        return code_region_start;
    default:
        UNREACHABLE();
        return {};
    }
}

constexpr std::size_t KPageTable::GetRegionSize(KMemoryState state) const {
    switch (state) {
    case KMemoryState::Free:
    case KMemoryState::Kernel:
        return address_space_end - address_space_start;
    case KMemoryState::Normal:
        return heap_region_end - heap_region_start;
    case KMemoryState::Ipc:
    case KMemoryState::NonSecureIpc:
    case KMemoryState::NonDeviceIpc:
        return alias_region_end - alias_region_start;
    case KMemoryState::Stack:
        return stack_region_end - stack_region_start;
    case KMemoryState::Io:
    case KMemoryState::Static:
    case KMemoryState::ThreadLocal:
        return kernel_map_region_end - kernel_map_region_start;
    case KMemoryState::Shared:
    case KMemoryState::AliasCode:
    case KMemoryState::AliasCodeData:
    case KMemoryState::Transferred:
    case KMemoryState::SharedTransferred:
    case KMemoryState::SharedCode:
    case KMemoryState::GeneratedCode:
    case KMemoryState::CodeOut:
        return alias_code_region_end - alias_code_region_start;
    case KMemoryState::Code:
    case KMemoryState::CodeData:
        return code_region_end - code_region_start;
    default:
        UNREACHABLE();
        return {};
    }
}

constexpr bool KPageTable::CanContain(VAddr addr, std::size_t size, KMemoryState state) const {
    const VAddr end{addr + size};
    const VAddr last{end - 1};
    const VAddr region_start{GetRegionAddress(state)};
    const std::size_t region_size{GetRegionSize(state)};
    const bool is_in_region{region_start <= addr && addr < end &&
                            last <= region_start + region_size - 1};
    const bool is_in_heap{!(end <= heap_region_start || heap_region_end <= addr)};
    const bool is_in_alias{!(end <= alias_region_start || alias_region_end <= addr)};

    switch (state) {
    case KMemoryState::Free:
    case KMemoryState::Kernel:
        return is_in_region;
    case KMemoryState::Io:
    case KMemoryState::Static:
    case KMemoryState::Code:
    case KMemoryState::CodeData:
    case KMemoryState::Shared:
    case KMemoryState::AliasCode:
    case KMemoryState::AliasCodeData:
    case KMemoryState::Stack:
    case KMemoryState::ThreadLocal:
    case KMemoryState::Transferred:
    case KMemoryState::SharedTransferred:
    case KMemoryState::SharedCode:
    case KMemoryState::GeneratedCode:
    case KMemoryState::CodeOut:
        return is_in_region && !is_in_heap && !is_in_alias;
    case KMemoryState::Normal:
        ASSERT(is_in_heap);
        return is_in_region && !is_in_alias;
    case KMemoryState::Ipc:
    case KMemoryState::NonSecureIpc:
    case KMemoryState::NonDeviceIpc:
        ASSERT(is_in_alias);
        return is_in_region && !is_in_heap;
    default:
        return false;
    }
}

constexpr ResultCode KPageTable::CheckMemoryState(const KMemoryInfo& info, KMemoryState state_mask,
                                                  KMemoryState state, KMemoryPermission perm_mask,
                                                  KMemoryPermission perm,
                                                  KMemoryAttribute attr_mask,
                                                  KMemoryAttribute attr) const {
    // Validate the states match expectation
    if ((info.state & state_mask) != state) {
        return ResultInvalidCurrentMemory;
    }
    if ((info.perm & perm_mask) != perm) {
        return ResultInvalidCurrentMemory;
    }
    if ((info.attribute & attr_mask) != attr) {
        return ResultInvalidCurrentMemory;
    }

    return RESULT_SUCCESS;
}

ResultCode KPageTable::CheckMemoryState(KMemoryState* out_state, KMemoryPermission* out_perm,
                                        KMemoryAttribute* out_attr, VAddr addr, std::size_t size,
                                        KMemoryState state_mask, KMemoryState state,
                                        KMemoryPermission perm_mask, KMemoryPermission perm,
                                        KMemoryAttribute attr_mask, KMemoryAttribute attr,
                                        KMemoryAttribute ignore_attr) {
    std::lock_guard lock{page_table_lock};

    // Get information about the first block
    const VAddr last_addr{addr + size - 1};
    KMemoryBlockManager::const_iterator it{block_manager->FindIterator(addr)};
    KMemoryInfo info{it->GetMemoryInfo()};

    // Validate all blocks in the range have correct state
    const KMemoryState first_state{info.state};
    const KMemoryPermission first_perm{info.perm};
    const KMemoryAttribute first_attr{info.attribute};

    while (true) {
        // Validate the current block
        if (!(info.state == first_state)) {
            return ResultInvalidCurrentMemory;
        }
        if (!(info.perm == first_perm)) {
            return ResultInvalidCurrentMemory;
        }
        if (!((info.attribute | static_cast<KMemoryAttribute>(ignore_attr)) ==
              (first_attr | static_cast<KMemoryAttribute>(ignore_attr)))) {
            return ResultInvalidCurrentMemory;
        }

        // Validate against the provided masks
        CASCADE_CODE(CheckMemoryState(info, state_mask, state, perm_mask, perm, attr_mask, attr));

        // Break once we're done
        if (last_addr <= info.GetLastAddress()) {
            break;
        }

        // Advance our iterator
        it++;
        ASSERT(it != block_manager->cend());
        info = it->GetMemoryInfo();
    }

    // Write output state
    if (out_state) {
        *out_state = first_state;
    }
    if (out_perm) {
        *out_perm = first_perm;
    }
    if (out_attr) {
        *out_attr = first_attr & static_cast<KMemoryAttribute>(~ignore_attr);
    }

    return RESULT_SUCCESS;
}

} // namespace Kernel
