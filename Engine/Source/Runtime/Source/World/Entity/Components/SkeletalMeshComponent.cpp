#include "RuntimePCH.h"
#include "SkeletalMeshComponent.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"


namespace Lumina
{
    CMaterialInstance* SSkeletalMeshComponent::CreateDynamicMaterialInstance(uint32 Slot)
    {
        CMaterialInstance* Dynamic = MeshComponentUtils::MakeDynamicMaterialInstance(GetMaterialForSlot(Slot));
        if (Dynamic != nullptr)
        {
            SetMaterialAtSlot(Dynamic, Slot);
        }
        return Dynamic;
    }

    CMaterialInterface* SSkeletalMeshComponent::GetMaterialForSlot(uint32 Slot) const
    {
        if (Slot < static_cast<uint32>(MaterialOverrides.Num()))
        {
            if (CMaterialInterface* Interface = MaterialOverrides[Slot].Get())
            {
                return Interface;
            }
        }
        
        if (SkeletalMesh.IsValid())
        {
            return SkeletalMesh->GetMaterialAtSlot(Slot);
        }

        return nullptr;
    }

    SAABB SSkeletalMeshComponent::GetAABB() const
    {
        return SkeletalMesh ? SkeletalMesh->GetAABB() : SAABB();
    }
}
