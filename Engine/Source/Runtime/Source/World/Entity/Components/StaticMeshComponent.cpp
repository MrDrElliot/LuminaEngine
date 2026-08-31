#include "RuntimePCH.h"
#include "StaticMeshComponent.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"



namespace Lumina
{
    CMaterialInstance* SStaticMeshComponent::CreateDynamicMaterialInstance(uint32 Slot)
    {
        CMaterialInstance* Dynamic = MeshComponentUtils::MakeDynamicMaterialInstance(GetMaterialForSlot(Slot));
        if (Dynamic != nullptr)
        {
            SetMaterialAtSlot(Dynamic, Slot);
        }
        return Dynamic;
    }

    CMaterialInterface* SStaticMeshComponent::GetMaterialForSlot(uint32 Slot) const
    {
        if (Slot < MaterialOverrides.size())
        {
            if (CMaterialInterface* Interface = MaterialOverrides[Slot])
            {
                return Interface;
            }
        }
        
        if (StaticMesh.IsValid())
        {
            return StaticMesh->GetMaterialAtSlot(Slot);
        }

        return nullptr;
    }

    FAABB SStaticMeshComponent::GetAABB() const
    {
        return StaticMesh ? StaticMesh->GetAABB() : FAABB();
    }
}
