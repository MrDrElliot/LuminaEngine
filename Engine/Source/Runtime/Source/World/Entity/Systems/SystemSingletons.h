#pragma once
#include <entt/entt.hpp>
#include "Containers/Array.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/PostProcessSettings.h"

namespace Lumina
{
    class CMaterialInterface;
    enum class ECameraBlendFunction : uint8;

    // Registry-context singletons holding per-world state systems produce/consume. Plain (non-reflected),
    // transient, never serialized. Emplaced in CWorld::InitializeWorld, read in Extract / the systems.

    // Resolved active view. SCameraSystem writes it at end of update (FrameEnd, or Paused in editor);
    // CWorld::Extract reads + forwards it, keeping camera/post-process resolution in a system.
    struct FResolvedSceneView
    {
        FViewVolume                     ViewVolume;
        SPostProcessSettings            PostProcess;
        TVector<CMaterialInterface*>    PostProcessMaterials;
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

    // Active-camera selection + cinematic blend, owned and ticked entirely by SCameraSystem. A non-zero
    // blend snapshots the displayed view and eases to the new camera over Duration; the resolved view is
    // stored back here each frame as the source for the next blend.
    struct FCameraGlobalState
    {
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

        entt::entity            ActiveCameraEntity = entt::null;
        FBlendState             Blend;

        // Live additive shakes summed into the rendered view each frame; handles let callers stop them.
        TVector<FCameraShakeInstance>   Shakes;
        uint32                          NextShakeHandle = 1;

        // Last fully resolved (post-blend) view; the source for the next blend's snapshot.
        FVector3                LastViewPosition = FVector3(0.0f);
        FQuat                   LastViewRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        float                   LastViewFOV = 90.0f;
        SPostProcessSettings    LastPostProcess;
        bool                    bHasResolvedView = false;
    };

    // Game-thread accumulator driving C# EntityScript OnFixedUpdate at the physics fixed rate (1/PhysicsHz).
    // Owned by SCSharpScriptSystem (PrePhysics pass); independent of the physics scene's own accumulator but
    // uses the same Hz/cap, so it runs the same number of fixed steps per frame.
    struct FScriptFixedUpdateState
    {
        float Accumulator = 0.0f;
    };
}
