#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "WorldRenderSettings.generated.h"

namespace Lumina
{
    // Saved with the world on its singleton entity, so the editor's rendering toggles come back with the level.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SWorldRenderSettings
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Culling")
        bool bFrustumCull = true;

        PROPERTY(Editable, Category = "Culling")
        bool bConeCull = true;

        PROPERTY(Editable, Category = "Culling")
        bool bOcclusionCull = true;

        // Per meshlet Hi-Z across the two VisBuffer phases, and off collapses the frame to a single geometry phase.
        PROPERTY(Editable, Category = "Culling")
        bool bMeshletOcclusionCull = true;

        PROPERTY(Editable, Category = "Culling")
        bool bCPUInstanceCull = true;

        // Rejects a local light whose attenuation sphere reaches no shading volume, before it takes a slot.
        PROPERTY(Editable, Category = "Culling")
        bool bCullLights = true;

        PROPERTY(Editable, Category = "Culling")
        bool bUseLODs = true;

        PROPERTY(Editable)
        bool bDrawBillboards = true;

        //~ Debug views, left out of the save so a level never reopens in one.

        ERenderSceneDebugFlags Flags = ERenderSceneDebugFlags::None;

        bool bWireframe = false;

        // Culls against the inputs captured when this went on, so the selected set holds still while the camera flies free.
        bool bFreezeCulling = false;

        // The editor's game view, which hides billboards without touching the saved toggle.
        bool bHideEditorOverlays = false;

        // Builds a missing pipeline on the spot instead of drawing the default material until it is ready, for one-shot captures such as thumbnails.
        bool bWaitForPipelines = false;

        // An offline capture such as an impostor bake, which needs the exact surface, so it draws without jitter, proxies or impostors.
        bool bExactCapture = false;

        bool ShouldDrawBillboards() const { return bDrawBillboards && !bHideEditorOverlays; }
    };
}
