#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationGraph.generated.h"

namespace Lumina
{
    class CSkeleton;
    class CAnimation;
    class CBlackboard;
    class CBlendSpace;

    REFLECT()
    enum class EAnimGraphParamType : uint8
    {
        Float,
        Bool,
    };

    // Transition-condition comparison: the named parameter's value vs the transition's CompareValue.
    REFLECT()
    enum class EAnimTransitionCompare : uint8
    {
        Greater,
        GreaterEqual,
        Less,
        LessEqual,
        Equal,
        NotEqual,
    };

    // A named value editor/Lua tweak at runtime to drive blend weights, playback speeds, etc.
    struct FAnimGraphParameter
    {
        FName Name;
        EAnimGraphParamType Type = EAnimGraphParamType::Float;
        float DefaultValue = 0.0f;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphParameter& Data)
        {
            Ar << Data.Name;
            Ar << Data.Type;
            Ar << Data.DefaultValue;
            return Ar;
        }
    };

    // One compiled state-machine edge; the VM cross-fades FromState->ToState over BlendDuration
    // when the condition passes while FromState is active.
    struct FAnimGraphTransition
    {
        // -1 means "any state" (checked regardless of active state).
        int32 FromState = -1;
        int32 ToState = 0;

        // Gating parameter; an undeclared parameter evaluates as 0.
        FName ConditionParameter;
        EAnimTransitionCompare Compare = EAnimTransitionCompare::Greater;
        float CompareValue = 0.0f;

        // Cross-fade length in seconds; 0 snaps instantly.
        float BlendDuration = 0.2f;

        // Re-evaluate the condition mid-cross-fade to pre-empt in flight; default off (runs to completion).
        bool bCanInterrupt = false;

        // ConditionParameter resolved to a Parameters index (INDEX_NONE = undeclared) so the VM skips
        // the per-frame name lookup. Transient: filled by ResolveTransitionParameters, not serialized.
        static constexpr int32 ParamUnresolved = -2;
        int32 CachedParamIndex = ParamUnresolved;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphTransition& Data)
        {
            Ar << Data.FromState;
            Ar << Data.ToState;
            Ar << Data.ConditionParameter;
            Ar << Data.Compare;
            Ar << Data.CompareValue;
            Ar << Data.BlendDuration;
            Ar << Data.bCanInterrupt;
            return Ar;
        }
    };

    // One bone of a bone mask; resolved to a skeleton index and baked into FAnimGraphBoneMask::Weights at compile.
    REFLECT()
    struct FAnimGraphBoneMaskBone
    {
        GENERATED_BODY()

        /** Name of the bone to weight. Must exist in the graph's skeleton. */
        PROPERTY(Editable, Category = "Bone Mask", BonePicker)
        FName BoneName;

        /** Blend weight applied to this bone (0 = base, 1 = overlay). */
        PROPERTY(Editable, Category = "Bone Mask", ClampMin = 0.0f, ClampMax = 1.0f)
        float Weight = 1.0f;
    };

    // Editor-authored bone mask (named (bone, weight) list); the compiler resolves it into a runtime FAnimGraphBoneMask.
    REFLECT()
    struct FAnimGraphBoneMaskDef
    {
        GENERATED_BODY()

        /** Identifier referenced by Layered Blend Per Bone nodes. */
        PROPERTY(Editable, Category = "Bone Mask")
        FName Name;

        /** Per-bone weight entries; unlisted bones default to zero. */
        PROPERTY(Editable, Category = "Bone Mask")
        TVector<FAnimGraphBoneMaskBone> Bones;
    };

    // Compiled bone mask: dense per-bone weight array (skeleton bone count); the VM's BlendMasked op indexes it directly.
    struct FAnimGraphBoneMask
    {
        TVector<float> Weights;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphBoneMask& Data)
        {
            Ar << Data.Weights;
            return Ar;
        }
    };

    // Which graph curve slot each of a clip's authored curves feeds (INDEX_NONE = not referenced).
    // Resolved at compile so sampling a clip writes its curves by index, never by name.
    struct FAnimGraphClipCurveMap
    {
        TVector<int32> Slots;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphClipCurveMap& Data)
        {
            Ar << Data.Slots;
            return Ar;
        }
    };

    // One clip curve map per blend-space sample, in sample order.
    struct FAnimGraphBlendSpaceCurveMap
    {
        TVector<FAnimGraphClipCurveMap> SampleMaps;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphBlendSpaceCurveMap& Data)
        {
            Ar << Data.SampleMaps;
            return Ar;
        }
    };

    // Compiled state machine; each state owns a pose register. Per-frame bookkeeping (active state,
    // timer) lives in per-instance FAnimGraphVMState.StateSlots, addressed by the *Slot indices below.
    struct FAnimGraphStateMachine
    {
        // State entered on first init.
        int32 EntryState = 0;

        // One pose register per state, in state-index order.
        TVector<uint16> StatePoseRegisters;

        // Outgoing edges, checked in list order; first passing wins.
        TVector<FAnimGraphTransition> Transitions;

        // Slots into FAnimGraphVMState.StateSlots: Current/From state indices (From -1 when not
        // transitioning), Timer elapsed, Duration of the active transition.
        uint16 CurrentStateSlot = 0;
        uint16 FromStateSlot = 0;
        uint16 TimerSlot = 0;
        uint16 DurationSlot = 0;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphStateMachine& Data)
        {
            Ar << Data.EntryState;
            Ar << Data.StatePoseRegisters;
            Ar << Data.Transitions;
            Ar << Data.CurrentStateSlot;
            Ar << Data.FromStateSlot;
            Ar << Data.TimerSlot;
            Ar << Data.DurationSlot;
            return Ar;
        }
    };

    // Runtime anim-graph asset: compiled bytecode + referenced resources/metadata. Editor compiles a
    // CAnimationGraphNodeGraph into this; SAnimationSystem runs it each frame via FAnimationGraphVM.
    REFLECT()
    class RUNTIME_API CAnimationGraph : public CObject
    {
        GENERATED_BODY()

        friend class CAnimationGraphNodeGraph;

    public:

        using Super::Serialize;
        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }

        int32 FindParameterIndex(const FName& Name) const;

        int32 FindCurveIndex(const FName& Name) const;

        // Fills every transition's CachedParamIndex; call after Parameters/StateMachines change.
        void ResolveTransitionParameters();

        bool IsCompiled() const { return !Bytecode.empty(); }

        /** Skeleton every pose produced by this graph is authored against. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CSkeleton> Skeleton;

        /** Blackboard schema graph parameters are picked from; runtime values come from SBlackboardComponent. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CBlackboard> Blackboard;

        /** Animation clips referenced by SampleAnim opcodes, indexed by clip index. */
        PROPERTY()
        TVector<TObjectPtr<CAnimation>> Clips;

        /** Blend spaces referenced by SampleBlendSpace opcodes, indexed by blend-space index. */
        PROPERTY()
        TVector<TObjectPtr<CBlendSpace>> BlendSpaces;

        /** Lua- and editor-tweakable parameters that drive the graph. */
        TVector<FAnimGraphParameter> Parameters;

        /** Editor-authored bone mask definitions; resolved into BoneMasks at compile. */
        PROPERTY(Editable, Category = "Bone Masks")
        TVector<FAnimGraphBoneMaskDef> BoneMaskDefs;

        /** Compiled per-bone weight arrays referenced by BlendMasked opcodes. */
        TVector<FAnimGraphBoneMask> BoneMasks;

        /** State machines referenced by EvalStateMachine opcodes, indexed by machine index. */
        TVector<FAnimGraphStateMachine> StateMachines;

        /** Curve slots carried by every pose in this graph: the union of the referenced clips' curves
         *  and any name a Get/Set Curve node uses. Slot index is what the bytecode addresses. */
        TVector<FName> CurveNames;

        /** Curve slot bindings for Clips / BlendSpaces, parallel to those arrays. */
        TVector<FAnimGraphClipCurveMap> ClipCurveMaps;
        TVector<FAnimGraphBlendSpaceCurveMap> BlendSpaceCurveMaps;

        /** Compiled instruction stream consumed by FAnimationGraphVM. */
        TVector<uint8> Bytecode;

        /** Register-file and persistent-state sizing produced by the compiler. */
        uint16 NumScalarRegisters = 0;
        uint16 NumPoseRegisters = 0;
        uint16 NumStateSlots = 0;

        /** Sync groups referenced by AdvanceClock opcodes; members share one normalized phase. */
        uint16 NumSyncGroups = 0;

        /** Opcode layout the bytecode was compiled against (kAnimBytecodeVersion); the VM refuses
         *  mismatches instead of misparsing. 0 = compiled before versioning existed. */
        uint16 BytecodeVersion = 0;
    };
}
