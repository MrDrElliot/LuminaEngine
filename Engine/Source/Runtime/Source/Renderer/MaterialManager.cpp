#include "RuntimePCH.h"
#include "MaterialManager.h"
#include "MaterialTypes.h"
#include "RHICore.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"

namespace Lumina::RHI
{
    static constexpr uint32 MaxMaterialUniforms = 1024;

    FMaterialManager::FMaterialManager()
    {
        MaterialBuffer = Malloc(sizeof(FMaterialUniforms) * MaxMaterialUniforms, kDefaultAlign, EMemoryType::GPUOnly);

        for (uint32 i = 0; i < MaxMaterialUniforms; ++i)
        {
            FreeList.push(i);
        }
    }

    FMaterialManager::~FMaterialManager()
    {
        Core::Retire(MaterialBuffer);
        MaterialBuffer = 0;
    }

    void FMaterialManager::AddMaterial(CMaterialInterface* Material)
    {
        FWriteScopeLock Lock(Mutex);

        DEBUG_ASSERT(Material != nullptr);
        DEBUG_ASSERT(Material->GetMaterialIndex() == -1);
        DEBUG_ASSERT(!FreeList.empty());

        uint32 FreeIndex = FreeList.top();
        FreeList.pop();

        Material->SetMaterialIndex((int32)FreeIndex);
        UpdateMaterialUniforms(Material->GetMaterialUniforms(), FreeIndex);
    }

    void FMaterialManager::RemoveMaterial(CMaterialInterface* Material)
    {
        FWriteScopeLock Lock(Mutex);

        DEBUG_ASSERT(Material != nullptr);
        DEBUG_ASSERT(Material->GetMaterialIndex() != -1);

        int32 MaterialIndex = Material->GetMaterialIndex();

        UpdateMaterialUniforms(nullptr, MaterialIndex);

        Material->SetMaterialIndex(-1);

        FreeList.push(MaterialIndex);
    }

    void FMaterialManager::UpdateMaterialUniforms(const FMaterialUniforms* InUniforms, uint32 Index)
    {
        // Zeroed slot doubles as the "removed" state.
        FMaterialUniforms Copy = {};
        if (InUniforms)
        {
            Copy = *InUniforms;
        }

        UploadBuffer(MaterialBuffer + Index * sizeof(FMaterialUniforms), &Copy, sizeof(FMaterialUniforms));
    }
}
