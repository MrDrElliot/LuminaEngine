#pragma once

#include "Containers/StaticArray.h"
#include "Renderer/RHI.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    // Zeroed is an opt-in with a stated invariant; Undefined is poisoned in Development to expose missed writes.
    enum class EBufferInit : uint8
    {
        Undefined,
        Zeroed,
    };

    // A GPU-only scene buffer that carries its growth policy, so every reserve of it reads the same numbers.
    struct FSceneBuffer : RHI::FGPUAllocation
    {
        const char* DebugName      = nullptr;
        float       Slack          = 1.5f;
        EBufferInit Init           = EBufferInit::Undefined;
        bool        bAllowShrink   = true;
        uint32      LowUsageFrames = 0;
        // Reserves since the last growth, saturating, which tells a buffer being streamed into from one that grew once.
        uint32      ReservesSinceGrowth = ~0u;
        // The most the slack may reach, for a buffer whose readers never address past a fixed count.
        uint64      MaxBytes       = ~0ull;

        constexpr FSceneBuffer() = default;
        constexpr FSceneBuffer(const char* InDebugName, float InSlack, EBufferInit InInit = EBufferInit::Undefined, bool bInAllowShrink = true)
            : DebugName(InDebugName), Slack(InSlack), Init(InInit), bAllowShrink(bInAllowShrink)
        {
        }

        void Allocate(uint64 Bytes)
        {
            Adopt(CreateSceneBuffer(Bytes, DebugName));
        }

        void Adopt(const RHI::FGPUAllocation& Allocation)
        {
            static_cast<RHI::FGPUAllocation&>(*this) = Allocation;
            LowUsageFrames = 0;
        }

        void Release()
        {
            if (Gpu != 0)
            {
                RHI::Retire(*this);
            }
            Adopt({});
        }

        NODISCARD uint64 SlackedBytes(uint64 NeededBytes) const
        {
            const uint64 Bytes = Math::Max(NeededBytes, Math::Min((uint64)((double)NeededBytes * Slack), MaxBytes));
            return (Bytes + 15ull) & ~15ull;
        }

        // A buffer that outgrows itself again soon after growing is being streamed into, so it at least doubles instead of stepping by its slack.
        NODISCARD uint64 GrownBytes(uint64 NeededBytes) const
        {
            const uint64 Slacked = SlackedBytes(NeededBytes);
            if (ReservesSinceGrowth >= kGrowthStreakReserves)
            {
                return Slacked;
            }
            return Math::Max(Slacked, (Math::Min(Size * 2, MaxBytes) + 15) & ~uint64(15));
        }

        // About two seconds of frames, the spacing a streaming load's growths fall inside.
        static constexpr uint32 kGrowthStreakReserves = 240;

        template<typename T>
        NODISCARD uint32 CapacityOf() const
        {
            return (uint32)Math::Min<uint64>(Size / sizeof(T), 0xFFFFFFFFull);
        }
    };

    // The whole margin for a demand-fed buffer, both its spike headroom and its reallocation spacing.
    inline constexpr float kSceneBufferGrowth = 1.5f;

    template<size_t N>
    constexpr TArray<FSceneBuffer, N> MakeSceneRing(const char* DebugName, float Slack, EBufferInit Init = EBufferInit::Undefined, bool bAllowShrink = true)
    {
        TArray<FSceneBuffer, N> Ring = {};
        for (FSceneBuffer& Buffer : Ring)
        {
            Buffer = FSceneBuffer(DebugName, Slack, Init, bAllowShrink);
        }
        return Ring;
    }
}
