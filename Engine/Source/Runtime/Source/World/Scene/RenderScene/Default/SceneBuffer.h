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
            const uint64 Bytes = (uint64)((double)NeededBytes * Slack);
            return (Bytes + 15ull) & ~15ull;
        }

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
