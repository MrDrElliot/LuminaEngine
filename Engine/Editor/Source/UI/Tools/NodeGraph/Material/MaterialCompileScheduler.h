#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CMaterial;
    class FMaterialCompileQueue;

    // Drains FMaterialCompileQueue with several materials in flight, finishing each as soon as its own stages return.
    class EDITOR_API FMaterialCompileScheduler
    {
    public:
        FMaterialCompileScheduler();
        ~FMaterialCompileScheduler();

        FMaterialCompileScheduler(const FMaterialCompileScheduler&) = delete;
        FMaterialCompileScheduler& operator=(const FMaterialCompileScheduler&) = delete;
        FMaterialCompileScheduler(FMaterialCompileScheduler&&) = delete;
        FMaterialCompileScheduler& operator=(FMaterialCompileScheduler&&) = delete;

        void Tick();

        // A recompile that finishes after a save marks its package dirty again, so a save waits for every queued one first.
        void FinishPendingRecompiles();

    private:
        enum class ERequestKind : uint8
        {
            Recompile,
            FunctionRecompile,
            Permutation,
        };

        struct FRequest
        {
            TStrongObjectPtr<CMaterial> Material;
            uint64                      Key  = 0;
            ERequestKind                Kind = ERequestKind::Recompile;
        };

        struct FJob;

        void FinishSettledJobs(FMaterialCompileQueue& Queue);
        void LaunchJobs(FMaterialCompileQueue& Queue);
        bool PopRequest(FMaterialCompileQueue& Queue, FRequest& OutRequest);
        void Launch(FRequest&& Request);
        void LaunchRecompile(FRequest&& Request);
        void LaunchPermutation(FRequest&& Request);
        void FinishRecompile(FJob& Job, FMaterialCompileQueue& Queue);
        void FinishPermutation(FJob& Job);

        bool IsCompiling(const CMaterial* Material) const;
        uint32 OutstandingStages() const;

        TVector<TUniquePtr<FJob>> InFlight;

        // Requests for a material that already has a job in flight, retried once that job finishes.
        TVector<FRequest>         Deferred;

        // Reported together when the queue drains, rather than one toast per material.
        uint32                    MaterialsNeedingSave = 0;
    };
}
