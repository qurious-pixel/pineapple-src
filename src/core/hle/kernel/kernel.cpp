// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

#include "common/assert.h"
#include "common/common_sizes.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/thread.h"
#include "common/thread_worker.h"
#include "core/arm/arm_interface.h"
#include "core/arm/cpu_interrupt_handler.h"
#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/cpu_manager.h"
#include "core/device_memory.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/k_slab_heap.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/service_thread.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/kernel/time_manager.h"
#include "core/hle/lock.h"
#include "core/hle/result.h"
#include "core/memory.h"

MICROPROFILE_DEFINE(Kernel_SVC, "Kernel", "SVC", MP_RGB(70, 200, 70));

namespace Kernel {

struct KernelCore::Impl {
    explicit Impl(Core::System& system, KernelCore& kernel)
        : time_manager{system}, global_handle_table{kernel}, system{system} {}

    void SetMulticore(bool is_multicore) {
        this->is_multicore = is_multicore;
    }

    void Initialize(KernelCore& kernel) {
        global_scheduler_context = std::make_unique<Kernel::GlobalSchedulerContext>(kernel);

        RegisterHostThread();

        service_thread_manager =
            std::make_unique<Common::ThreadWorker>(1, "yuzu:ServiceThreadManager");
        is_phantom_mode_for_singlecore = false;

        InitializePhysicalCores();
        InitializeSystemResourceLimit(kernel, system);
        InitializeMemoryLayout();
        InitializeSchedulers();
        InitializeSuspendThreads();
        InitializePreemption(kernel);
    }

    void InitializeCores() {
        for (auto& core : cores) {
            core.Initialize(current_process->Is64BitProcess());
        }
    }

    void Shutdown() {
        process_list.clear();

        // Ensures all service threads gracefully shutdown
        service_thread_manager.reset();
        service_threads.clear();

        next_object_id = 0;
        next_kernel_process_id = Process::InitialKIPIDMin;
        next_user_process_id = Process::ProcessIDMin;
        next_thread_id = 1;

        for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
            if (suspend_threads[i]) {
                suspend_threads[i].reset();
            }
        }

        cores.clear();

        current_process = nullptr;

        global_handle_table.Clear();

        preemption_event = nullptr;

        named_ports.clear();

        exclusive_monitor.reset();

        hid_shared_mem = nullptr;
        font_shared_mem = nullptr;
        irs_shared_mem = nullptr;
        time_shared_mem = nullptr;

        system_resource_limit = nullptr;

        // Next host thead ID to use, 0-3 IDs represent core threads, >3 represent others
        next_host_thread_id = Core::Hardware::NUM_CPU_CORES;
    }

    void InitializePhysicalCores() {
        exclusive_monitor =
            Core::MakeExclusiveMonitor(system.Memory(), Core::Hardware::NUM_CPU_CORES);
        for (u32 i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
            schedulers[i] = std::make_unique<Kernel::KScheduler>(system, i);
            cores.emplace_back(i, system, *schedulers[i], interrupts);
        }
    }

    void InitializeSchedulers() {
        for (u32 i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
            cores[i].Scheduler().Initialize();
        }
    }

    // Creates the default system resource limit
    void InitializeSystemResourceLimit(KernelCore& kernel, Core::System& system) {
        system_resource_limit = std::make_shared<KResourceLimit>(kernel, system);

        // If setting the default system values fails, then something seriously wrong has occurred.
        ASSERT(system_resource_limit->SetLimitValue(LimitableResource::PhysicalMemory, 0x100000000)
                   .IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(LimitableResource::Threads, 800).IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(LimitableResource::Events, 700).IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(LimitableResource::TransferMemory, 200)
                   .IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(LimitableResource::Sessions, 933).IsSuccess());

        // Derived from recent software updates. The kernel reserves 27MB
        constexpr u64 kernel_size{0x1b00000};
        if (!system_resource_limit->Reserve(LimitableResource::PhysicalMemory, kernel_size)) {
            UNREACHABLE();
        }
        // Reserve secure applet memory, introduced in firmware 5.0.0
        constexpr u64 secure_applet_memory_size{0x400000};
        ASSERT(system_resource_limit->Reserve(LimitableResource::PhysicalMemory,
                                              secure_applet_memory_size));
    }

    void InitializePreemption(KernelCore& kernel) {
        preemption_event = Core::Timing::CreateEvent(
            "PreemptionCallback", [this, &kernel](std::uintptr_t, std::chrono::nanoseconds) {
                {
                    KScopedSchedulerLock lock(kernel);
                    global_scheduler_context->PreemptThreads();
                }
                const auto time_interval = std::chrono::nanoseconds{
                    Core::Timing::msToCycles(std::chrono::milliseconds(10))};
                system.CoreTiming().ScheduleEvent(time_interval, preemption_event);
            });

        const auto time_interval =
            std::chrono::nanoseconds{Core::Timing::msToCycles(std::chrono::milliseconds(10))};
        system.CoreTiming().ScheduleEvent(time_interval, preemption_event);
    }

    void InitializeSuspendThreads() {
        for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
            std::string name = "Suspend Thread Id:" + std::to_string(i);
            std::function<void(void*)> init_func = Core::CpuManager::GetSuspendThreadStartFunc();
            void* init_func_parameter = system.GetCpuManager().GetStartFuncParamater();
            auto thread_res = KThread::CreateThread(
                system, ThreadType::HighPriority, std::move(name), 0, 0, 0, static_cast<u32>(i), 0,
                nullptr, std::move(init_func), init_func_parameter);

            suspend_threads[i] = std::move(thread_res).Unwrap();
        }
    }

    void MakeCurrentProcess(Process* process) {
        current_process = process;
        if (process == nullptr) {
            return;
        }

        const u32 core_id = GetCurrentHostThreadID();
        if (core_id < Core::Hardware::NUM_CPU_CORES) {
            system.Memory().SetCurrentPageTable(*process, core_id);
        }
    }

    /// Creates a new host thread ID, should only be called by GetHostThreadId
    u32 AllocateHostThreadId(std::optional<std::size_t> core_id) {
        if (core_id) {
            // The first for slots are reserved for CPU core threads
            ASSERT(*core_id < Core::Hardware::NUM_CPU_CORES);
            return static_cast<u32>(*core_id);
        } else {
            return next_host_thread_id++;
        }
    }

    /// Gets the host thread ID for the caller, allocating a new one if this is the first time
    u32 GetHostThreadId(std::optional<std::size_t> core_id = std::nullopt) {
        const thread_local auto host_thread_id{AllocateHostThreadId(core_id)};
        return host_thread_id;
    }

    // Gets the dummy KThread for the caller, allocating a new one if this is the first time
    KThread* GetHostDummyThread() {
        const thread_local auto thread =
            KThread::CreateThread(
                system, ThreadType::Main, fmt::format("DummyThread:{}", GetHostThreadId()), 0,
                KThread::DefaultThreadPriority, 0, static_cast<u32>(3), 0, nullptr)
                .Unwrap();
        return thread.get();
    }

    /// Registers a CPU core thread by allocating a host thread ID for it
    void RegisterCoreThread(std::size_t core_id) {
        ASSERT(core_id < Core::Hardware::NUM_CPU_CORES);
        const auto this_id = GetHostThreadId(core_id);
        if (!is_multicore) {
            single_core_thread_id = this_id;
        }
    }

    /// Registers a new host thread by allocating a host thread ID for it
    void RegisterHostThread() {
        [[maybe_unused]] const auto this_id = GetHostThreadId();
        [[maybe_unused]] const auto dummy_thread = GetHostDummyThread();
    }

    [[nodiscard]] u32 GetCurrentHostThreadID() {
        const auto this_id = GetHostThreadId();
        if (!is_multicore && single_core_thread_id == this_id) {
            return static_cast<u32>(system.GetCpuManager().CurrentCore());
        }
        return this_id;
    }

    bool IsPhantomModeForSingleCore() const {
        return is_phantom_mode_for_singlecore;
    }

    void SetIsPhantomModeForSingleCore(bool value) {
        ASSERT(!is_multicore);
        is_phantom_mode_for_singlecore = value;
    }

    KThread* GetCurrentEmuThread() {
        const auto thread_id = GetCurrentHostThreadID();
        if (thread_id >= Core::Hardware::NUM_CPU_CORES) {
            return GetHostDummyThread();
        }
        return schedulers[thread_id]->GetCurrentThread();
    }

    void DeriveInitialMemoryLayout(KMemoryLayout& memory_layout) {
        // Insert the root region for the virtual memory tree, from which all other regions will
        // derive.
        memory_layout.GetVirtualMemoryRegionTree().InsertDirectly(
            KernelVirtualAddressSpaceBase,
            KernelVirtualAddressSpaceBase + KernelVirtualAddressSpaceSize - 1);

        // Insert the root region for the physical memory tree, from which all other regions will
        // derive.
        memory_layout.GetPhysicalMemoryRegionTree().InsertDirectly(
            KernelPhysicalAddressSpaceBase,
            KernelPhysicalAddressSpaceBase + KernelPhysicalAddressSpaceSize - 1);

        // Save start and end for ease of use.
        const VAddr code_start_virt_addr = KernelVirtualAddressCodeBase;
        const VAddr code_end_virt_addr = KernelVirtualAddressCodeEnd;

        // Setup the containing kernel region.
        constexpr size_t KernelRegionSize = Common::Size_1_GB;
        constexpr size_t KernelRegionAlign = Common::Size_1_GB;
        constexpr VAddr kernel_region_start =
            Common::AlignDown(code_start_virt_addr, KernelRegionAlign);
        size_t kernel_region_size = KernelRegionSize;
        if (!(kernel_region_start + KernelRegionSize - 1 <= KernelVirtualAddressSpaceLast)) {
            kernel_region_size = KernelVirtualAddressSpaceEnd - kernel_region_start;
        }
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            kernel_region_start, kernel_region_size, KMemoryRegionType_Kernel));

        // Setup the code region.
        constexpr size_t CodeRegionAlign = PageSize;
        constexpr VAddr code_region_start =
            Common::AlignDown(code_start_virt_addr, CodeRegionAlign);
        constexpr VAddr code_region_end = Common::AlignUp(code_end_virt_addr, CodeRegionAlign);
        constexpr size_t code_region_size = code_region_end - code_region_start;
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            code_region_start, code_region_size, KMemoryRegionType_KernelCode));

        // Setup board-specific device physical regions.
        Init::SetupDevicePhysicalMemoryRegions(memory_layout);

        // Determine the amount of space needed for the misc region.
        size_t misc_region_needed_size;
        {
            // Each core has a one page stack for all three stack types (Main, Idle, Exception).
            misc_region_needed_size = Core::Hardware::NUM_CPU_CORES * (3 * (PageSize + PageSize));

            // Account for each auto-map device.
            for (const auto& region : memory_layout.GetPhysicalMemoryRegionTree()) {
                if (region.HasTypeAttribute(KMemoryRegionAttr_ShouldKernelMap)) {
                    // Check that the region is valid.
                    ASSERT(region.GetEndAddress() != 0);

                    // Account for the region.
                    misc_region_needed_size +=
                        PageSize + (Common::AlignUp(region.GetLastAddress(), PageSize) -
                                    Common::AlignDown(region.GetAddress(), PageSize));
                }
            }

            // Multiply the needed size by three, to account for the need for guard space.
            misc_region_needed_size *= 3;
        }

        // Decide on the actual size for the misc region.
        constexpr size_t MiscRegionAlign = KernelAslrAlignment;
        constexpr size_t MiscRegionMinimumSize = Common::Size_32_MB;
        const size_t misc_region_size = Common::AlignUp(
            std::max(misc_region_needed_size, MiscRegionMinimumSize), MiscRegionAlign);
        ASSERT(misc_region_size > 0);

        // Setup the misc region.
        const VAddr misc_region_start =
            memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegion(
                misc_region_size, MiscRegionAlign, KMemoryRegionType_Kernel);
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            misc_region_start, misc_region_size, KMemoryRegionType_KernelMisc));

        // Setup the stack region.
        constexpr size_t StackRegionSize = Common::Size_14_MB;
        constexpr size_t StackRegionAlign = KernelAslrAlignment;
        const VAddr stack_region_start =
            memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegion(
                StackRegionSize, StackRegionAlign, KMemoryRegionType_Kernel);
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            stack_region_start, StackRegionSize, KMemoryRegionType_KernelStack));

        // Determine the size of the resource region.
        const size_t resource_region_size = memory_layout.GetResourceRegionSizeForInit();

        // Determine the size of the slab region.
        const size_t slab_region_size = Common::AlignUp(KernelSlabHeapSize, PageSize);
        ASSERT(slab_region_size <= resource_region_size);

        // Setup the slab region.
        const PAddr code_start_phys_addr = KernelPhysicalAddressCodeBase;
        const PAddr code_end_phys_addr = code_start_phys_addr + code_region_size;
        const PAddr slab_start_phys_addr = code_end_phys_addr;
        const PAddr slab_end_phys_addr = slab_start_phys_addr + slab_region_size;
        constexpr size_t SlabRegionAlign = KernelAslrAlignment;
        const size_t slab_region_needed_size =
            Common::AlignUp(code_end_phys_addr + slab_region_size, SlabRegionAlign) -
            Common::AlignDown(code_end_phys_addr, SlabRegionAlign);
        const VAddr slab_region_start =
            memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegion(
                slab_region_needed_size, SlabRegionAlign, KMemoryRegionType_Kernel) +
            (code_end_phys_addr % SlabRegionAlign);
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            slab_region_start, slab_region_size, KMemoryRegionType_KernelSlab));

        // Setup the temp region.
        constexpr size_t TempRegionSize = Common::Size_128_MB;
        constexpr size_t TempRegionAlign = KernelAslrAlignment;
        const VAddr temp_region_start =
            memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegion(
                TempRegionSize, TempRegionAlign, KMemoryRegionType_Kernel);
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(temp_region_start, TempRegionSize,
                                                                 KMemoryRegionType_KernelTemp));

        // Automatically map in devices that have auto-map attributes.
        for (auto& region : memory_layout.GetPhysicalMemoryRegionTree()) {
            // We only care about kernel regions.
            if (!region.IsDerivedFrom(KMemoryRegionType_Kernel)) {
                continue;
            }

            // Check whether we should map the region.
            if (!region.HasTypeAttribute(KMemoryRegionAttr_ShouldKernelMap)) {
                continue;
            }

            // If this region has already been mapped, no need to consider it.
            if (region.HasTypeAttribute(KMemoryRegionAttr_DidKernelMap)) {
                continue;
            }

            // Check that the region is valid.
            ASSERT(region.GetEndAddress() != 0);

            // Set the attribute to note we've mapped this region.
            region.SetTypeAttribute(KMemoryRegionAttr_DidKernelMap);

            // Create a virtual pair region and insert it into the tree.
            const PAddr map_phys_addr = Common::AlignDown(region.GetAddress(), PageSize);
            const size_t map_size =
                Common::AlignUp(region.GetEndAddress(), PageSize) - map_phys_addr;
            const VAddr map_virt_addr =
                memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegionWithGuard(
                    map_size, PageSize, KMemoryRegionType_KernelMisc, PageSize);
            ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
                map_virt_addr, map_size, KMemoryRegionType_KernelMiscMappedDevice));
            region.SetPairAddress(map_virt_addr + region.GetAddress() - map_phys_addr);
        }

        Init::SetupDramPhysicalMemoryRegions(memory_layout);

        // Insert a physical region for the kernel code region.
        ASSERT(memory_layout.GetPhysicalMemoryRegionTree().Insert(
            code_start_phys_addr, code_region_size, KMemoryRegionType_DramKernelCode));

        // Insert a physical region for the kernel slab region.
        ASSERT(memory_layout.GetPhysicalMemoryRegionTree().Insert(
            slab_start_phys_addr, slab_region_size, KMemoryRegionType_DramKernelSlab));

        // Determine size available for kernel page table heaps, requiring > 8 MB.
        const PAddr resource_end_phys_addr = slab_start_phys_addr + resource_region_size;
        const size_t page_table_heap_size = resource_end_phys_addr - slab_end_phys_addr;
        ASSERT(page_table_heap_size / Common::Size_4_MB > 2);

        // Insert a physical region for the kernel page table heap region
        ASSERT(memory_layout.GetPhysicalMemoryRegionTree().Insert(
            slab_end_phys_addr, page_table_heap_size, KMemoryRegionType_DramKernelPtHeap));

        // All DRAM regions that we haven't tagged by this point will be mapped under the linear
        // mapping. Tag them.
        for (auto& region : memory_layout.GetPhysicalMemoryRegionTree()) {
            if (region.GetType() == KMemoryRegionType_Dram) {
                // Check that the region is valid.
                ASSERT(region.GetEndAddress() != 0);

                // Set the linear map attribute.
                region.SetTypeAttribute(KMemoryRegionAttr_LinearMapped);
            }
        }

        // Get the linear region extents.
        const auto linear_extents =
            memory_layout.GetPhysicalMemoryRegionTree().GetDerivedRegionExtents(
                KMemoryRegionAttr_LinearMapped);
        ASSERT(linear_extents.GetEndAddress() != 0);

        // Setup the linear mapping region.
        constexpr size_t LinearRegionAlign = Common::Size_1_GB;
        const PAddr aligned_linear_phys_start =
            Common::AlignDown(linear_extents.GetAddress(), LinearRegionAlign);
        const size_t linear_region_size =
            Common::AlignUp(linear_extents.GetEndAddress(), LinearRegionAlign) -
            aligned_linear_phys_start;
        const VAddr linear_region_start =
            memory_layout.GetVirtualMemoryRegionTree().GetRandomAlignedRegionWithGuard(
                linear_region_size, LinearRegionAlign, KMemoryRegionType_None, LinearRegionAlign);

        const u64 linear_region_phys_to_virt_diff = linear_region_start - aligned_linear_phys_start;

        // Map and create regions for all the linearly-mapped data.
        {
            PAddr cur_phys_addr = 0;
            u64 cur_size = 0;
            for (auto& region : memory_layout.GetPhysicalMemoryRegionTree()) {
                if (!region.HasTypeAttribute(KMemoryRegionAttr_LinearMapped)) {
                    continue;
                }

                ASSERT(region.GetEndAddress() != 0);

                if (cur_size == 0) {
                    cur_phys_addr = region.GetAddress();
                    cur_size = region.GetSize();
                } else if (cur_phys_addr + cur_size == region.GetAddress()) {
                    cur_size += region.GetSize();
                } else {
                    cur_phys_addr = region.GetAddress();
                    cur_size = region.GetSize();
                }

                const VAddr region_virt_addr =
                    region.GetAddress() + linear_region_phys_to_virt_diff;
                ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
                    region_virt_addr, region.GetSize(),
                    GetTypeForVirtualLinearMapping(region.GetType())));
                region.SetPairAddress(region_virt_addr);

                KMemoryRegion* virt_region =
                    memory_layout.GetVirtualMemoryRegionTree().FindModifiable(region_virt_addr);
                ASSERT(virt_region != nullptr);
                virt_region->SetPairAddress(region.GetAddress());
            }
        }

        // Insert regions for the initial page table region.
        ASSERT(memory_layout.GetPhysicalMemoryRegionTree().Insert(
            resource_end_phys_addr, KernelPageTableHeapSize, KMemoryRegionType_DramKernelInitPt));
        ASSERT(memory_layout.GetVirtualMemoryRegionTree().Insert(
            resource_end_phys_addr + linear_region_phys_to_virt_diff, KernelPageTableHeapSize,
            KMemoryRegionType_VirtualDramKernelInitPt));

        // All linear-mapped DRAM regions that we haven't tagged by this point will be allocated to
        // some pool partition. Tag them.
        for (auto& region : memory_layout.GetPhysicalMemoryRegionTree()) {
            if (region.GetType() == (KMemoryRegionType_Dram | KMemoryRegionAttr_LinearMapped)) {
                region.SetType(KMemoryRegionType_DramPoolPartition);
            }
        }

        // Setup all other memory regions needed to arrange the pool partitions.
        Init::SetupPoolPartitionMemoryRegions(memory_layout);

        // Cache all linear regions in their own trees for faster access, later.
        memory_layout.InitializeLinearMemoryRegionTrees(aligned_linear_phys_start,
                                                        linear_region_start);
    }

    void InitializeMemoryLayout() {
        // Derive the initial memory layout from the emulated board
        KMemoryLayout memory_layout;
        DeriveInitialMemoryLayout(memory_layout);

        const auto system_pool = memory_layout.GetKernelSystemPoolRegionPhysicalExtents();
        const auto applet_pool = memory_layout.GetKernelAppletPoolRegionPhysicalExtents();
        const auto application_pool = memory_layout.GetKernelApplicationPoolRegionPhysicalExtents();

        // Initialize memory managers
        memory_manager = std::make_unique<KMemoryManager>();
        memory_manager->InitializeManager(KMemoryManager::Pool::Application,
                                          application_pool.GetAddress(),
                                          application_pool.GetEndAddress());
        memory_manager->InitializeManager(KMemoryManager::Pool::Applet, applet_pool.GetAddress(),
                                          applet_pool.GetEndAddress());
        memory_manager->InitializeManager(KMemoryManager::Pool::System, system_pool.GetAddress(),
                                          system_pool.GetEndAddress());

        // Setup memory regions for emulated processes
        // TODO(bunnei): These should not be hardcoded regions initialized within the kernel
        constexpr std::size_t hid_size{0x40000};
        constexpr std::size_t font_size{0x1100000};
        constexpr std::size_t irs_size{0x8000};
        constexpr std::size_t time_size{0x1000};

        const PAddr hid_phys_addr{system_pool.GetAddress()};
        const PAddr font_phys_addr{system_pool.GetAddress() + hid_size};
        const PAddr irs_phys_addr{system_pool.GetAddress() + hid_size + font_size};
        const PAddr time_phys_addr{system_pool.GetAddress() + hid_size + font_size + irs_size};

        hid_shared_mem = Kernel::KSharedMemory::Create(
            system.Kernel(), system.DeviceMemory(), nullptr, {hid_phys_addr, hid_size / PageSize},
            KMemoryPermission::None, KMemoryPermission::Read, hid_phys_addr, hid_size,
            "HID:SharedMemory");
        font_shared_mem = Kernel::KSharedMemory::Create(
            system.Kernel(), system.DeviceMemory(), nullptr, {font_phys_addr, font_size / PageSize},
            KMemoryPermission::None, KMemoryPermission::Read, font_phys_addr, font_size,
            "Font:SharedMemory");
        irs_shared_mem = Kernel::KSharedMemory::Create(
            system.Kernel(), system.DeviceMemory(), nullptr, {irs_phys_addr, irs_size / PageSize},
            KMemoryPermission::None, KMemoryPermission::Read, irs_phys_addr, irs_size,
            "IRS:SharedMemory");
        time_shared_mem = Kernel::KSharedMemory::Create(
            system.Kernel(), system.DeviceMemory(), nullptr, {time_phys_addr, time_size / PageSize},
            KMemoryPermission::None, KMemoryPermission::Read, time_phys_addr, time_size,
            "Time:SharedMemory");

        // Allocate slab heaps
        user_slab_heap_pages = std::make_unique<KSlabHeap<Page>>();

        constexpr u64 user_slab_heap_size{0x1ef000};
        // Reserve slab heaps
        ASSERT(
            system_resource_limit->Reserve(LimitableResource::PhysicalMemory, user_slab_heap_size));
        // Initialize slab heaps
        user_slab_heap_pages->Initialize(
            system.DeviceMemory().GetPointer(Core::DramMemoryMap::SlabHeapBase),
            user_slab_heap_size);
    }

    std::atomic<u32> next_object_id{0};
    std::atomic<u64> next_kernel_process_id{Process::InitialKIPIDMin};
    std::atomic<u64> next_user_process_id{Process::ProcessIDMin};
    std::atomic<u64> next_thread_id{1};

    // Lists all processes that exist in the current session.
    std::vector<std::shared_ptr<Process>> process_list;
    Process* current_process = nullptr;
    std::unique_ptr<Kernel::GlobalSchedulerContext> global_scheduler_context;
    Kernel::TimeManager time_manager;

    std::shared_ptr<KResourceLimit> system_resource_limit;

    std::shared_ptr<Core::Timing::EventType> preemption_event;

    // This is the kernel's handle table or supervisor handle table which
    // stores all the objects in place.
    HandleTable global_handle_table;

    /// Map of named ports managed by the kernel, which can be retrieved using
    /// the ConnectToPort SVC.
    NamedPortTable named_ports;

    std::unique_ptr<Core::ExclusiveMonitor> exclusive_monitor;
    std::vector<Kernel::PhysicalCore> cores;

    // Next host thead ID to use, 0-3 IDs represent core threads, >3 represent others
    std::atomic<u32> next_host_thread_id{Core::Hardware::NUM_CPU_CORES};

    // Kernel memory management
    std::unique_ptr<KMemoryManager> memory_manager;
    std::unique_ptr<KSlabHeap<Page>> user_slab_heap_pages;

    // Shared memory for services
    std::shared_ptr<Kernel::KSharedMemory> hid_shared_mem;
    std::shared_ptr<Kernel::KSharedMemory> font_shared_mem;
    std::shared_ptr<Kernel::KSharedMemory> irs_shared_mem;
    std::shared_ptr<Kernel::KSharedMemory> time_shared_mem;

    // Threads used for services
    std::unordered_set<std::shared_ptr<Kernel::ServiceThread>> service_threads;

    // Service threads are managed by a worker thread, so that a calling service thread can queue up
    // the release of itself
    std::unique_ptr<Common::ThreadWorker> service_thread_manager;

    std::array<std::shared_ptr<KThread>, Core::Hardware::NUM_CPU_CORES> suspend_threads{};
    std::array<Core::CPUInterruptHandler, Core::Hardware::NUM_CPU_CORES> interrupts{};
    std::array<std::unique_ptr<Kernel::KScheduler>, Core::Hardware::NUM_CPU_CORES> schedulers{};

    bool is_multicore{};
    bool is_phantom_mode_for_singlecore{};
    u32 single_core_thread_id{};

    std::array<u64, Core::Hardware::NUM_CPU_CORES> svc_ticks{};

    // System context
    Core::System& system;
};

KernelCore::KernelCore(Core::System& system) : impl{std::make_unique<Impl>(system, *this)} {}
KernelCore::~KernelCore() {
    Shutdown();
}

void KernelCore::SetMulticore(bool is_multicore) {
    impl->SetMulticore(is_multicore);
}

void KernelCore::Initialize() {
    impl->Initialize(*this);
}

void KernelCore::InitializeCores() {
    impl->InitializeCores();
}

void KernelCore::Shutdown() {
    impl->Shutdown();
}

std::shared_ptr<KResourceLimit> KernelCore::GetSystemResourceLimit() const {
    return impl->system_resource_limit;
}

std::shared_ptr<KThread> KernelCore::RetrieveThreadFromGlobalHandleTable(Handle handle) const {
    return impl->global_handle_table.Get<KThread>(handle);
}

void KernelCore::AppendNewProcess(std::shared_ptr<Process> process) {
    impl->process_list.push_back(std::move(process));
}

void KernelCore::MakeCurrentProcess(Process* process) {
    impl->MakeCurrentProcess(process);
}

Process* KernelCore::CurrentProcess() {
    return impl->current_process;
}

const Process* KernelCore::CurrentProcess() const {
    return impl->current_process;
}

const std::vector<std::shared_ptr<Process>>& KernelCore::GetProcessList() const {
    return impl->process_list;
}

Kernel::GlobalSchedulerContext& KernelCore::GlobalSchedulerContext() {
    return *impl->global_scheduler_context;
}

const Kernel::GlobalSchedulerContext& KernelCore::GlobalSchedulerContext() const {
    return *impl->global_scheduler_context;
}

Kernel::KScheduler& KernelCore::Scheduler(std::size_t id) {
    return *impl->schedulers[id];
}

const Kernel::KScheduler& KernelCore::Scheduler(std::size_t id) const {
    return *impl->schedulers[id];
}

Kernel::PhysicalCore& KernelCore::PhysicalCore(std::size_t id) {
    return impl->cores[id];
}

const Kernel::PhysicalCore& KernelCore::PhysicalCore(std::size_t id) const {
    return impl->cores[id];
}

Kernel::PhysicalCore& KernelCore::CurrentPhysicalCore() {
    u32 core_id = impl->GetCurrentHostThreadID();
    ASSERT(core_id < Core::Hardware::NUM_CPU_CORES);
    return impl->cores[core_id];
}

const Kernel::PhysicalCore& KernelCore::CurrentPhysicalCore() const {
    u32 core_id = impl->GetCurrentHostThreadID();
    ASSERT(core_id < Core::Hardware::NUM_CPU_CORES);
    return impl->cores[core_id];
}

Kernel::KScheduler* KernelCore::CurrentScheduler() {
    u32 core_id = impl->GetCurrentHostThreadID();
    if (core_id >= Core::Hardware::NUM_CPU_CORES) {
        // This is expected when called from not a guest thread
        return {};
    }
    return impl->schedulers[core_id].get();
}

std::array<Core::CPUInterruptHandler, Core::Hardware::NUM_CPU_CORES>& KernelCore::Interrupts() {
    return impl->interrupts;
}

const std::array<Core::CPUInterruptHandler, Core::Hardware::NUM_CPU_CORES>& KernelCore::Interrupts()
    const {
    return impl->interrupts;
}

Kernel::TimeManager& KernelCore::TimeManager() {
    return impl->time_manager;
}

const Kernel::TimeManager& KernelCore::TimeManager() const {
    return impl->time_manager;
}

Core::ExclusiveMonitor& KernelCore::GetExclusiveMonitor() {
    return *impl->exclusive_monitor;
}

const Core::ExclusiveMonitor& KernelCore::GetExclusiveMonitor() const {
    return *impl->exclusive_monitor;
}

void KernelCore::InvalidateAllInstructionCaches() {
    for (auto& physical_core : impl->cores) {
        physical_core.ArmInterface().ClearInstructionCache();
    }
}

void KernelCore::InvalidateCpuInstructionCacheRange(VAddr addr, std::size_t size) {
    for (auto& physical_core : impl->cores) {
        if (!physical_core.IsInitialized()) {
            continue;
        }
        physical_core.ArmInterface().InvalidateCacheRange(addr, size);
    }
}

void KernelCore::PrepareReschedule(std::size_t id) {
    // TODO: Reimplement, this
}

void KernelCore::AddNamedPort(std::string name, std::shared_ptr<ClientPort> port) {
    impl->named_ports.emplace(std::move(name), std::move(port));
}

KernelCore::NamedPortTable::iterator KernelCore::FindNamedPort(const std::string& name) {
    return impl->named_ports.find(name);
}

KernelCore::NamedPortTable::const_iterator KernelCore::FindNamedPort(
    const std::string& name) const {
    return impl->named_ports.find(name);
}

bool KernelCore::IsValidNamedPort(NamedPortTable::const_iterator port) const {
    return port != impl->named_ports.cend();
}

u32 KernelCore::CreateNewObjectID() {
    return impl->next_object_id++;
}

u64 KernelCore::CreateNewThreadID() {
    return impl->next_thread_id++;
}

u64 KernelCore::CreateNewKernelProcessID() {
    return impl->next_kernel_process_id++;
}

u64 KernelCore::CreateNewUserProcessID() {
    return impl->next_user_process_id++;
}

Kernel::HandleTable& KernelCore::GlobalHandleTable() {
    return impl->global_handle_table;
}

const Kernel::HandleTable& KernelCore::GlobalHandleTable() const {
    return impl->global_handle_table;
}

void KernelCore::RegisterCoreThread(std::size_t core_id) {
    impl->RegisterCoreThread(core_id);
}

void KernelCore::RegisterHostThread() {
    impl->RegisterHostThread();
}

u32 KernelCore::GetCurrentHostThreadID() const {
    return impl->GetCurrentHostThreadID();
}

KThread* KernelCore::GetCurrentEmuThread() const {
    return impl->GetCurrentEmuThread();
}

KMemoryManager& KernelCore::MemoryManager() {
    return *impl->memory_manager;
}

const KMemoryManager& KernelCore::MemoryManager() const {
    return *impl->memory_manager;
}

KSlabHeap<Page>& KernelCore::GetUserSlabHeapPages() {
    return *impl->user_slab_heap_pages;
}

const KSlabHeap<Page>& KernelCore::GetUserSlabHeapPages() const {
    return *impl->user_slab_heap_pages;
}

Kernel::KSharedMemory& KernelCore::GetHidSharedMem() {
    return *impl->hid_shared_mem;
}

const Kernel::KSharedMemory& KernelCore::GetHidSharedMem() const {
    return *impl->hid_shared_mem;
}

Kernel::KSharedMemory& KernelCore::GetFontSharedMem() {
    return *impl->font_shared_mem;
}

const Kernel::KSharedMemory& KernelCore::GetFontSharedMem() const {
    return *impl->font_shared_mem;
}

Kernel::KSharedMemory& KernelCore::GetIrsSharedMem() {
    return *impl->irs_shared_mem;
}

const Kernel::KSharedMemory& KernelCore::GetIrsSharedMem() const {
    return *impl->irs_shared_mem;
}

Kernel::KSharedMemory& KernelCore::GetTimeSharedMem() {
    return *impl->time_shared_mem;
}

const Kernel::KSharedMemory& KernelCore::GetTimeSharedMem() const {
    return *impl->time_shared_mem;
}

void KernelCore::Suspend(bool in_suspention) {
    const bool should_suspend = exception_exited || in_suspention;
    {
        KScopedSchedulerLock lock(*this);
        const auto state = should_suspend ? ThreadState::Runnable : ThreadState::Waiting;
        for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
            impl->suspend_threads[i]->SetState(state);
            impl->suspend_threads[i]->SetWaitReasonForDebugging(
                ThreadWaitReasonForDebugging::Suspended);
        }
    }
}

bool KernelCore::IsMulticore() const {
    return impl->is_multicore;
}

void KernelCore::ExceptionalExit() {
    exception_exited = true;
    Suspend(true);
}

void KernelCore::EnterSVCProfile() {
    std::size_t core = impl->GetCurrentHostThreadID();
    impl->svc_ticks[core] = MicroProfileEnter(MICROPROFILE_TOKEN(Kernel_SVC));
}

void KernelCore::ExitSVCProfile() {
    std::size_t core = impl->GetCurrentHostThreadID();
    MicroProfileLeave(MICROPROFILE_TOKEN(Kernel_SVC), impl->svc_ticks[core]);
}

std::weak_ptr<Kernel::ServiceThread> KernelCore::CreateServiceThread(const std::string& name) {
    auto service_thread = std::make_shared<Kernel::ServiceThread>(*this, 1, name);
    impl->service_thread_manager->QueueWork(
        [this, service_thread] { impl->service_threads.emplace(service_thread); });
    return service_thread;
}

void KernelCore::ReleaseServiceThread(std::weak_ptr<Kernel::ServiceThread> service_thread) {
    impl->service_thread_manager->QueueWork([this, service_thread] {
        if (auto strong_ptr = service_thread.lock()) {
            impl->service_threads.erase(strong_ptr);
        }
    });
}

bool KernelCore::IsPhantomModeForSingleCore() const {
    return impl->IsPhantomModeForSingleCore();
}

void KernelCore::SetIsPhantomModeForSingleCore(bool value) {
    impl->SetIsPhantomModeForSingleCore(value);
}

} // namespace Kernel
