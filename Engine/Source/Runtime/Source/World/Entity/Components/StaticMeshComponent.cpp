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
        if (Slot < static_cast<uint32>(MaterialOverrides.Num()))
        {
            if (CMaterialInterface* Interface = MaterialOverrides[Slot].Get())
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

    SAABB SStaticMeshComponent::GetAABB() const
    {
        return StaticMesh ? StaticMesh->GetAABB() : SAABB();
    }
}
