#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpu_affinity
{
    // One entry represents one physical CPU core.
    // Mask contains all logical processors belonging to that core (e.g. both
    // SMT/Hyper-Threading siblings), so an instance is assigned a core rather
    // than a single hardware thread.
    struct PhysicalCore
    {
        WORD group{};
        KAFFINITY mask{};
        BYTE efficiencyClass{};
    };

    // Returns the usable physical cores for instance affinity.
    //
    // On a normal homogeneous CPU all physical cores are returned.
    // On a heterogeneous CPU (Intel P/E-core style), only cores with the
    // highest EfficiencyClass are returned; the lower-efficiency cores are
    // intentionally excluded.
    std::vector<PhysicalCore> get_usable_physical_cores();

    // Assigns one physical core to a newly created, still-suspended process.
    // The core is selected by round-robin order and the next instance moves
    // to the next usable physical core.
    //
    // Returns true on success. The instanceIndex is zero-based.
    bool assign_next_core(HANDLE processHandle, HANDLE primaryThread, std::size_t instanceIndex);
}
