#pragma once
#include "Containers/Array.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
    struct RUNTIME_API FLineBatcherComponent
    {
        static constexpr auto in_place_delete = true;

        struct FLineInstance
        {
            FVector3   Start;
            FVector3   End;
            uint32      ColorPacked;
            float       RemainingLifetime;
            float       Thickness;
            uint8       bDepthTest:1;
            uint8       bSingleFrame:1;
        };

        // Persistent (Duration >= 0) lines, carried across frames; rebuilt from survivors each frame by
        // the render-side ProcessBatchedLines.
        TVector<FLineInstance> Lines;

        // Per-worker-slot produce buffers
        TVector<TVector<FLineInstance>> ThreadBuffers;

        FLineBatcherComponent()
        {
            // One slot per addressable thread (workers + external), so GetWorkerIndex() below is always in range.
            ThreadBuffers.resize(Jobs::GetNumThreadSlots());
        }

        // Lock-free: writes only the calling thread's own slot. Visible at the next render extract.
        void EnqueueLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = -1.0f)
        {
            const uint32 Slot = Jobs::GetWorkerIndex();
            if (Slot >= ThreadBuffers.size())
            {
                return;
            }

            FLineInstance L;
            L.Start             = Start;
            L.End               = End;
            L.ColorPacked       = PackColor(Color);
            L.RemainingLifetime = Duration;
            L.Thickness         = Thickness;
            L.bDepthTest        = bDepthTest ? 1u : 0u;
            L.bSingleFrame      = Math::EpsilonEqual(Duration, -1.0f, LE_SMALL_NUMBER) ? 1u : 0u;
            ThreadBuffers[Slot].push_back(L);
        }
    };
}
