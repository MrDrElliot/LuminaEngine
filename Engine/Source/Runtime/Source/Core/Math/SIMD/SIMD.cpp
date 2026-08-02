#include "SIMD.h"

// Lumina::SIMD is header-only; this TU just forces the headers through the compiler.

namespace Lumina::SIMD
{
    static_assert(sizeof(VFloat4) == 16 && alignof(VFloat4) == 16, "VFloat4 must be a 16-byte, 16-aligned __m128");
    static_assert(sizeof(VFloat8) == 32 && alignof(VFloat8) == 32, "VFloat8 must be a 32-byte, 32-aligned __m256");
    static_assert(VFloat4::Width == 4 && VFloat8::Width == 8, "Lane width mismatch");

    // Touch a representative slice of the API so it is instantiated and
    // type-checked even before any real call site exists.
    [[maybe_unused]] static VFloat8 SmokeTest(const float* A, const float* B, float S)
    {
        VFloat8 Va = VFloat8::Load(A);
        VFloat8 Vb = VFloat8::Load(B);
        VFloat8 R  = MulAdd(Va, VFloat8::Broadcast(S), Vb);
        R          = Select(CmpGt(R, VFloat8::Zero()), Min(R, Vb), Max(R, Va));
        return Sqrt(Abs(R));
    }
}
