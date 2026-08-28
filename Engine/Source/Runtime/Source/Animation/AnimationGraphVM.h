#pragma once

#include "Animation/AnimEvents.h"
#include "Animation/AnimSyncTrack.h"
#include "Animation/RootMotion.h"
#include "Animation/RootMotionTypes.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Serialization/Archiver.h"
#include "Renderer/SkeletonResource.h"
#include "AnimationGraphVM.generated.h"

namespace Lumina
{
    namespace Physics { class IPhysicsScene; }

    class CAnimationGraph;
    struct FSkeletonResource;
    struct FAnimMontagePlayer;

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
        LocalBone,
        ComponentSpace,
    };

    // Whether the BoneTransform op layers the (T, R, S) onto the bone's existing
    // transform or replaces it.
    REFLECT()
    enum class EBoneTransformMode : uint8
    {
        Add,
        Replace,
    };

    // Frame the MakeAdditive op measures its delta in.
    REFLECT()
    enum class EAdditiveSpace : uint8
    {
        LocalSpace,
        MeshSpace,
    };

    enum class EAnimOp : uint8
    {
        Halt = 0,
        LoadConst,       // imm:float, dst:sReg
        LoadParam,       // paramIdx:uint16, dst:sReg
        ScalarOp,        // op:uint8, a:sReg, b:sReg, dst:sReg
        AdvanceClock,    // stateIdx:uint16, speed:sReg, clipIdx:uint16, loopMode:sReg, startPos:sReg, seededSlot:uint16, dstClock:sReg, dstFinished:sReg, syncGroup:uint16
        SampleAnim,      // clipIdx:uint16, time:sReg, dst:pReg
        SampleBlendSpace,// bsIdx:uint16, x:sReg, y:sReg, speed:sReg, phaseSlot:uint16, startPos:sReg, seededSlot:uint16, smoothSlot:uint16, dst:pReg
        RefPose,         // dst:pReg
        Blend,           // a:pReg, b:pReg, alpha:sReg, dst:pReg
        BlendMasked,     // a:pReg, b:pReg, alpha:sReg, maskIdx:uint16, dst:pReg
        MakeAdditive,    // src:pReg, dst:pReg
        ApplyAdditive,   // base:pReg, delta:pReg, alpha:sReg, dst:pReg
        EvalStateMachine,// smIdx:uint16, dst:pReg
        BoneTransform,   // src:pReg, alpha:sReg, boneIdx:uint16, space:sReg, mode:sReg, T:vec3, R:quat, S:vec3, dst:pReg
        TwoBoneIK,       // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, rootIdx:uint16, midIdx:uint16, endIdx:uint16, pole:vec3, dst:pReg
        Output,          // src:pReg
        GetCurve,        // src:pReg, curveIdx:uint16, dst:sReg
        SetCurve,        // src:pReg, curveIdx:uint16, value:sReg, dst:pReg
        EvalSlot,        // slotIdx:uint16, src:pReg, dst:pReg

        //~ Object dataflow. Appended, so every opcode above keeps its value and existing bytecode stays valid.
        LoadObjectParam,        // paramIdx:uint16, dst:oReg
        LoadObjectConst,        // constIdx:uint16, dst:oReg
        SampleAnimDyn,          // clip:oReg, time:sReg, dst:pReg
        AdvanceClockDyn,        // stateIdx:uint16, speed:sReg, clip:oReg, loopMode:sReg, startPos:sReg, seededSlot:uint16, dstClock:sReg, dstFinished:sReg, syncGroup:uint16
        SampleBlendSpaceDyn,    // bs:oReg, x:sReg, y:sReg, speed:sReg, phaseSlot:uint16, startPos:sReg, seededSlot:uint16, smoothSlot:uint16, dst:pReg

        //~ MakeAdditive with an explicit base pose and space. Appended, so old programs stay valid.
        MakeAdditiveEx,         // src:pReg, base:pReg (kAnimNoPoseRegister = bind pose), space:uint8, dst:pReg

        //~ Seam smoothing a graph can ask for anywhere, not just at a state machine edge.
        Inertialize,            // src:pReg, request:sReg, duration:sReg, inertIdx:uint16, dst:pReg
        DeadBlend,              // src:pReg, request:sReg, duration:sReg, halfLife:sReg, deadIdx:uint16, dst:pReg

        //~ Exponential smoothing of a scalar, state carried in two slots (value + seeded flag).
        SmoothScalar,           // value:sReg, halfLife:sReg, valueSlot:uint16, seededSlot:uint16, dst:sReg

        //~ Named pose buffers that outlive the frame, for blending out of a pose captured earlier.
        SavePoseSnapshot,       // src:pReg, request:sReg, snapIdx:uint16, dst:pReg
        LoadPoseSnapshot,       // snapIdx:uint16, dst:pReg

        //~ Solvers whose targets are computed at runtime, so every operand is a register.
        FABRIK,                 // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, rootIdx:uint16, tipIdx:uint16, iterations:uint16, dst:pReg
        LookAt,                 // src:pReg, alpha:sReg, tx:sReg, ty:sReg, tz:sReg, boneIdx:uint16, forward:vec3, clamp:float, dst:pReg
        FootIK,                 // src:pReg, alpha:sReg, ox/oy/oz:sReg, nx/ny/nz:sReg, alignAlpha:sReg, thigh/calf/foot:uint16, up:vec3, dst:pReg
        TranslateBone,          // src:pReg, alpha:sReg, x:sReg, y:sReg, z:sReg, boneIdx:uint16, dst:pReg

        // Reshapes an alpha through an easing curve. Operands are easing uint8, value sReg, dst sReg.
        EaseAlpha,

        GroundTrace,            // footIdx:uint16, up:vec3, traceUp/traceDown/maxOffset/soleHeight:float, layerMask:uint16, gate:sReg, 9 dst:sReg (offset xyz, normal xyz, up xyz)

        // Signed angle from the forward axis to the entity's own velocity, measured about the turn axis.
        LoadMoveAngle,          // axis:vec3, forward:vec3, minSpeed:float, dst:sReg

        // Turns one bone about a component-space axis by a runtime angle, as a BoneTransform task.
        AxisRotateBone,         // src:pReg, alpha:sReg, angle:sReg, axis:vec3, boneIdx:uint16, dst:pReg
    };

    // MakeAdditiveEx base operand meaning "no base pose supplied".
    inline constexpr uint16 kAnimNoPoseRegister = 0xFFFFu;

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

    // Easing curve applied to a node's alpha before it blends. Append-only, the value rides in bytecode.
    REFLECT()
    enum class EAnimAlphaEasing : uint8
    {
        Linear,            // Passes the alpha through untouched, and emits no opcode at all.
        SmoothStep,        // Hermite, the gentle default when you just want the ends eased.
        QuadraticIn,
        QuadraticOut,
        QuadraticInOut,
        CubicIn,
        CubicOut,
        CubicInOut,
        QuarticIn,
        QuarticOut,
        QuarticInOut,
        QuinticIn,
        QuinticOut,
        QuinticInOut,
        SinusoidalIn,
        SinusoidalOut,
        SinusoidalInOut,
        ExponentialIn,
        ExponentialOut,
        ExponentialInOut,
        CircularIn,
        CircularOut,
        CircularInOut,
    };

    // Clamps to the unit range before shaping, so an unbounded input cannot overshoot a blend.
    RUNTIME_API float ApplyAlphaEasing(EAnimAlphaEasing Easing, float Alpha);

    // AdvanceClock operand value for "not in a sync group".
    inline constexpr uint16 kAnimNoSyncGroup = 0xFFFFu;
    
    inline constexpr uint16 kAnimBytecodeVersion = 7;
    
    struct FAnimSyncGroup
    {
        FSyncPosition Position;     // shared playhead on the group's blended sync track
        FSyncPosition PrevPosition; // position before this update's advance

        float Duration     = 0.0f; // blended seconds one cycle of the track takes
        float NextDuration = 0.0f; // accumulated from this update's blend provenance

        FSyncTrack Track;     // the track this update's clips map their playheads through
        FSyncTrack NextTrack; // accumulated from this update's blend provenance

        // Until a clip has run, the group holds the default track and behaves as a plain phase.
        bool bHasTrack = false;
        bool bAdvanced = false;
    };
    
    struct FAnimGraphVMState
    {
        TVector<float> ScalarRegisters;
        TVector<float> StateSlots;     // persistent playback clocks
        TVector<float> Parameters;

        // Current object parameter values, refilled each update from the entity's blackboard.
        TVector<TObjectPtr<CObject>> ObjectParameters;

        TVector<FAnimInertializer> Inertializers;     // per state machine; transition smoothing state
        TVector<FAnimInertializer> NodeInertializers; // per Inertialization node
        TVector<FAnimDeadBlend>    DeadBlends;        // per Dead Blending node
        TVector<FPose>             PoseSnapshots;     // per named snapshot slot
        TVector<FAnimSyncGroup> SyncGroups;       // shared phase per sync group

        // Curve values the output pose carried this update, indexed by CAnimationGraph::CurveNames.
        TVector<float> CurveValues;

        // Graph this state was sized against; the VM re-initializes the state
        // when the component's graph asset changes underneath it.
        const void* SourceGraph = nullptr;

        bool bInitialized = false;
    };

    // Per-entity scene state a GroundTrace queries; left invalid when there is no world, which traces flat.
    struct FAnimGraphSceneContext
    {
        Physics::IPhysicsScene* Scene = nullptr;

        // Last update's skinning matrices, since this update's pose is what the graph is still building.
        const TVector<FMatrix4>* BoneTransforms = nullptr;

        FVector3 WorldLocation = FVector3(0.0f);
        FVector3 WorldScale    = FVector3(1.0f);
        FQuat    WorldRotation = FQuat::Identity();

        // Ignored by every trace, so a foot never lands on the character it belongs to.
        uint32 SelfBodyID = ~0u;

        // World-space movement this frame, which is what LoadMoveAngle measures its angle against.
        FVector3 Velocity = FVector3(0.0f);

        bool IsValid() const { return Scene != nullptr && BoneTransforms != nullptr; }
    };

    struct FAnimGraphRootMotion
    {
        ERootMotionLockMode Mode = ERootMotionLockMode::FromAsset;
        int32 RootBoneIndex = INDEX_NONE;

        // Out: this frame's blended entity-space delta (FromAsset only).
        FRootMotionDelta Delta;
    };

    class RUNTIME_API FAnimationGraphVM
    {
    public:

        // Sizes register files / state slots / parameters from the graph; call when the graph asset changes.
        static void InitState(const CAnimationGraph* Graph, FAnimGraphVMState& State);

        static void BuildTasks(const CAnimationGraph* Graph,
                               FSkeletonResource* Skeleton,
                               float DeltaTime,
                               FAnimGraphVMState& State,
                               FAnimTaskList& OutTasks,
                               FAnimGraphRootMotion& RootMotionInOut,
                               TVector<FAnimNotifyEvent>* OutEvents = nullptr,
                               const FAnimMontagePlayer* Montages = nullptr,
                               const FAnimGraphSceneContext* SceneContext = nullptr);
    };
}
