#pragma once

#include "World/ECS/Registry.h"
#include "World/Entity/Systems/SystemResources.h"

#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Renderer/RenderLayers.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/PostProcessSettings.h"
#include "SystemSingletons.generated.h"

namespace Lumina
{
    class CMaterialInterface;
    enum class ECameraBlendFunction : uint8;

    // Registry-context singletons holding per-world state systems produce and consume; transient and never serialized.

    // Resolved active view. CCameraSystem writes it at end of update (FrameEnd, or Paused in editor);
    // CWorld::Extract reads + forwards it, keeping camera/post-process resolution in a system.
    struct FResolvedSceneView : ECS::FResource
    {
        FViewVolume                     ViewVolume;
        SPostProcessSettings            PostProcess;
        TVector<CMaterialInterface*>    PostProcessMaterials;
        FViewLayerSettings              Layers;
        bool                            bHasView = false;
        bool                            bHasPostProcess = false;
    };

    // One live additive camera shake. Applied to the RENDERED view each frame (not the camera entity
    // transform), so several shakes sum and none permanently move the camera. Oscillation is sine-per-axis
    // with seeded phase/frequency variation so axes don't move in lockstep.
    struct FCameraShakeInstance
    {
        FVector3    LocationAmplitude = FVector3(0.0f);  // max local-space offset per axis (world units)
        FVector3    RotationAmplitude = FVector3(0.0f);  // max rotation per axis, degrees (X pitch, Y yaw, Z roll)
        float       Frequency         = 10.0f;           // oscillation rate (Hz)
        float       Duration          = 0.5f;            // seconds; <= 0 loops until stopped
        float       BlendInTime       = 0.05f;
        float       BlendOutTime      = 0.2f;
        float       Elapsed           = 0.0f;
        uint32      Handle            = 0;
        float       LocPhase[3]       = { 0.0f, 0.0f, 0.0f };
        float       RotPhase[3]       = { 0.0f, 0.0f, 0.0f };
        float       LocFreqMul[3]     = { 1.0f, 1.0f, 1.0f };
        float       RotFreqMul[3]     = { 1.0f, 1.0f, 1.0f };
    };

    // Active-camera selection + cinematic blend, owned and ticked entirely by CCameraSystem. A non-zero
    // blend snapshots the displayed view and eases to the new camera over Duration; the resolved view is
    // stored back here each frame as the source for the next blend.
    REFLECT()
    struct RUNTIME_API FCameraGlobalState : ECS::FResource
    {
        GENERATED_BODY()

        struct FBlendState
        {
            bool                    bActive = false;
            float                   Elapsed = 0.0f;
            float                   Duration = 0.0f;
            ECameraBlendFunction    Function = (ECameraBlendFunction)3; // EaseInOut

            // Snapshot of the displayed view when the blend began.
            FVector3                FromPosition = FVector3(0.0f);
            FQuat                   FromRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            float                   FromFOV = 90.0f;
            SPostProcessSettings    FromPostProcess;
        };

        PROPERTY(Hidden)
        ECS::FEntity            ActiveCameraEntity = ECS::NullEntity;

        FBlendState             Blend;

        // Live additive shakes summed into the rendered view each frame; handles let callers stop them.
        TVector<FCameraShakeInstance>   Shakes;
        uint32                          NextShakeHandle = 1;

        // Last fully resolved (post-blend) view; the source for the next blend's snapshot.
        PROPERTY(Hidden)
        FVector3                LastViewPosition = FVector3(0.0f);

        FQuat                   LastViewRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);

        PROPERTY(Hidden)
        float                   LastViewFOV = 90.0f;

        SPostProcessSettings    LastPostProcess;

        PROPERTY(Hidden)
        bool                    bHasResolvedView = false;

        // Laid over whichever camera is live, so a sequence's fade and bars hold across its cuts.
        PROPERTY(Hidden)
        float                   OverlayFade = 0.0f;

        PROPERTY(Hidden)
        FVector3                OverlayFadeColor = FVector3(0.0f);

        PROPERTY(Hidden)
        float                   OverlayLetterbox = 0.0f;
    };
}
