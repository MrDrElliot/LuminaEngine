#include "AnimGraphNode_FootPlacement.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        void ReportError(FAnimationGraphCompiler& Compiler, CEdGraphNode* Node, const char* Name, const FString& Description)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = Name;
            NodeError.Description = Description;
            NodeError.Node        = Node;
            Compiler.AddError(NodeError);
        }

        struct FResolvedLeg
        {
            uint16 Thigh = 0;
            uint16 Calf  = 0;
            uint16 Foot  = 0;
            float  SoleHeight = 0.0f;
        };

        struct FLegRegisters
        {
            uint16 OffsetX = 0, OffsetY = 0, OffsetZ = 0;
            uint16 NormalX = 0, NormalY = 0, NormalZ = 0;
        };
    }

    void CAnimGraphNode_FootPlacement::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
    }

    void CAnimGraphNode_FootPlacement::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);
        Compiler.SetPinRegister(PoseOutPin, SrcReg);

        const int32 PelvisIndex = Compiler.ResolveBoneIndex(PelvisBone);
        if (PelvisIndex == INDEX_NONE)
        {
            ReportError(Compiler, this, "Unknown Pelvis Bone",
                        FString("Foot Placement references '") + PelvisBone.ToString() +
                        "', which is not a bone on the graph's skeleton.");
            return;
        }

        if (Legs.empty())
        {
            ReportError(Compiler, this, "No Legs",
                        "Foot Placement has no legs, so it would trace nothing. Add one leg per foot.");
            return;
        }

        const FSkeletonResource* Skeleton = Compiler.GetSkeleton();

        TVector<FResolvedLeg> Resolved;
        Resolved.reserve(Legs.size());

        for (const SAnimFootPlacementLeg& Leg : Legs)
        {
            const int32 ThighIndex = Compiler.ResolveBoneIndex(Leg.ThighBone);
            const int32 CalfIndex  = Compiler.ResolveBoneIndex(Leg.CalfBone);
            const int32 FootIndex  = Compiler.ResolveBoneIndex(Leg.FootBone);

            if (ThighIndex == INDEX_NONE || CalfIndex == INDEX_NONE || FootIndex == INDEX_NONE)
            {
                const FName& Missing = ThighIndex == INDEX_NONE ? Leg.ThighBone
                                     : CalfIndex == INDEX_NONE  ? Leg.CalfBone
                                                                : Leg.FootBone;
                ReportError(Compiler, this, "Unknown Leg Bone",
                            FString("Foot Placement references '") + Missing.ToString() +
                            "', which is not a bone on the graph's skeleton.");
                return;
            }

            // A broken chain solves onto nothing, and silently, which is worse than refusing to compile.
            if (Skeleton != nullptr &&
                (Skeleton->GetBone(CalfIndex).ParentIndex != ThighIndex ||
                 Skeleton->GetBone(FootIndex).ParentIndex != CalfIndex))
            {
                ReportError(Compiler, this, "Bad Leg Chain",
                            FString("'") + Leg.ThighBone.ToString() + "' -> '" + Leg.CalfBone.ToString() +
                            "' -> '" + Leg.FootBone.ToString() + "' is not a parent chain on the skeleton.");
                return;
            }

            Resolved.push_back(FResolvedLeg{ (uint16)ThighIndex, (uint16)CalfIndex, (uint16)FootIndex,
                                             Math::Max(Leg.SoleHeight, 0.0f) });
        }

        const uint16 AlphaReg    = ResolveAlphaInput(AlphaPin, Compiler, AlphaEasing);
        const uint16 HalfLifeReg = Compiler.EmitLoadConst(Math::Max(SmoothingHalfLife, 0.0f));
        const uint16 AlignReg    = Compiler.EmitLoadConst(Math::Clamp(GroundAlignment, 0.0f, 1.0f));

        TVector<FLegRegisters> LegRegisters;
        LegRegisters.reserve(Resolved.size());

        uint16 UpX = 0;
        uint16 UpY = 0;
        uint16 UpZ = 0;

        for (SIZE_T i = 0; i < Resolved.size(); ++i)
        {
            const FAnimationGraphCompiler::FGroundTraceRegisters Trace = Compiler.EmitGroundTrace(
                Resolved[i].Foot, UpAxis,
                Math::Max(TraceUpDistance, 0.0f), Math::Max(TraceDownDistance, 0.0f),
                Math::Max(MaxOffset, 0.0f), Resolved[i].SoleHeight,
                (uint16)TraceLayerMask, AlphaReg);

            FLegRegisters Registers;
            Registers.OffsetX = Compiler.EmitSmoothScalar(Trace.OffsetX, HalfLifeReg);
            Registers.OffsetY = Compiler.EmitSmoothScalar(Trace.OffsetY, HalfLifeReg);
            Registers.OffsetZ = Compiler.EmitSmoothScalar(Trace.OffsetZ, HalfLifeReg);
            Registers.NormalX = Compiler.EmitSmoothScalar(Trace.NormalX, HalfLifeReg);
            Registers.NormalY = Compiler.EmitSmoothScalar(Trace.NormalY, HalfLifeReg);
            Registers.NormalZ = Compiler.EmitSmoothScalar(Trace.NormalZ, HalfLifeReg);
            LegRegisters.push_back(Registers);

            // Every trace resolves the same component-space up, so the first one speaks for all of them.
            if (i == 0)
            {
                UpX = Trace.UpX;
                UpY = Trace.UpY;
                UpZ = Trace.UpZ;
            }
        }

        const auto Dot = [&](const FLegRegisters& Registers) -> uint16
        {
            const uint16 X = Compiler.EmitScalarOp(EAnimScalarOp::Mul, Registers.OffsetX, UpX);
            const uint16 Y = Compiler.EmitScalarOp(EAnimScalarOp::Mul, Registers.OffsetY, UpY);
            const uint16 Z = Compiler.EmitScalarOp(EAnimScalarOp::Mul, Registers.OffsetZ, UpZ);
            return Compiler.EmitScalarOp(EAnimScalarOp::Add, Compiler.EmitScalarOp(EAnimScalarOp::Add, X, Y), Z);
        };

        // Seeded at zero, so the pelvis only ever drops toward a foot that had to reach down.
        uint16 LowestReg = Compiler.EmitLoadConst(0.0f);
        for (const FLegRegisters& Registers : LegRegisters)
        {
            LowestReg = Compiler.EmitScalarOp(EAnimScalarOp::Min, LowestReg, Dot(Registers));
        }

        const uint16 PelvisX = Compiler.EmitScalarOp(EAnimScalarOp::Mul, UpX, LowestReg);
        const uint16 PelvisY = Compiler.EmitScalarOp(EAnimScalarOp::Mul, UpY, LowestReg);
        const uint16 PelvisZ = Compiler.EmitScalarOp(EAnimScalarOp::Mul, UpZ, LowestReg);

        uint16 PoseReg = Compiler.EmitTranslateBone(SrcReg, AlphaReg, PelvisX, PelvisY, PelvisZ, (uint16)PelvisIndex);

        // The pelvis carried every foot down with it, so each leg reaches for what is left of its offset.
        for (SIZE_T i = 0; i < Resolved.size(); ++i)
        {
            const FLegRegisters& Registers = LegRegisters[i];

            const uint16 FootX = Compiler.EmitScalarOp(EAnimScalarOp::Sub, Registers.OffsetX, PelvisX);
            const uint16 FootY = Compiler.EmitScalarOp(EAnimScalarOp::Sub, Registers.OffsetY, PelvisY);
            const uint16 FootZ = Compiler.EmitScalarOp(EAnimScalarOp::Sub, Registers.OffsetZ, PelvisZ);

            PoseReg = Compiler.EmitFootIK(PoseReg, AlphaReg, FootX, FootY, FootZ,
                                          Registers.NormalX, Registers.NormalY, Registers.NormalZ,
                                          AlignReg, Resolved[i].Thigh, Resolved[i].Calf, Resolved[i].Foot,
                                          FootUpAxis);
        }

        Compiler.SetPinRegister(PoseOutPin, PoseReg);
    }
}
