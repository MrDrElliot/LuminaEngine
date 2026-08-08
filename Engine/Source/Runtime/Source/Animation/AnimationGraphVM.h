#pragma once

#include "Animation/AnimEvents.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Serialization/Archiver.h"
#include "Renderer/SkeletonResource.h"
#include "AnimationGraphVM.generated.h"

namespace Lumina
{
    class CAnimationGraph;
    struct FSkeletonResource;

    // What AdvanceClock does when the playback clock reaches a clip's duration.
    REFLECT()
    enum class EClipLoopMode : uint8
    {
        Loop,
        PlayOnce,
    };

    // Frame the BoneTransform op interprets its (T, R, S) offsets in.
    REFLECT()
    enum class EBoneTransformSpace : uint8
    {
        // The bone's local frame (relative to its parent). Cheap; no FK walk.
        LocalBone,
        // The component (entity-root) space. The offset is applied to the bone's
        // global transform after FK; the result is converted back into local space.
        ComponentSpace,
    };

    // Whether the BoneTransform op layers the (T, R, S) onto the bone's existing
    // transform or replaces it.
    REFLECT()
    enum class EBoneTransformMode : uint8
    {
        // Additive: T offsets translation, R post-multiplies rotation, S
        // multiplies scale. Alpha scales how strongly the offset is applied.
        Add,
        // Override: lerp the bone's transform toward (T, R, S) by alpha.
        Replace,
    };

    enum class EAnimOp : uint8
    {
        Halt = 0,
        LoadConst,       // imm:float, dst:sReg
        LoadParam,       // paramIdx:uint16, dst:sReg
        ScalarOp,        // op:uint8, a:sReg, b:sReg, dst:sReg
        AdvanceClock,    // stateIdx:uint16, speed:sReg, clipIdx:uint16, loopMode:sReg, dstClock:sReg, dstFinished:sReg, syncGroup:uint16
        SampleAnim,      // clipIdx:uint16, time:sReg, dst:pReg
        SampleBlendSpace,// bsIdx:uint16, x:sReg, y:sReg, phase:sReg, dst:pReg
        RefPose,         // dst:pReg
        Blend,           // a:pReg, b:pReg, alpha:sReg, dst:pReg
        BlendMasked,     // a:pReg, b:pReg, alpha:sReg, maskIdx:uint16, dst:pReg
        MakeAdditive,    // src:pReg, dst:pReg
        ApplyAdditive,   // base:pReg, delta:pReg, alpha:sReg, dst:pReg
        EvalStateMachine,// smIdx:uint16, dst:pReg
        BoneTransform,   // src:pReg, alpha:sReg, boneIdx:uint16, space:sReg, mode:sReg, T:vec3, R:quat, S:vec3, dst:pReg
        TwoBoneIK,       // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, rootIdx:uint16, midIdx:uint16, endIdx:uint16, pole:vec3, dst:pReg
        Output,          // src:pReg
    };

    // Append-only: the enum value is baked into compiled bytecode.
    REFLECT()
    enum class EAnimScalarOp : uint8
    {
        Add,
        Sub,
        Mul,
        Div,
        Min,
        Max,
        // Unary (ignore operand B).
        Clamp01,
        OneMinus,    // 1 - A
        Abs,         // |A|
        Sin,         // sin(A)
        Cos,         // cos(A)
        // Binary.
        Mod,         // fmod(A, B), 0 when B == 0
        Pow,         // A ^ B
        Atan2,       // atan2(A, B)
        Less,        // A < B ? 1 : 0
        Greater,     // A > B ? 1 : 0
        // Unary (ignore operand B).
        Floor,
        Ceil,
        Frac,        // A - floor(A)
        Sqrt,        // sqrt(max(A, 0))
        Negate,      // -A
        Sign,        // -1 / 0 / 1
    };

    // AdvanceClock operand value for "not in a sync group".
    inline constexpr uint16 kAnimNoSyncGroup = 0xFFFFu;

    // Stamped into CAnimationGraph by the compiler and checked by BuildTasks. Bump whenever an
    // opcode's operand layout changes: a stale program would misparse operands into garbage poses,
    // so the VM refuses it (bind pose + warning) until the graph is recompiled in the editor.
    // 0 = compiled before versioning existed (pre-sync-group layout). 2 = AdvanceClock syncGroup operand.
    // 3 = SampleBlendSpace opcode inserted after SampleAnim, which renumbers every opcode past it.
    inline constexpr uint16 kAnimBytecodeVersion = 3;

    // Clips in a sync group advance one shared normalized phase instead of independent clocks, so a
    // walk->run blend samples both clips at the same stride phase (no foot slide). The phase speed
    // uses the group's blended duration, refined from blend alphas one frame behind (weights change
    // smoothly, so the latency is invisible). Requires member clips to be phase-aligned by authoring
    // (all starting on the same foot plant); synced clips always loop.
    struct FAnimSyncGroup
    {
        float Phase        = 0.0f; // shared normalized playhead, wraps 0..1
        float PrevPhase    = 0.0f; // phase before this update's advance
        float Duration     = 0.0f; // blended seconds driving the phase speed
        float NextDuration = 0.0f; // accumulated from this update's blend provenance
        bool  bAdvanced    = false;
    };

    // Per-instance mutable execution state. Persists across frames so playback
    // clocks keep advancing; owned by SAnimationGraphComponent. Pose data no longer
    // lives here -- poses come from the executor's per-thread buffer pool.
    struct FAnimGraphVMState
    {
        TVector<float> ScalarRegisters;
        TVector<float> StateSlots;     // persistent playback clocks
        TVector<float> Parameters;     // current parameter values (editor / Lua driven)
        TVector<FAnimInertializer> Inertializers; // per state machine; transition smoothing state
        TVector<FAnimSyncGroup> SyncGroups;       // shared phase per sync group

        // Graph this state was sized against; the VM re-initializes the state
        // when the component's graph asset changes underneath it.
        const void* SourceGraph = nullptr;

        bool bInitialized = false;
    };

    // Root-motion policy in, blended delta out. FromAsset extracts per-clip deltas (clips with
    // bEnableRootMotion) and blends them through the graph like the pose; the root is pinned iff the
    // branch reaching the output is root-motion driven. ForceLock pins without extraction;
    // ForceUnlock leaves the root free in the pose.
    struct FAnimGraphRootMotion
    {
        ERootMotionLockMode Mode = ERootMotionLockMode::FromAsset;
        int32 RootBoneIndex = INDEX_NONE;

        // Out: this frame's blended entity-space delta (FromAsset only).
        FRootMotionDelta Delta;
    };

    // Stateless graph-update pass. Interprets the bytecode's logic (scalars, clocks, state-machine
    // transitions) immediately and records every pose operation into an FAnimTaskList for deferred
    // execution by Anim::ExecuteTaskList. Cheap: no pose math runs here.
    class RUNTIME_API FAnimationGraphVM
    {
    public:

        // Sizes register files / state slots / parameters from the graph; call when the graph asset changes.
        static void InitState(const CAnimationGraph* Graph, FAnimGraphVMState& State);

        // Runs one frame's graph logic and records the frame's pose recipe. Always leaves a valid
        // output task (bind pose when the graph produced none). Root-motion deltas and notify events
        // flow alongside the pose registers: blends weight them, inactive state-machine branches drop
        // them. OutEvents (optional) receives the surviving events, weighted by their branch's blend.
        static void BuildTasks(const CAnimationGraph* Graph,
                               FSkeletonResource* Skeleton,
                               float DeltaTime,
                               FAnimGraphVMState& State,
                               FAnimTaskList& OutTasks,
                               FAnimGraphRootMotion& RootMotionInOut,
                               TVector<FAnimNotifyEvent>* OutEvents = nullptr);
    };
}
