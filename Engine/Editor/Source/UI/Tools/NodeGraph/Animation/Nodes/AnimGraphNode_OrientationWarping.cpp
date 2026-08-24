#include "AnimGraphNode_OrientationWarping.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        void ReportWarpError(FAnimationGraphCompiler& Compiler, CEdGraphNode* Node, const char* Name, const FString& Description)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = Name;
            NodeError.Description = Description;
            NodeError.Node        = Node;
            Compiler.AddError(NodeError);
        }

        bool WarpDescendsFrom(const FSkeletonResource& Skeleton, int32 BoneIndex, int32 AncestorIndex)
        {
            for (int32 Cursor = BoneIndex; Cursor >= 0; Cursor = Skeleton.GetBone(Cursor).ParentIndex)
            {
                if (Cursor == AncestorIndex)
                {
                    return true;
                }
            }
            return false;
        }

        uint16 EmitWarpSmoothedAngle(FAnimationGraphCompiler& Compiler, uint16 AngleReg, float HalfLife)
        {
            return HalfLife > 0.0f
                ? Compiler.EmitSmoothScalar(AngleReg, Compiler.EmitLoadConst(HalfLife))
                : AngleReg;
        }

        uint16 EmitWarpScaled(FAnimationGraphCompiler& Compiler, uint16 ValueReg, float Scale)
        {
            return Compiler.EmitScalarOp(EAnimScalarOp::Mul, ValueReg, Compiler.EmitLoadConst(Scale));
        }
    }

    void CAnimGraphNode_OrientationWarping::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        AnglePin   = CreateAnimPin("Angle", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
        BindFloatPinEditor(AnglePin);
    }

    void CAnimGraphNode_OrientationWarping::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);
        Compiler.SetPinRegister(PoseOutPin, SrcReg);

        const int32 PelvisIndex = Compiler.ResolveBoneIndex(PelvisBone);
        if (PelvisIndex == INDEX_NONE)
        {
            ReportWarpError(Compiler, this, "Unknown Pelvis Bone",
                        FString("Orientation Warping references '") + PelvisBone.ToString() +
                        "', which is not a bone on the graph's skeleton.");
            return;
        }

        const FSkeletonResource* Skeleton = Compiler.GetSkeleton();

        TVector<uint16> SpineIndices;
        TVector<float>  SpineWeights;
        float TotalWeight = 0.0f;

        for (const SAnimOrientationSpineBone& Spine : SpineBones)
        {
            const int32 SpineIndex = Compiler.ResolveBoneIndex(Spine.Bone);
            if (SpineIndex == INDEX_NONE)
            {
                ReportWarpError(Compiler, this, "Unknown Spine Bone",
                            FString("Orientation Warping references '") + Spine.Bone.ToString() +
                            "', which is not a bone on the graph's skeleton.");
                return;
            }

            // Counter-rotating a bone the pelvis does not carry would twist it away from the torso.
            if (Skeleton != nullptr && !WarpDescendsFrom(*Skeleton, SpineIndex, PelvisIndex))
            {
                ReportWarpError(Compiler, this, "Spine Bone Outside Chain",
                            FString("'") + Spine.Bone.ToString() + "' does not descend from '" +
                            PelvisBone.ToString() + "', so the pelvis turn never reaches it.");
                return;
            }

            const float Weight = Math::Max(Spine.Weight, 0.0f);
            SpineIndices.push_back((uint16)SpineIndex);
            SpineWeights.push_back(Weight);
            TotalWeight += Weight;
        }

        const uint16 AlphaReg = ResolveAlphaInput(AlphaPin, Compiler, AlphaEasing);

        uint16 AngleReg;
        if (AngleSource == EOrientationAngleSource::Velocity)
        {
            AngleReg = Compiler.EmitLoadMoveAngle(RotationAxis, ForwardAxis, Math::Max(MinSpeed, 0.0f));
        }
        else
        {
            AngleReg = EmitWarpScaled(Compiler, ResolveValueInput(AnglePin, Compiler), Math::Radians(1.0f));
        }

        const float MaxRadians = Math::Radians(Math::Clamp(MaxAngle, 0.0f, 180.0f));
        AngleReg = Compiler.EmitScalarOp(EAnimScalarOp::Min, AngleReg, Compiler.EmitLoadConst(MaxRadians));
        AngleReg = Compiler.EmitScalarOp(EAnimScalarOp::Max, AngleReg, Compiler.EmitLoadConst(-MaxRadians));

        AngleReg = EmitWarpSmoothedAngle(Compiler, AngleReg, SmoothingHalfLife);

        uint16 PoseReg = Compiler.EmitAxisRotateBone(SrcReg, AlphaReg, AngleReg, RotationAxis, (uint16)PelvisIndex);

        // Each share compounds onto the ones below it, so the shares summing to one undoes the pelvis turn.
        const float Counter = Math::Clamp(CounterRotation, 0.0f, 1.0f);
        if (Counter > 0.0f && TotalWeight > 0.0f)
        {
            for (SIZE_T i = 0; i < SpineIndices.size(); ++i)
            {
                const float Share = -Counter * (SpineWeights[i] / TotalWeight);
                if (Share == 0.0f)
                {
                    continue;
                }

                PoseReg = Compiler.EmitAxisRotateBone(PoseReg, AlphaReg, EmitWarpScaled(Compiler, AngleReg, Share),
                                                      RotationAxis, SpineIndices[i]);
            }
        }

        Compiler.SetPinRegister(PoseOutPin, PoseReg);
    }

    void CAnimGraphNode_MovementAngle::BuildNode()
    {
        AngleOutPin = CreateAnimPin("Angle", ENodePinDirection::Output, EAnimPinType::Value);
    }

    void CAnimGraphNode_MovementAngle::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        uint16 AngleReg = Compiler.EmitLoadMoveAngle(RotationAxis, ForwardAxis, Math::Max(MinSpeed, 0.0f));

        AngleReg = EmitWarpSmoothedAngle(Compiler, AngleReg, SmoothingHalfLife);

        if (bOutputDegrees)
        {
            AngleReg = EmitWarpScaled(Compiler, AngleReg, Math::Degrees(1.0f));
        }

        Compiler.SetPinRegister(AngleOutPin, AngleReg);
    }
}
