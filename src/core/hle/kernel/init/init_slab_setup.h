// Copyright 2021 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace Core {
class System;
} // namespace Core

namespace Kernel {
class KernelCore;
class KMemoryLayout;
} // namespace Kernel

namespace Kernel::Init {

struct KSlabResourceCounts {
    static KSlabResourceCounts CreateDefault();

    size_t num_KProcess;
    size_t num_KThread;
    size_t num_KEvent;
    size_t num_KInterruptEvent;
    size_t num_KPort;
    size_t num_KSharedMemory;
    size_t num_KTransferMemory;
    size_t num_KCodeMemory;
    size_t num_KDeviceAddressSpace;
    size_t num_KSession;
    size_t num_KLightSession;
    size_t num_KObjectName;
    size_t num_KResourceLimit;
    size_t num_KDebug;
    size_t num_KAlpha;
    size_t num_KBeta;
};

void InitializeSlabResourceCounts(KernelCore& kernel);
size_t CalculateTotalSlabHeapSize(const KernelCore& kernel);
void InitializeSlabHeaps(Core::System& system, KMemoryLayout& memory_layout);

} // namespace Kernel::Init
