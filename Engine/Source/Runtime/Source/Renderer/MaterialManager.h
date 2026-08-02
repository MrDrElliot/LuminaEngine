#pragma once

#include "RHI.h"
#include "MaterialTypes.h"
#include "Containers/Array.h"
#include "Containers/SparseArray.h"

namespace Lumina
{
    class CMaterialInterface;
    struct FMaterialUniforms;
}

namespace Lumina::RHI
{
    class FMaterialManager
    {
    public:

        FMaterialManager();
        ~FMaterialManager();

        void AddMaterial(CMaterialInterface* Material);
        void RemoveMaterial(CMaterialInterface* Material);

        void UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index);
        GPUPtr GetMaterialBuffer() const { return MaterialBuffer; }

    private:

        FSharedMutex                            Mutex;
        TStack<uint32>                          FreeList;
        GPUPtr                                  MaterialBuffer = 0;
    };
}
