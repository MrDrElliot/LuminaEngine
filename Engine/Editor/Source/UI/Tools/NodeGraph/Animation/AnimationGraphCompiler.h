#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CAnimation;
    class CAnimationGraph;
    class CBlackboard;
    class CBlendSpace;
    class CEdGraphNode;
    struct FSkeletonResource;

    // Debug breadcrumb: maps an editor State node to its runtime slot + state index so the editor can
    // highlight the VM's current state. Populated by the State Machine node.
    struct FAnimGraphDebugStateNode
    {
        CEdGraphNode* Node = nullptr;
        uint16        CurrentStateSlot = 0;
        int32         StateIndex = 0;
    };

    // Bytecode-emission backend: nodes call Emit*/Alloc*/Add* during CompileGraph, BuildGraph then
    // stamps the program, resource tables, and register sizing into the runtime CAnimationGraph asset.
    class FAnimationGraphCompiler
    {
    public:

        FAnimationGraphCompiler() = default;

        // Registers a clip and returns its index; dedups identical clips.
        uint16 AddClip(CAnimation* Clip);

        // Registers a blend space and returns its index; dedups identical assets.
        uint16 AddBlendSpace(CBlendSpace* BlendSpace);

        // Registers an exposed parameter and returns its index; dedups by name.
        // A name collision with a different type reports an error and returns the existing index.
        int32 AddParameter(const FName& Name, EAnimGraphParamType Type, float DefaultValue);

        uint16 AllocScalarReg() { return NextScalarReg++; }
        uint16 AllocPoseReg()   { return NextPoseReg++; }
        uint16 AllocStateSlot() { return NextStateSlot++; }

        // Value-producing emitters return the destination register they allocated; callers thread that
        // index into downstream emitters.
        uint16 EmitLoadConst(float Value);
        uint16 EmitLoadParam(uint16 ParameterIndex);
        uint16 EmitScalarOp(EAnimScalarOp Op, uint16 RegA, uint16 RegB);

        // Advances a persistent playback clock; returns the clock and writes a "finished" flag to
        // OutFinishedReg (stays 0 in Loop mode; PlayOnce sets it once the clip's duration is reached).
        // SyncGroup (kAnimNoSyncGroup = none) makes the clip sample at its group's shared phase.
        uint16 EmitAdvanceClock(uint16 StateSlot, uint16 SpeedReg, uint16 ClipIndex, uint16 LoopModeReg, uint16& OutFinishedReg,
                                uint16 SyncGroup = kAnimNoSyncGroup);

        // Registers a named sync group and returns its index; dedups by name.
        uint16 AddSyncGroup(const FName& Name);

        uint16 EmitSampleAnim(uint16 ClipIndex, uint16 TimeReg);

        // Samples a blend space at (X, Y). The op owns its playback phase in PhaseSlot and advances it
        // against the weighted-blend duration, so the contributing clips stay stride-aligned.
        uint16 EmitSampleBlendSpace(uint16 BlendSpaceIndex, uint16 XReg, uint16 YReg, uint16 SpeedReg, uint16 PhaseSlot);
        uint16 EmitRefPose();
        uint16 EmitBlend(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg);
        uint16 EmitBlendMasked(uint16 PoseRegA, uint16 PoseRegB, uint16 AlphaReg, uint16 MaskIndex);

        // Additive blending: MakeAdditive produces a pose-delta against the
        // skeleton's bind pose; ApplyAdditive layers that delta onto a base pose.
        uint16 EmitMakeAdditive(uint16 SrcPoseReg);
        uint16 EmitApplyAdditive(uint16 BasePoseReg, uint16 DeltaPoseReg, uint16 AlphaReg);

        // Registers a compiled state machine and emits its eval opcode. The machine's
        // StatePoseRegisters / *Slot fields must be filled by the caller. Returns the resolved-pose register.
        uint16 EmitEvalStateMachine(FAnimGraphStateMachine&& StateMachine);

        void   EmitOutput(uint16 PoseReg);
        void   EmitHalt();

        // Nodes record the register their output pin resolved to; downstream nodes look it up to thread
        // the value through. Keyed on the pin pointer, valid for one CompileGraph pass.
        void SetPinRegister(const CEdNodeGraphPin* Pin, uint16 Register);
        bool TryGetPinRegister(const CEdNodeGraphPin* Pin, uint16& OutRegister) const;

        // Captured by the editor tool after compile to drive the live debug overlay
        // (pin values, active-state highlight).
        const THashMap<const CEdNodeGraphPin*, uint16>& GetPinRegisters() const { return PinRegisters; }

        void AddDebugStateNode(CEdGraphNode* Node, uint16 CurrentStateSlot, int32 StateIndex)
        {
            DebugStateNodes.push_back({ Node, CurrentStateSlot, StateIndex });
        }
        const TVector<FAnimGraphDebugStateNode>& GetDebugStateNodes() const { return DebugStateNodes; }

        // Modifies a single bone of an incoming pose. Returns the destination
        // pose register; (T, R, S) are baked into the bytecode at compile time.
        uint16 EmitBoneTransform(uint16 SrcPoseReg, uint16 AlphaReg, uint16 BoneIndex,
                                 uint16 SpaceReg, uint16 ModeReg,
                                 const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale);

        // Two-bone analytical IK. Target X/Y/Z come from scalar registers so they
        // can be driven dynamically; Pole is baked at compile time.
        uint16 EmitTwoBoneIK(uint16 SrcPoseReg, uint16 AlphaReg,
                             uint16 TargetXReg, uint16 TargetYReg, uint16 TargetZReg,
                             uint16 RootIndex, uint16 MidIndex, uint16 EndIndex,
                             const FVector3& Pole);

        // Tool calls SetSkeleton + ResolveBoneMasks once up front so nodes can look up bones / masks
        // by name in GenerateBytecode without re-fetching the asset.
        void SetSkeleton(const FSkeletonResource* InSkeleton) { Skeleton = InSkeleton; }
        const FSkeletonResource* GetSkeleton() const { return Skeleton; }

        // Tool sets the blackboard schema before compiling so nodes can verify referenced keys
        // still exist and are scalar-typed.
        void SetBlackboard(const CBlackboard* InBlackboard) { Blackboard = InBlackboard; }
        const CBlackboard* GetBlackboard() const { return Blackboard; }

        // Warns if Name is missing on the assigned blackboard or isn't a scalar (Float/Int/Bool/Enum).
        // No-op when no blackboard is assigned or Name is None.
        void ValidateParameterKey(const FName& Name, CEdGraphNode* Node);

        int32 ResolveBoneIndex(const FName& BoneName) const;

        void ResolveBoneMasks(const TVector<FAnimGraphBoneMaskDef>& Defs, const FSkeletonResource* InSkeleton);
        int32 FindBoneMaskIndex(const FName& Name) const;

        // Ad-hoc bone mask from a bone's subtree (weight 1.0 for the root when bInclusive + descendants);
        // appended to the mask table, returns its index. Lets Layered Blend Per Bone mask by bone choice.
        uint16 AddBoneSubtreeMask(int32 RootBoneIndex, bool bInclusive);

        FORCEINLINE bool HasErrors() const { return !Errors.empty(); }
        FORCEINLINE void AddError(const EdNodeGraph::FError& Error) { Errors.push_back(Error); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetErrors() const { return Errors; }

        // Non-fatal diagnostics (e.g. a node references a renamed/removed
        // blackboard key). The graph still compiles; the tool surfaces these.
        FORCEINLINE void AddWarning(const EdNodeGraph::FError& Warning) { Warnings.push_back(Warning); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetWarnings() const { return Warnings; }

        // Writes the compiled program, clip / parameter tables, and register sizing into OutGraph.
        // Appends a Halt if the program did not end in an Output. Call once per compile.
        void BuildGraph(CAnimationGraph* OutGraph);

    private:

        template <typename T>
        void Write(const T& Value)
        {
            const uint8* Bytes = reinterpret_cast<const uint8*>(&Value);
            Bytecode.insert(Bytecode.end(), Bytes, Bytes + sizeof(T));
        }

        void WriteOp(EAnimOp Op) { Bytecode.push_back((uint8)Op); }

        TVector<uint8>                          Bytecode;
        TVector<TObjectPtr<CAnimation>>         Clips;
        TVector<TObjectPtr<CBlendSpace>>        BlendSpaces;
        TVector<FName>                          SyncGroupNames;
        TVector<FAnimGraphParameter>            Parameters;
        TVector<FAnimGraphBoneMask>             BoneMasks;
        THashMap<FName, int32>                  BoneMaskNameToIndex;
        TVector<FAnimGraphStateMachine>         StateMachines;
        TVector<EdNodeGraph::FError>            Errors;
        TVector<EdNodeGraph::FError>            Warnings;
        THashMap<const CEdNodeGraphPin*, uint16> PinRegisters;
        TVector<FAnimGraphDebugStateNode>       DebugStateNodes;
        const FSkeletonResource*                Skeleton = nullptr;
        const CBlackboard*                      Blackboard = nullptr;

        uint16 NextScalarReg = 0;
        uint16 NextPoseReg   = 0;
        uint16 NextStateSlot = 0;

        bool bEmittedOutput = false;
    };
}
