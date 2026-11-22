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

    // Resolved active view, written by CCameraSystem at end of update and forwarded by CWorld::Extract.
    struct FResolvedSceneView : ECS::FResource
    {
        FViewVolume                     ViewVolume;
        SPostProcessSettings            PostProcess;
        TVector<CMaterialInterface*>    PostProcessMaterials;
        FViewLayerSettings              Layers;
        float                           MouseLookDegreesPerCount = 0.0f;
        bool                            bHasView = false;
        bool                            bHasPostProcess = false;
    };

    // One live additive camera shake applied to the rendered view, so shakes sum and never move the camera entity.
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

    // Active-camera selection and cinematic blend, where the resolved view is stored back each frame as the next blend source.
    REFLECT()
    struct RUNTIME_API SCameraGlobalState : ECS::FResource
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
