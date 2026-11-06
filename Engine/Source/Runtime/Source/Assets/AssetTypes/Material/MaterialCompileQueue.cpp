#include "RuntimePCH.h"
#include "MaterialCompileQueue.h"
#include "Material.h"
#include "Containers/Algorithm.h"

namespace Lumina
{
    FMaterialCompileQueue::FMaterialCompileQueue() = default;
    FMaterialCompileQueue::~FMaterialCompileQueue() = default;

    void FMaterialCompileQueue::QueueRecompile(CMaterial* Material)
    {
        FScopeLock Lock(Mutex);
        if (QueuedRecompiles.TryAdd(Material))
        {
            Recompiles.Add(Material);
        }
    }

    void FMaterialCompileQueue::QueueFunctionRecompile(const FGuid& MaterialGUID)
    {
        FScopeLock Lock(Mutex);
        if (QueuedFunctionRecompiles.TryAdd(MaterialGUID))
        {
            FunctionRecompiles.Add(MaterialGUID);
        }
    }

    void FMaterialCompileQueue::QueuePermutation(CMaterial* Material, uint64 Key)
    {
        // A cooked material has no graph to compile from, and the default permutation always exists.
        if (!IsValid(Material) || Material->GetPackage() == nullptr || Material->HasPermutation(Key))
        {
            return;
        }

        FScopeLock Lock(Mutex);
        const bool bQueued = Algo::AnyOf(Permutations, [Material, Key](const FPermutationRequest& Request)
        {
            return Request.Material == Material && Request.Key == Key;
        });
        if (!bQueued)
        {
            Permutations.Emplace(Material, Key);
        }
    }

    TStrongObjectPtr<CMaterial> FMaterialCompileQueue::PopRecompile()
    {
        FScopeLock Lock(Mutex);
        if (Recompiles.IsEmpty())
        {
            return nullptr;
        }
        TStrongObjectPtr<CMaterial> Material = Move(Recompiles.Last());
        Recompiles.Pop();
        QueuedRecompiles.Remove(Material.Get());
        return Material;
    }

    bool FMaterialCompileQueue::PopFunctionRecompile(FGuid& OutMaterialGUID)
    {
        FScopeLock Lock(Mutex);
        if (FunctionRecompiles.IsEmpty())
        {
            return false;
        }
        OutMaterialGUID = FunctionRecompiles.Last();
        FunctionRecompiles.Pop();
        QueuedFunctionRecompiles.Remove(OutMaterialGUID);
        return true;
    }

    bool FMaterialCompileQueue::PopPermutation(TStrongObjectPtr<CMaterial>& OutMaterial, uint64& OutKey)
    {
        FScopeLock Lock(Mutex);
        if (Permutations.IsEmpty())
        {
            return false;
        }
        OutMaterial = Move(Permutations.Last().Material);
        OutKey      = Permutations.Last().Key;
        Permutations.Pop();
        return true;
    }

    bool FMaterialCompileQueue::IsFunctionRecompileQueued(const FGuid& MaterialGUID) const
    {
        FScopeLock Lock(Mutex);
        return QueuedFunctionRecompiles.Contains(MaterialGUID);
    }
}
