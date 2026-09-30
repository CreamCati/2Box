#include "CpuAffinity.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

namespace cpu_affinity
{
    namespace
    {
        std::vector<PhysicalCore> query_physical_cores()
        {
            DWORD length = 0;
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
                GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            {
                throw std::runtime_error(std::format(
                    "GetLogicalProcessorInformationEx(size) failed, error code: {}", GetLastError()));
            }

            std::vector<std::byte> buffer(length);
            auto* current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

            if (!GetLogicalProcessorInformationEx(
                    RelationProcessorCore,
                    current,
                    &length))
            {
                throw std::runtime_error(std::format(
                    "GetLogicalProcessorInformationEx(data) failed, error code: {}", GetLastError()));
            }

            std::vector<PhysicalCore> cores;
            DWORD offset = 0;

            while (offset < length)
            {
                current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);

                if (current->Relationship == RelationProcessorCore)
                {
                    const PROCESSOR_RELATIONSHIP& relationship = current->Processor;

                    // RelationProcessorCore always has GroupCount == 1.
                    // Keep the defensive check because this code should never
                    // rely on malformed topology data.
                    if (relationship.GroupCount == 1)
                    {
                        const GROUP_AFFINITY& group = relationship.GroupMask[0];

                        cores.push_back(PhysicalCore{
                            group.Group,
                            group.Mask,
                            relationship.EfficiencyClass
                        });
                    }
                }

                if (current->Size == 0)
                    break;

                offset += current->Size;
            }

            if (cores.empty())
            {
                throw std::runtime_error("No physical processor cores were found");
            }

            return cores;
        }

        std::vector<PhysicalCore> select_usable_cores(std::vector<PhysicalCore> cores)
        {
            // EfficiencyClass is zero on homogeneous systems.
            // It is non-zero on heterogeneous systems and a higher value
            // represents the higher-performance / lower-efficiency core class.
            // Therefore, when heterogeneous cores are present, keep only the
            // highest class (P-cores on Intel hybrid desktop CPUs).
            BYTE maxEfficiencyClass = 0;
            bool heterogeneous = false;

            for (const PhysicalCore& core : cores)
            {
                maxEfficiencyClass = std::max(maxEfficiencyClass, core.efficiencyClass);
            }

            for (const PhysicalCore& core : cores)
            {
                if (core.efficiencyClass != 0)
                {
                    heterogeneous = true;
                    break;
                }
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

            // Make the allocation deterministic by group/mask instead of
            // relying on undocumented enumeration order.
            std::sort(
                cores.begin(),
                cores.end(),
                [](const PhysicalCore& a, const PhysicalCore& b)
                {
                    if (a.group != b.group)
                        return a.group < b.group;
                    return a.mask < b.mask;
                });

            return cores;
        }
    }

    std::vector<PhysicalCore> get_usable_physical_cores()
    {
        return select_usable_cores(query_physical_cores());
    }

    bool assign_next_core(HANDLE processHandle, HANDLE primaryThread, std::size_t instanceIndex)
    {
        if (!processHandle || !primaryThread)
            return false;

        const std::vector<PhysicalCore> cores = get_usable_physical_cores();
        if (cores.empty())
            return false;

        const PhysicalCore& core = cores[instanceIndex % cores.size()];

        // The target process is created suspended and therefore has only its
        // primary thread running. Setting the primary thread's group affinity
        // before ResumeThread makes it start on exactly this physical core.
        // The mask contains all logical processors of that physical core, so
        // SMT/HT siblings belong to the same instance instead of becoming
        // separate "cores".
        GROUP_AFFINITY groupAffinity{};
        groupAffinity.Mask = core.mask;
        groupAffinity.Group = core.group;

        if (!SetThreadGroupAffinity(primaryThread, &groupAffinity, nullptr))
        {
            return false;
        }

        // On systems with <= 64 logical processors, the normal process-level
        // affinity API is also safe and makes the intended process affinity
        // explicit. For larger systems SetThreadGroupAffinity above is the
        // group-aware path.
        if (GetActiveProcessorGroupCount() == 1)
        {
            if (!SetProcessAffinityMask(processHandle, static_cast<DWORD_PTR>(core.mask)))
            {
                // The thread affinity has already been applied. Do not fail
                // the launch solely because the redundant process-level call
                // was rejected by an unusual security/topology configuration.
                return true;
            }
        }

        return true;
    }
}
