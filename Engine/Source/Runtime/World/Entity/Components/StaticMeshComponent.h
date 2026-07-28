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
        
        FUNCTION(Script)
        FAABB GetAABB() const;
        
        FUNCTION(Script)
        void SetStaticMesh(CStaticMesh* InMesh) { StaticMesh = InMesh; InvalidateRenderResolve(); }
        
        FUNCTION(Script)
        CStaticMesh* GetStaticMesh() const { return StaticMesh; }
        
        /** The static mesh asset to render for this component. */
        PROPERTY(Editable, Replicated, Category = "Rendering")
        TObjectPtr<CStaticMesh> StaticMesh;
    };
}
