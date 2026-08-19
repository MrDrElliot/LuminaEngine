#pragma once


#include "MeshComponent.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "StaticMeshComponent.generated.h"

namespace Lumina
{
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API CACHE_ALIGN SStaticMeshComponent : SMeshComponent
    {
        GENERATED_BODY()
        
        CMaterialInterface* GetMaterialForSlot(size_t Slot) const;

        /** Installs a transient instance over this slot so its parameters diverge for THIS component only. */
        CMaterialInstance* CreateDynamicMaterialInstance(uint32 Slot);

        FUNCTION()
        FAABB GetAABB() const;
        
        FUNCTION()
        void SetStaticMesh(CStaticMesh* InMesh) { StaticMesh = InMesh; InvalidateRenderResolve(); }
        
        FUNCTION()
        CStaticMesh* GetStaticMesh() const { return StaticMesh; }
        
        /** The static mesh asset to render for this component. */
        PROPERTY(Editable, Replicated, Category = "Rendering")
        TObjectPtr<CStaticMesh> StaticMesh;
    };
}
