#pragma once

#include <cstddef>

namespace cpu_affinity
{
    // Assign one usable physical CPU core to a newly created, suspended
    // process.  The first instance gets the first usable core, the next
    // instance gets the next core, and so on; after the last core it wraps.
    //
    // The public header intentionally does not include Windows.h or STL
    // containers.  Launcher.cpp is a C++ module implementation unit, and
    // pulling Windows/STL headers into the module purview can cause MSVC STL
    // declarations to be parsed twice.
    bool assign_next_core(
        void* processHandle,
        void* primaryThread,
        std::size_t instanceIndex);
}
