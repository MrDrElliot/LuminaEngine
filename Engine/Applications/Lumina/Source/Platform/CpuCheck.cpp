#include "CpuCheck.h"

#include <cstdint>

#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__x86_64__)
    #include <cpuid.h>
#endif

#if defined(_WIN32)
    #include <Windows.h>
#else
    #include <cstdio>
#endif

namespace
{
#if defined(_M_X64) || defined(__x86_64__)
    void ReadCpuid(int Leaf, int SubLeaf, uint32_t Out[4])
    {
    #if defined(_MSC_VER)
        int Registers[4] = {};
        __cpuidex(Registers, Leaf, SubLeaf);
        for (int Index = 0; Index < 4; ++Index)
        {
            Out[Index] = (uint32_t)Registers[Index];
        }
    #else
        __cpuid_count(Leaf, SubLeaf, Out[0], Out[1], Out[2], Out[3]);
    #endif
    }

    uint64_t ReadEnabledRegisterState()
    {
    #if defined(_MSC_VER)
        return _xgetbv(0);
    #else
        uint32_t Low = 0;
        uint32_t High = 0;
        __asm__ volatile("xgetbv" : "=a"(Low), "=d"(High) : "c"(0));
        return ((uint64_t)High << 32) | Low;
    #endif
    }

    constexpr uint32_t Bit(int Index)
    {
        return 1u << Index;
    }
#endif
}

bool LuminaHasRequiredCpu()
{
#if defined(_M_X64) || defined(__x86_64__)
    uint32_t Features[4] = {};
    ReadCpuid(1, 0, Features);
    const uint32_t Ecx1 = Features[2];

    // AVX2 instructions fault unless the OS saves the upper halves of the vector registers on a context switch.
    const bool bOsSavesYmm = (Ecx1 & Bit(27)) != 0 && (ReadEnabledRegisterState() & 0x6) == 0x6;
    const bool bFma = (Ecx1 & Bit(12)) != 0;
    const bool bF16c = (Ecx1 & Bit(29)) != 0;

    ReadCpuid(7, 0, Features);
    const uint32_t Ebx7 = Features[1];
    const bool bAvx2 = (Ebx7 & Bit(5)) != 0;
    const bool bBmi1 = (Ebx7 & Bit(3)) != 0;
    const bool bBmi2 = (Ebx7 & Bit(8)) != 0;

    return bOsSavesYmm && bAvx2 && bFma && bF16c && bBmi1 && bBmi2;
#else
    return true;
#endif
}

void LuminaReportUnsupportedCpu()
{
    const char* Message =
        "Lumina needs a processor with AVX2, FMA and BMI2, which means an Intel Haswell (2013) or AMD Zen (2017) "
        "processor or newer. This computer's processor does not have them, so Lumina cannot start.";

#if defined(_WIN32)
    MessageBoxA(nullptr, Message, "Lumina", MB_OK | MB_ICONERROR);
#else
    std::fputs(Message, stderr);
    std::fputs("\n", stderr);
#endif
}
