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

        /**
         * Staleness token of the resolve entry this component last copied from
         * (FMeshResolveCache::GetEntryState).
         *
         * This used to be a stamp of the cache's GLOBAL epoch, which made every invalidation scene-wide:
         * one mesh finishing its GPU upload bumped the epoch, so every mesh component in the world failed
         * its gate, re-resolved, and got marked dirty -- and the render scene then re-bound and re-uploaded
         * every primitive. Comparing the entry's own token instead means the components that re-resolve are
         * exactly the ones drawing the mesh that changed.
         *
         * MESH_RESOLVE_STATE_STALE never matches a live entry, so a fresh component always resolves.
         */
        uint32  CachedEntryState = MESH_RESOLVE_STATE_STALE;

        // Hash of the override list this component last RESOLVED with. Compared against the live array
        // every frame, so a material change is detected from its effect rather than from a notification.
        // Belt and braces on purpose: the editor's PostEditChange, SetMaterialAtSlot and MarkRenderStateDirty
        // all signal correctly, but a direct write from C++ or C# signals nothing, and a retained renderer
        // bakes the material into a GPU record that is only rewritten when something reports a change.
        // 0 is the never-resolved sentinel.
        uint32  CachedMaterialHash = 0;

        // FMeshResolveCache::GetEpoch() at the last resolve. The epoch is the editor's "nuke every resolve"
        // signal (AssetEditorTool bumps it on any mesh/material edit). Components that own a cache entry get
        // it for free -- ApplyPendingInvalidations marks every entry stale -- but a component resolving
        // OUTSIDE the cache has no entry to mark, so it compares this itself. 0 never matches a live epoch.
        uint32  CachedResolveEpoch = 0;

        FUNCTION(Script)
        void SetMaterialAtSlot(CMaterialInterface* Material, uint32 Slot)
        {
            // Grow to cover Slot, then assign. The previous form tested `size() < Slot` and otherwise
            // indexed directly, so the common case of slot 0 on a component with no overrides yet
            // (0 < 0 is false) wrote through MaterialOverrides[0] on an EMPTY vector. Nothing sizes
            // this array -- not SetStaticMesh, not construction -- so that was an out-of-bounds write
            // on the first call for any component. push_back was also wrong for Slot > size, landing
            // the material at whatever index happened to be next rather than the one asked for.
            if (MaterialOverrides.size() <= Slot)
            {
                MaterialOverrides.resize(Slot + 1);
            }

            MaterialOverrides[Slot] = Material;
            InvalidateRenderResolve();
        }

        /** Tells the renderer this component's render state changed and has to be re-read.
         *
         *  Call this after writing ANY render-facing property directly rather than through a setter --
         *  ForcedLODIndex, CustomPrimitiveData, MaterialOverrides, bCastShadow, MaxDrawDistance. The
         *  renderer keeps a RETAINED per-primitive GPU record and only rewrites it for primitives that
         *  report a change, so a direct write with no mark is simply never picked up. It used to be free
         *  because the old renderer re-read every component every frame; it is not any more.
         *
         *  Editor property edits route here through PostEditChange, so this is only needed from gameplay
         *  code (C++ or C#).
         */
        FUNCTION(Script)
        void MarkRenderStateDirty() { InvalidateRenderResolve(); }

        void InvalidateRenderResolve();

        #if USING(WITH_EDITOR)
        void PostEditChange(const FPropertyChangedEvent& Event) { InvalidateRenderResolve(); }
        #endif
    };
}
