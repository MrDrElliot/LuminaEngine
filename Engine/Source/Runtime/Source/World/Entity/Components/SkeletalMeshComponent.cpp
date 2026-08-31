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
        if (Slot < MaterialOverrides.size())
        {
            if (CMaterialInterface* Interface = MaterialOverrides[Slot])
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

    FAABB SSkeletalMeshComponent::GetAABB() const
    {
        return SkeletalMesh ? SkeletalMesh->GetAABB() : FAABB();
    }
}
