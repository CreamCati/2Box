#include "CpuAffinity.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

namespace cpu_affinity
{
    namespace
    {
        // One physical core.  A physical core may contain multiple CPU Sets
        // (for example two SMT/Hyper-Threading logical processors).
        struct PhysicalCore
        {
            WORD group{};
            BYTE coreIndex{};
            BYTE efficiencyClass{};
            std::vector<ULONG> cpuSetIds;
        };

        std::vector<PhysicalCore> query_physical_cores()
        {
            DWORD length = 0;

            // GetSystemCpuSetInformation is available on Windows 10+.
            // The first call obtains the required buffer size.
            GetSystemCpuSetInformation(nullptr, 0, &length, nullptr, 0);
            const DWORD firstError = GetLastError();
            if (length == 0 && firstError != ERROR_INSUFFICIENT_BUFFER)
            {
                throw std::runtime_error(std::format(
                    "GetSystemCpuSetInformation(size) failed, error code: {}",
                    firstError));
            }

            std::vector<std::byte> buffer(length);
            if (!GetSystemCpuSetInformation(
                    reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                    length,
                    &length,
                    nullptr,
                    0))
            {
                throw std::runtime_error(std::format(
                    "GetSystemCpuSetInformation(data) failed, error code: {}",
                    GetLastError()));
            }

            std::vector<PhysicalCore> cores;
            DWORD offset = 0;

            while (offset < length)
            {
                auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(
                    buffer.data() + offset);

                if (info->Type == CpuSetInformation)
                {
                    const auto& cpuSet = info->CpuSet;

                    // CoreIndex is identical for CPU Sets belonging to the
                    // same physical core, including SMT siblings.
                    auto it = std::find_if(
                        cores.begin(),
                        cores.end(),
                        [&cpuSet](const PhysicalCore& core)
                        {
                            return core.group == cpuSet.Group &&
                                   core.coreIndex == cpuSet.CoreIndex;
                        });

                    if (it == cores.end())
                    {
                        PhysicalCore core;
                        core.group = cpuSet.Group;
                        core.coreIndex = cpuSet.CoreIndex;
                        core.efficiencyClass = cpuSet.EfficiencyClass;
                        core.cpuSetIds.push_back(cpuSet.Id);
                        cores.push_back(std::move(core));
                    }
                    else
                    {
                        it->cpuSetIds.push_back(cpuSet.Id);
                    }
                }

                if (info->Size == 0)
                    break;

                offset += info->Size;
            }

            if (cores.empty())
            {
                throw std::runtime_error("No physical processor cores were found");
            }

            return cores;
        }

        std::vector<PhysicalCore> select_usable_cores(std::vector<PhysicalCore> cores)
        {
            // Microsoft defines a higher EfficiencyClass as intrinsically
            // higher performance and lower power efficiency.  On Intel P/E
            // systems this lets us keep P-cores and exclude E-cores.
            BYTE maxEfficiencyClass = 0;
            bool heterogeneous = false;

            for (const PhysicalCore& core : cores)
            {
                if (core.efficiencyClass != 0)
                    heterogeneous = true;

                if (core.efficiencyClass > maxEfficiencyClass)
                    maxEfficiencyClass = core.efficiencyClass;
            }

            if (heterogeneous)
            {
                cores.erase(
                    std::remove_if(
                        cores.begin(),
                        cores.end(),
                        [maxEfficiencyClass](const PhysicalCore& core)
                        {
                            return core.efficiencyClass != maxEfficiencyClass;
                        }),
                    cores.end());
            }

            // Make allocation deterministic.
            std::sort(
                cores.begin(),
                cores.end(),
                [](const PhysicalCore& a, const PhysicalCore& b)
                {
                    if (a.group != b.group)
                        return a.group < b.group;
                    return a.coreIndex < b.coreIndex;
                });

            return cores;
        }
    }

    bool assign_next_core(
        void* processHandle,
        void* primaryThread,
        std::size_t instanceIndex)
    {
        if (processHandle == nullptr || primaryThread == nullptr)
        {
            SetLastError(ERROR_INVALID_HANDLE);
            return false;
        }

        const std::vector<PhysicalCore> cores =
            select_usable_cores(query_physical_cores());

        if (cores.empty())
        {
            SetLastError(ERROR_NOT_FOUND);
            return false;
        }

        const PhysicalCore& core = cores[instanceIndex % cores.size()];
        HANDLE process = static_cast<HANDLE>(processHandle);
        HANDLE thread = static_cast<HANDLE>(primaryThread);

        // CPU Sets are used instead of SetProcessAffinityMask here.  This is
        // important because the 2Box executable is built as x86 in the CI;
        // DWORD_PTR is only 32 bits there and cannot represent a core whose
        // logical processor index is >= 32.  CPU Set IDs are ULONGs and work
        // correctly from an x86 controller process as well.
        //
        // SetProcessDefaultCpuSets makes newly created threads inherit the
        // selected physical core's CPU Sets, so the whole target process stays
        // on this physical core rather than only its primary thread.
        if (!SetProcessDefaultCpuSets(
                process,
                core.cpuSetIds.data(),
                static_cast<ULONG>(core.cpuSetIds.size())))
        {
            return false;
        }

        // Explicitly select the same CPU Sets for the already-created primary
        // thread.  This removes any scheduling window before ResumeThread().
        if (!SetThreadSelectedCpuSets(
                thread,
                core.cpuSetIds.data(),
                static_cast<ULONG>(core.cpuSetIds.size())))
        {
            // Clear the process default if the primary-thread operation
            // failed, then report failure to the launcher.
            SetProcessDefaultCpuSets(process, nullptr, 0);
            return false;
        }

        return true;
    }
}
