#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Math/AABB.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/CustomPrimitiveData.h"
#include "Renderer/RenderLayers.h"
#include "Containers/Vector.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Lumina.h"
#include "MeshComponent.generated.h"

namespace Lumina
{
    class CMaterialInterface;
    class CMaterialInstance;
    struct FPropertyChangedEvent;

    namespace MeshComponentUtils
    {
        /** Current itself when it is already a transient instance, else a new one parented to it. */
        RUNTIME_API CMaterialInstance* MakeDynamicMaterialInstance(CMaterialInterface* Current);
    }

    REFLECT(OnScriptWrite = MarkRenderStateDirty)
    struct RUNTIME_API SMeshComponent
    {
        GENERATED_BODY()

        // Grouped first so a culled entity never touches the second cache line. Keep new fields out of this block.

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
        uint32          ResolveHandle = Constants::kIndexNoneU32;

        /** When true, this mesh writes to the shadow map. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadow = true;

        // Also casts into the sun's far cascade. Meant for large landmarks seen from far away.
        PROPERTY(Editable, Category = "Shadows")
        bool bCastFarShadow = false;

        /** When true, this mesh samples and applies shadowing from shadow-casting lights. */
        PROPERTY(Editable, Category = "Shadows")
        bool bReceiveShadow = true;

        // Off for anything that moves through the world, so a scorch mark does not paint the vehicle over it.
        PROPERTY(Editable, Category = "Rendering")
        bool bReceiveDecals = true;

        /** When true, this mesh blocks visibility for occluded objects behind it. */
        PROPERTY(Editable, Category = "Culling")
        bool bUseAsOccluder = true;

        /** When true, occlusion culling is skipped for this mesh entirely. */
        PROPERTY(Editable, Category = "Culling")
        bool bIgnoreOcclusionCulling = false;

        uint32          CachedMeshletHeaderSlot = 0;
        EInstanceFlags  CachedBaseFlags = EInstanceFlags::None;

        // -1 picks the LOD by distance; anything else pins it, clamped to the surface's coarsest LOD.
        PROPERTY(Editable, Category = "Rendering")
        int32 ForcedLODIndex = -1;
        
        /** Per-slot material overrides applied on top of the mesh's default materials. */
        PROPERTY(Editable, Category = "Rendering")
        TVector<TStrongObjectPtr<CMaterialInterface>> MaterialOverrides;

        /** Per-instance shader data packed into a single primitive data slot. */
        PROPERTY(Editable, Category = "Rendering")
        SCustomPrimitiveData CustomPrimitiveData{};

        // Drawn by any camera whose CullMask shares one of these layers.
        PROPERTY(Editable, Category = "Rendering")
        SRenderLayerMask RenderLayers;

        // Also drawn into the custom depth and stencil buffers, which post process materials read for outlines and x-ray effects.
        PROPERTY(Editable, Category = "Rendering|Custom Depth")
        bool bRenderCustomDepth = false;

        // Written into the custom stencil buffer wherever this mesh is the nearest custom depth surface.
        PROPERTY(Editable, Category = "Rendering|Custom Depth", EditCondition = "bRenderCustomDepth")
        uint8 CustomDepthStencilValue = 0;

        // Compared against the live mesh field, so a direct assignment is noticed whenever this component is next resolved.
        const void*     CachedMeshKey = nullptr;

        // True while the cached fields still describe the last mesh that could draw, kept on screen until the new one is resident.
        bool            bShowingPreviousMesh = false;

        // Staleness token of the resolve entry this component last copied from (FMeshResolveCache::GetEntryState).
        uint32  CachedEntryState = MESH_RESOLVE_STATE_STALE;

        // Hash of the override list this component last resolved with.
        uint32  CachedMaterialHash = 0;

        // FMeshResolveCache::GetEpoch() at the last resolve.
        uint32  CachedResolveEpoch = 0;

        mutable FMeshResolveBinding ResolveBinding;

        FUNCTION()
        void SetMaterialAtSlot(CMaterialInterface* Material, uint32 Slot)
        {
            if (static_cast<uint32>(MaterialOverrides.Num()) <= Slot)
            {
                MaterialOverrides.SetNum(Slot + 1);
            }

            MaterialOverrides[Slot] = Material;
            InvalidateRenderResolve();
        }

        // The retained scene never sees a raw C++ field write, so C++ calls this after one; editor and C# writes call it themselves.
        FUNCTION()
        void MarkRenderStateDirty() { InvalidateRenderResolve(); }

        // Any thread. Stales this component's resolve and queues its entity for the next resolve pass.
        void InvalidateRenderResolve();

        // Any thread. Queues the entity without touching the cached resolve, for a reader that holds the component const.
        void QueueRenderResolve() const { ResolveBinding.Enqueue(); }

        #if USING(WITH_EDITOR)
        void PostEditChange(const FPropertyChangedEvent& Event) { InvalidateRenderResolve(); }
        #endif
    };
}
