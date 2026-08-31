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
        
        FUNCTION()
        CMaterialInterface* GetMaterialForSlot(uint32 Slot) const;

        /** Installs a transient instance over this slot so its parameters diverge for this component only. */
        FUNCTION()
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
