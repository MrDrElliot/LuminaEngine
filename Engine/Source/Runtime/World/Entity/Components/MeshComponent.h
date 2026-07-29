#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Math/AABB.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/CustomPrimitiveData.h"
#include "Containers/Array.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "MeshComponent.generated.h"

namespace Lumina
{
    class CMaterialInterface;
    struct FPropertyChangedEvent;

    REFLECT()
    struct RUNTIME_API SMeshComponent
    {
        GENERATED_BODY()

        // Everything the cull-reject path reads is grouped first so it lands in the component's first cache
        // line; a culled entity then never touches the second one. Keep new fields out of this block.

        /** Mesh-local bounding sphere, cached at resolve so culling never touches the mesh asset. */
        FVector3        CachedLocalCenter;
        float           CachedLocalRadius = 0.0f;

        /** Scale applied to the mesh bounds used for occlusion and distance culling. */
        PROPERTY(Editable, Category = "Culling")
        float BoundsScale = 1.0f;

        /** Beyond this distance (meters) the mesh is culled (0 = never culled by distance). */
        PROPERTY(Editable, Category = "Culling")
        float MaxDrawDistance = 0.0f;

        /** Index into FMeshResolveCache; ~0u until first resolved. */
        uint32          ResolveHandle = ~0u;

        /** When true, this mesh writes to the shadow map. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadow = true;

        /** When true, this mesh samples and applies shadowing from shadow-casting lights. */
        PROPERTY(Editable, Category = "Shadows")
        bool bReceiveShadow = true;

        /** When true, this mesh blocks visibility for occluded objects behind it. */
        PROPERTY(Editable, Category = "Culling")
        bool bUseAsOccluder = true;

        /** When true, occlusion culling is skipped for this mesh entirely. */
        PROPERTY(Editable, Category = "Culling")
        bool bIgnoreOcclusionCulling = false;

        uint64          CachedMeshletHeaderAddress = 0;
        EInstanceFlags  CachedBaseFlags = EInstanceFlags::None;

        // LOD override. -1 = automatic (distance/radius); >= 0 pins that LOD. Clamped to the surface's
        // NumLODs, so out-of-range values safely degrade to the coarsest LOD.
        PROPERTY(Editable, Category = "Rendering")
        int32 ForcedLODIndex = -1;
        
        /** Per-slot material overrides applied on top of the mesh's default materials. */
        PROPERTY(Editable, Category = "Rendering")
        TVector<TObjectPtr<CMaterialInterface>> MaterialOverrides;

        /** Per-instance shader data packed into a single primitive data slot. */
        PROPERTY(Editable, Category = "Rendering")
        SCustomPrimitiveData CustomPrimitiveData{};

        // Compared against the live mesh field so a direct assignment self-heals after one frame.
        const void*     CachedMeshKey = nullptr;

        // 0 never matches a live epoch, so a fresh component always resolves.
        uint32  CachedEpoch = 0;

        FUNCTION(Script)
        void SetMaterialAtSlot(CMaterialInterface* Material, uint32 Slot)
        {
            if (MaterialOverrides.size() < Slot)
            {
                MaterialOverrides.push_back(Material);
            }
            else
            {
                MaterialOverrides[Slot] = Material;
            }
            InvalidateRenderResolve();
        }

        void InvalidateRenderResolve();

        #if USING(WITH_EDITOR)
        void PostEditChange(const FPropertyChangedEvent& Event) { InvalidateRenderResolve(); }
        #endif
    };
}
