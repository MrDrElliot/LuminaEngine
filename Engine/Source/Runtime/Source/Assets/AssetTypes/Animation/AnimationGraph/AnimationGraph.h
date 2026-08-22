#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/InstancedStruct.h"
#include "AnimationGraph.generated.h"

namespace Lumina
{
    class CSkeleton;
    class CAnimation;
    class CStruct;
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

    // Where a transition's left-hand value comes from.
    REFLECT()
    enum class EAnimTransitionSource : uint8
    {
        /** The graph parameter named by Condition Parameter. */
        Parameter,

        /** Seconds the state machine has spent in its current state, so a state can time itself out. */
        TimeInState,
    };

    // How the bytes at a resolved offset decode. Filled at link from the property's TypeFlags.
    enum class EAnimParamValueType : uint8
    {
        Unresolved,
        Float,
        Double,
        Bool,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        Object,
    };

    // A parameter name resolved to a byte offset in the entity's blackboard struct. Transient.
    struct FAnimGraphParamBinding
    {
        uint32 Offset = 0;
        EAnimParamValueType Type = EAnimParamValueType::Unresolved;

        bool IsResolved() const { return Type != EAnimParamValueType::Unresolved; }
    };

    /** Unresolved for anything a parameter cannot read: containers, structs, strings, delegates. */
    RUNTIME_API EAnimParamValueType AnimParamValueTypeFromProperty(const FProperty* Property);

    FORCEINLINE float ReadAnimParamScalar(const uint8* Base, const FAnimGraphParamBinding& Binding)
    {
        const uint8* At = Base + Binding.Offset;
        switch (Binding.Type)
        {
        case EAnimParamValueType::Float:  return *reinterpret_cast<const float*>(At);
        case EAnimParamValueType::Double: return (float)*reinterpret_cast<const double*>(At);
        case EAnimParamValueType::Bool:   return *reinterpret_cast<const bool*>(At) ? 1.0f : 0.0f;
        case EAnimParamValueType::Int8:   return (float)*reinterpret_cast<const int8*>(At);
        case EAnimParamValueType::Int16:  return (float)*reinterpret_cast<const int16*>(At);
        case EAnimParamValueType::Int32:  return (float)*reinterpret_cast<const int32*>(At);
        case EAnimParamValueType::Int64:  return (float)*reinterpret_cast<const int64*>(At);
        case EAnimParamValueType::UInt8:  return (float)*reinterpret_cast<const uint8*>(At);
        case EAnimParamValueType::UInt16: return (float)*reinterpret_cast<const uint16*>(At);
        case EAnimParamValueType::UInt32: return (float)*reinterpret_cast<const uint32*>(At);
        case EAnimParamValueType::UInt64: return (float)*reinterpret_cast<const uint64*>(At);
        default:                          return 0.0f;
        }
    }

    // TObjectPtr<T> holds exactly one T*, so an object property reads as a plain pointer at its offset.
    FORCEINLINE CObject* ReadAnimParamObject(const uint8* Base, const FAnimGraphParamBinding& Binding)
    {
        return *reinterpret_cast<CObject* const*>(Base + Binding.Offset);
    }

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

    // What a node expects an object parameter to hold, so the picker and compiler can reject a mismatch.
    REFLECT()
    enum class EAnimObjectParamType : uint8
    {
        Animation,
        BlendSpace,
    };

    // A named object-valued input, letting the graph swap which asset a node samples at runtime.
    REFLECT()
    struct FAnimGraphObjectParameter
    {
        GENERATED_BODY()

        /** Field on the graph's parameter struct this asset reference is read from. */
        PROPERTY()
        FName Name;

        /** Asset kind the consuming node expects. */
        PROPERTY()
        EAnimObjectParamType Type = EAnimObjectParamType::Animation;
    };

    // One compiled state-machine edge; the VM cross-fades FromState->ToState over BlendDuration
    // when the condition passes while FromState is active.
    struct FAnimGraphTransition
    {
        // -1 means "any state" (checked regardless of active state).
        int32 FromState = -1;
        int32 ToState = 0;

        // What the compare reads. ConditionParameter is ignored unless this is Parameter.
        EAnimTransitionSource ConditionSource = EAnimTransitionSource::Parameter;

        // Gating parameter; empty is unconditional, and a name that resolves to nothing never passes.
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
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_STATE_CLOCKS)
            {
                Ar << Data.ConditionSource;
            }
            return Ar;
        }
    };

    // One bone of a bone mask; resolved to a skeleton index and baked into FAnimGraphBoneMask::Weights at compile.
    REFLECT()
    struct FAnimGraphBoneMaskBone
    {
        GENERATED_BODY()

        /** Name of the bone to weight. Must exist in the graph's skeleton. */
        PROPERTY(Editable, Category = "Bone Mask", Picker = "Bone")
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

        // Clocks only: a nested machine's bookkeeping sits in its owner's blend tree and must not be zeroed.
        TVector<uint16> ClockSlots;
        TVector<uint16> StateClockSlotFirst;
        TVector<uint16> StateClockSlotEnd;

        // Slots into FAnimGraphVMState.StateSlots: Current/From state indices (From -1 when not
        // transitioning), seconds spent in the current state, Duration of the active transition.
        uint16 CurrentStateSlot = 0;
        uint16 FromStateSlot = 0;
        uint16 TimeInStateSlot = 0;
        uint16 DurationSlot = 0;

        friend FArchive& operator << (FArchive& Ar, FAnimGraphStateMachine& Data)
        {
            Ar << Data.EntryState;
            Ar << Data.StatePoseRegisters;
            Ar << Data.Transitions;
            Ar << Data.CurrentStateSlot;
            Ar << Data.FromStateSlot;
            Ar << Data.TimeInStateSlot;
            Ar << Data.DurationSlot;
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_STATE_CLOCK_LIST)
            {
                Ar << Data.ClockSlots;
                Ar << Data.StateClockSlotFirst;
                Ar << Data.StateClockSlotEnd;
            }
            else if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_STATE_CLOCKS)
            {
                // The interim layout's ranges addressed a different array; drop them and let a recompile refill.
                TVector<uint16> UnusedClockSlotFirst;
                TVector<uint16> UnusedClockSlotEnd;
                Ar << UnusedClockSlotFirst;
                Ar << UnusedClockSlotEnd;
            }
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

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }

        int32 FindParameterIndex(const FName& Name) const;

        int32 FindObjectParameterIndex(const FName& Name) const;

        int32 FindCurveIndex(const FName& Name) const;

        // Fills every transition's CachedParamIndex; call after Parameters/StateMachines change.
        void ResolveTransitionParameters();

        // Swapping the parameter struct moves every offset, so re-link rather than trust the old ones.
        void PostPropertyChange(FProperty* ChangedProperty) override { LinkParameters(); }

        /** The parameter block's type, or null when none is assigned. */
        CStruct* GetParameterStruct() const { return ParameterStruct.GetScriptStruct(); }

        // Resolves every parameter name to an offset in the parameter struct; call after either changes.
        void LinkParameters();

        bool IsCompiled() const { return !Bytecode.empty(); }

        /** Skeleton every pose produced by this graph is authored against. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CSkeleton> Skeleton;

        /** Parameter block this graph reads. The instance's values are the authored defaults. */
        PROPERTY(Editable, Category = "Animation")
        FInstancedStruct ParameterStruct;

        /** Animation clips referenced by SampleAnim opcodes, indexed by clip index. */
        PROPERTY()
        TVector<TObjectPtr<CAnimation>> Clips;

        /** Blend spaces referenced by SampleBlendSpace opcodes, indexed by blend-space index. */
        PROPERTY()
        TVector<TObjectPtr<CBlendSpace>> BlendSpaces;

        /** Lua- and editor-tweakable parameters that drive the graph. */
        TVector<FAnimGraphParameter> Parameters;

        /** Object-valued inputs; the runtime fills these so nodes can swap which asset they sample. */
        PROPERTY()
        TVector<FAnimGraphObjectParameter> ObjectParameters;

        /** Assets referenced by LoadObjectConst, so a static pin still flows through an object register. */
        PROPERTY()
        TVector<TObjectPtr<CObject>> ObjectConstants;

        /** Offsets into the blackboard struct, parallel to Parameters / ObjectParameters. Transient. */
        TVector<FAnimGraphParamBinding> ParamBindings;
        TVector<FAnimGraphParamBinding> ObjectParamBindings;

        /** Curve slots to register even when no node reads them, so a runtime-chosen clip can drive them. */
        PROPERTY(Editable, Category = "Animation")
        TVector<FName> DeclaredCurves;

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
        uint16 NumObjectRegisters = 0;
        uint16 NumScalarRegisters = 0;
        uint16 NumPoseRegisters = 0;
        uint16 NumStateSlots = 0;

        /** Sync groups referenced by AdvanceClock opcodes; members share one normalized phase. */
        uint16 NumSyncGroups = 0;

        /** Per-instance smoothing records the Inertialization and Dead Blending nodes address. */
        uint16 NumInertializerNodes = 0;
        uint16 NumDeadBlendNodes = 0;

        /** Montage slot names referenced by EvalSlot opcodes, indexed by slot index. */
        TVector<FName> SlotNames;

        /** Opcode layout the bytecode was compiled against (kAnimBytecodeVersion); the VM refuses
         *  mismatches instead of misparsing. 0 = compiled before versioning existed. */
        uint16 BytecodeVersion = 0;
    };
}
