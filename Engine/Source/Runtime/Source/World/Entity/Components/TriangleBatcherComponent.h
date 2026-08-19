#pragma once
#include "Containers/Vector.h"
#include "Memory/MemoryConcurrentQueue.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/Vertex.h"

namespace Lumina
{
    // Solid debug triangles. Submitted in bulk (one batch == one whole vertex span) so a
    // large surface (e.g. a navmesh overlay) is a single enqueue, not thousands of per-triangle ones.
    struct RUNTIME_API FTriangleBatcherComponent
    {
        static constexpr auto in_place_delete = true;

        struct FBatchInstance
        {
            TVector<FSimpleElementVertex>   Vertices;           // 3 verts per triangle, pre-colored
            float                           RemainingLifetime;
            ESolidDrawMode                  Mode;
            uint8                           bSingleFrame:1;
        };

        struct FQueuedBatch
        {
            TVector<FSimpleElementVertex>   Vertices;
            float                           Duration;
            ESolidDrawMode                  Mode;
            uint8                           bSingleFrame:1;
        };

        TVector<FBatchInstance>             Batches;

        TConcurrentQueue<FQueuedBatch>      Queue;

        // Thread-safe. Becomes visible the next time DrainQueue runs (once per render-extraction tick).
        void EnqueueTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode = ESolidDrawMode::Translucent, float Duration = -1.0f)
        {
            if (Vertices.empty())
            {
                return;
            }
            FQueuedBatch Q;
            Q.Vertices      = std::move(Vertices);
            Q.Duration      = Duration;
            Q.Mode          = Mode;
            Q.bSingleFrame  = (Duration == -1.0f) ? 1u : 0u;
            Queue.enqueue(std::move(Q));
        }

        // Single-threaded; call before reading Batches for render extraction.
        void DrainQueue()
        {
            FQueuedBatch Q;
            while (Queue.try_dequeue(Q))
            {
                Batches.emplace_back(FBatchInstance
                {
                    .Vertices           = std::move(Q.Vertices),
                    .RemainingLifetime  = Q.Duration,
                    .Mode               = Q.Mode,
                    .bSingleFrame       = Q.bSingleFrame,
                });
            }
        }
    };
}
