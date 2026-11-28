#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CStaticMesh;
    class CWorld;
    class FThumbnailScene;

    // Bakes octahedral impostors one mesh at a time across editor frames, so texture streaming can deliver the mips the capture reads.
    class EDITOR_API FImpostorBaker
    {
    public:

        FImpostorBaker();
        ~FImpostorBaker();

        void Enqueue(CStaticMesh* Mesh);

        // Every mesh a forest or painted foliage in World scatters.
        void EnqueueFoliage(CWorld& World);

        // Every mesh a static mesh component in World draws, unless the component keeps its full mesh at every distance.
        void EnqueueStaticMeshes(CWorld& World);

        void Tick();

        bool IsBusy() const { return Active != nullptr || !Building.IsEmpty() || !Queue.IsEmpty(); }

    private:

        friend struct FImpostorBakerTestAccess;

        struct FJob;

        bool StartNext();
        // One capture world serves the whole batch, since building one per mesh stalled the editor.
        FThumbnailScene* AcquireScene(uint32 RenderSize);
        // Submits the readback without waiting, so the GPU copy and a worker's atlas build never stall the editor.
        void Capture();
        void PollBuild();
        static void BuildAtlas(FJob& Job);
        void Apply(FJob& Job);
        // Queues every forest and foliage mesh of the open scene that has no impostor yet, each once a session, so nothing waits on a manual bake.
        void QueueMissingFoliageImpostors();

        TVector<TWeakObjectPtr<CStaticMesh>> Queue;
        TUniquePtr<FJob>                     Active;
        // Captured jobs, oldest first, whose readbacks are landing or whose atlases workers are building.
        TVector<TUniquePtr<FJob>>            Building;
        TUniquePtr<FThumbnailScene>          Scene;
        TVector<TWeakObjectPtr<CStaticMesh>> AutoQueued;
        uint32                               FramesToAutoScan = 0;
    };

    // Test-only seam into Active and Building, since reaching them through StartNext needs a GPU capture.
    struct EDITOR_API FImpostorBakerTestAccess
    {
        static void MarkActive(FImpostorBaker& Baker, CStaticMesh* Mesh);
        static void MarkBuilding(FImpostorBaker& Baker, CStaticMesh* Mesh);
        static int32 QueueSize(const FImpostorBaker& Baker) { return Baker.Queue.Num(); }
        // Drops a mock job before the baker destructs, since a real one would need RHI waits the test never set up.
        static void Reset(FImpostorBaker& Baker);
    };
}
