#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CStaticMesh;
    class CWorld;

    // Bakes octahedral impostors one mesh at a time across editor frames, so texture streaming can deliver the mips the capture reads.
    class FImpostorBaker
    {
    public:

        FImpostorBaker();
        ~FImpostorBaker();

        void Enqueue(CStaticMesh* Mesh);

        // Every mesh a forest or painted foliage in World scatters.
        void EnqueueFoliage(CWorld& World);

        void Tick();

        bool IsBusy() const { return Active != nullptr || !Queue.IsEmpty(); }

    private:

        struct FJob;

        bool StartNext();
        void Finish(FJob& Job);

        TVector<TWeakObjectPtr<CStaticMesh>> Queue;
        TUniquePtr<FJob>                     Active;
    };
}
