#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Threading/Thread.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class CMaterial;

    // Compile work found while loading, filled from the parallel PostLoad wave and drained by the editor's scheduler.
    class RUNTIME_API FMaterialCompileQueue
    {
    public:
        FMaterialCompileQueue();
        ~FMaterialCompileQueue();

        // A material with no bytecode for the current templates, or a legacy one that has never filled the artifact cache.
        void QueueRecompile(CMaterial* Material);

        // By GUID, since a material that calls a changed function need not be loaded.
        void QueueFunctionRecompile(const FGuid& MaterialGUID);

        // Idempotent, so every instance may ask for the permutation it selects.
        void QueuePermutation(CMaterial* Material, uint64 Key);

        TStrongObjectPtr<CMaterial> PopRecompile();
        bool PopFunctionRecompile(FGuid& OutMaterialGUID);
        bool PopPermutation(TStrongObjectPtr<CMaterial>& OutMaterial, uint64& OutKey);

        bool IsFunctionRecompileQueued(const FGuid& MaterialGUID) const;

        bool HasRecompiles() const;

    private:
        struct FPermutationRequest
        {
            TStrongObjectPtr<CMaterial> Material;
            uint64                      Key = 0;
        };

        mutable FMutex                       Mutex;
        TVector<TStrongObjectPtr<CMaterial>> Recompiles;
        THashSet<const CMaterial*>           QueuedRecompiles;
        TVector<FGuid>                       FunctionRecompiles;
        THashSet<FGuid>                      QueuedFunctionRecompiles;
        TVector<FPermutationRequest>         Permutations;
    };
}
