#include "RuntimePCH.h"
#include "SkeletalMeshComponent.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"


namespace Lumina
{
    CMaterialInterface* SSkeletalMeshComponent::GetMaterialForSlot(size_t Slot) const
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
