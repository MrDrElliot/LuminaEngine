#include "RuntimePCH.h"
#include "AnimGraphPlan.h"

#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Memory/Memcpy.h"

namespace Lumina
{
    namespace
    {
        // Immediates are copied out as packed floats, so a vector type with padding would shift every operand after it.
        static_assert(sizeof(FVector3) == 3 * sizeof(float) && sizeof(FQuat) == 4 * sizeof(float));

        enum class EOperand : uint8 { U8, U16, F32, Vec3, Quat };

        struct FLayout
        {
            const EOperand* Fields = nullptr;
            int32 Count = 0;
            bool bKnown = false;
        };

        template <size_t N>
        FLayout Fields(const EOperand (&Array)[N])
        {
            return FLayout{ Array, (int32)N, true };
        }

        // Operand layouts exactly as the update reads them, so a decoded op hands its fields back in the same order.
        FLayout LayoutOf(EAnimOp Op)
        {
            using enum EOperand;
            static constexpr EOperand ConstLoad[]      = { F32, U16 };
            static constexpr EOperand TwoRegs[]        = { U16, U16 };
            static constexpr EOperand ThreeRegs[]      = { U16, U16, U16 };
            static constexpr EOperand FourRegs[]       = { U16, U16, U16, U16 };
            static constexpr EOperand FiveRegs[]       = { U16, U16, U16, U16, U16 };
            static constexpr EOperand SixRegs[]        = { U16, U16, U16, U16, U16, U16 };
            static constexpr EOperand SevenRegs[]      = { U16, U16, U16, U16, U16, U16, U16 };
            static constexpr EOperand NineRegs[]       = { U16, U16, U16, U16, U16, U16, U16, U16, U16 };
            static constexpr EOperand OneReg[]         = { U16 };
            static constexpr EOperand ScalarOp[]       = { U8, U16, U16, U16 };
            static constexpr EOperand MakeAdditiveEx[] = { U16, U16, U8, U16 };
            static constexpr EOperand EaseAlpha[]      = { U8, U16, U16 };
            static constexpr EOperand LookAt[]         = { U16, U16, U16, U16, U16, U16, Vec3, F32, U16 };
            static constexpr EOperand FootIK[]         = { U16, U16, U16, U16, U16, U16, U16, U16, U16, U16, U16, U16, Vec3, U16 };
            static constexpr EOperand GroundTrace[]    = { U16, Vec3, F32, F32, F32, F32, U16, U16, U16, U16, U16, U16, U16, U16, U16, U16, U16 };
            static constexpr EOperand MoveAngle[]      = { Vec3, Vec3, F32, U16 };
            static constexpr EOperand AxisRotate[]     = { U16, U16, U16, Vec3, U16, U16 };
            static constexpr EOperand BoneTransform[]  = { U16, U16, U16, U16, U16, Vec3, Quat, Vec3, U16 };
            static constexpr EOperand TwoBoneIK[]      = { U16, U16, U16, U16, U16, U16, U16, U16, Vec3, U16 };

            switch (Op)
            {
            case EAnimOp::Halt:                return FLayout{ nullptr, 0, true };
            case EAnimOp::LoadConst:           return Fields(ConstLoad);
            case EAnimOp::LoadParam:           return Fields(TwoRegs);
            case EAnimOp::LoadObjectParam:     return Fields(TwoRegs);
            case EAnimOp::LoadObjectConst:     return Fields(TwoRegs);
            case EAnimOp::ScalarOp:            return Fields(ScalarOp);
            case EAnimOp::AdvanceClock:        return Fields(NineRegs);
            case EAnimOp::AdvanceClockDyn:     return Fields(NineRegs);
            case EAnimOp::SampleAnim:          return Fields(ThreeRegs);
            case EAnimOp::SampleAnimDyn:       return Fields(ThreeRegs);
            case EAnimOp::SampleBlendSpace:    return Fields(NineRegs);
            case EAnimOp::SampleBlendSpaceDyn: return Fields(NineRegs);
            case EAnimOp::RefPose:             return Fields(OneReg);
            case EAnimOp::Blend:               return Fields(FourRegs);
            case EAnimOp::BlendMasked:         return Fields(FiveRegs);
            case EAnimOp::MakeAdditive:        return Fields(TwoRegs);
            case EAnimOp::MakeAdditiveEx:      return Fields(MakeAdditiveEx);
            case EAnimOp::SmoothScalar:        return Fields(FiveRegs);
            case EAnimOp::FABRIK:              return Fields(NineRegs);
            case EAnimOp::LookAt:              return Fields(LookAt);
            case EAnimOp::FootIK:              return Fields(FootIK);
            case EAnimOp::TranslateBone:       return Fields(SevenRegs);
            case EAnimOp::GroundTrace:         return Fields(GroundTrace);
            case EAnimOp::LoadMoveAngle:       return Fields(MoveAngle);
            case EAnimOp::AxisRotateBone:      return Fields(AxisRotate);
            case EAnimOp::EaseAlpha:           return Fields(EaseAlpha);
            case EAnimOp::SavePoseSnapshot:    return Fields(FourRegs);
            case EAnimOp::LoadPoseSnapshot:    return Fields(TwoRegs);
            case EAnimOp::Inertialize:         return Fields(FiveRegs);
            case EAnimOp::DeadBlend:           return Fields(SixRegs);
            case EAnimOp::ApplyAdditive:       return Fields(FourRegs);
            case EAnimOp::EvalStateMachine:    return Fields(TwoRegs);
            case EAnimOp::BoneTransform:       return Fields(BoneTransform);
            case EAnimOp::TwoBoneIK:           return Fields(TwoBoneIK);
            case EAnimOp::Output:              return Fields(OneReg);
            case EAnimOp::GetCurve:            return Fields(ThreeRegs);
            case EAnimOp::SetCurve:            return Fields(FourRegs);
            case EAnimOp::EvalSlot:            return Fields(ThreeRegs);
            default:                           break;
            }
            return FLayout();
        }

        // Reads past the end come back zeroed while the cursor still advances, as the byte reader always behaved.
        struct FBytecodeCursor
        {
            const uint8* Data = nullptr;
            SIZE_T Size = 0;
            SIZE_T Cursor = 0;

            void ReadRaw(void* Out, SIZE_T Count)
            {
                Memory::Memset(Out, 0, Count);
                if (Cursor + Count <= Size)
                {
                    Memory::Memcpy(Out, Data + Cursor, Count);
                }
                Cursor += Count;
            }

            template <typename T>
            T Read()
            {
                T Value;
                ReadRaw(&Value, sizeof(T));
                return Value;
            }
        };

        enum class ERole : uint8 { None, ScalarIn, ScalarOut, PoseIn, PoseOut, ObjectIn, ObjectOut, SnapshotIn, SnapshotOut };

        struct FRoles
        {
            ERole Of[FAnimPlanOp::MaxOperands] = {};
        };

        // What each integer operand means to dataflow, in the same order as LayoutOf.
        FRoles RolesOf(EAnimOp Op)
        {
            using enum ERole;
            FRoles Out;
            const auto Set = [&Out](std::initializer_list<ERole> Roles)
            {
                int32 Index = 0;
                for (const ERole Role : Roles)
                {
                    Out.Of[Index++] = Role;
                }
            };

            switch (Op)
            {
            case EAnimOp::LoadConst:           Set({ ScalarOut }); break;
            case EAnimOp::LoadParam:           Set({ None, ScalarOut }); break;
            case EAnimOp::LoadObjectParam:
            case EAnimOp::LoadObjectConst:     Set({ None, ObjectOut }); break;
            case EAnimOp::ScalarOp:            Set({ None, ScalarIn, ScalarIn, ScalarOut }); break;
            case EAnimOp::AdvanceClock:        Set({ None, ScalarIn, None, ScalarIn, ScalarIn, None, ScalarOut, ScalarOut, None }); break;
            case EAnimOp::AdvanceClockDyn:     Set({ None, ScalarIn, ObjectIn, ScalarIn, ScalarIn, None, ScalarOut, ScalarOut, None }); break;
            case EAnimOp::SampleAnim:          Set({ None, ScalarIn, PoseOut }); break;
            case EAnimOp::SampleAnimDyn:       Set({ ObjectIn, ScalarIn, PoseOut }); break;
            case EAnimOp::SampleBlendSpace:    Set({ None, ScalarIn, ScalarIn, ScalarIn, None, ScalarIn, None, None, PoseOut }); break;
            case EAnimOp::SampleBlendSpaceDyn: Set({ ObjectIn, ScalarIn, ScalarIn, ScalarIn, None, ScalarIn, None, None, PoseOut }); break;
            case EAnimOp::RefPose:             Set({ PoseOut }); break;
            case EAnimOp::Blend:               Set({ PoseIn, PoseIn, ScalarIn, PoseOut }); break;
            case EAnimOp::BlendMasked:         Set({ PoseIn, PoseIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::MakeAdditive:        Set({ PoseIn, PoseOut }); break;
            case EAnimOp::MakeAdditiveEx:      Set({ PoseIn, PoseIn, None, PoseOut }); break;
            case EAnimOp::SmoothScalar:        Set({ ScalarIn, ScalarIn, None, None, ScalarOut }); break;
            case EAnimOp::FABRIK:              Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, None, None, None, PoseOut }); break;
            case EAnimOp::LookAt:              Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::FootIK:              Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, None, None, None, PoseOut }); break;
            case EAnimOp::TranslateBone:       Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::GroundTrace:         Set({ None, None, ScalarIn, ScalarOut, ScalarOut, ScalarOut, ScalarOut, ScalarOut, ScalarOut, ScalarOut, ScalarOut, ScalarOut }); break;
            case EAnimOp::LoadMoveAngle:       Set({ ScalarOut }); break;
            case EAnimOp::AxisRotateBone:      Set({ PoseIn, ScalarIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::EaseAlpha:           Set({ None, ScalarIn, ScalarOut }); break;
            case EAnimOp::SavePoseSnapshot:    Set({ PoseIn, ScalarIn, SnapshotOut, PoseOut }); break;
            case EAnimOp::LoadPoseSnapshot:    Set({ SnapshotIn, PoseOut }); break;
            case EAnimOp::Inertialize:         Set({ PoseIn, ScalarIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::DeadBlend:           Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, None, PoseOut }); break;
            case EAnimOp::ApplyAdditive:       Set({ PoseIn, PoseIn, ScalarIn, PoseOut }); break;
            case EAnimOp::EvalStateMachine:    Set({ None, PoseOut }); break;
            case EAnimOp::BoneTransform:       Set({ PoseIn, ScalarIn, None, ScalarIn, ScalarIn, PoseOut }); break;
            case EAnimOp::TwoBoneIK:           Set({ PoseIn, ScalarIn, ScalarIn, ScalarIn, ScalarIn, None, None, None, PoseOut }); break;
            case EAnimOp::Output:              Set({ PoseIn }); break;
            case EAnimOp::GetCurve:            Set({ PoseIn, None, ScalarOut }); break;
            case EAnimOp::SetCurve:            Set({ PoseIn, None, ScalarIn, PoseOut }); break;
            case EAnimOp::EvalSlot:            Set({ None, PoseIn, PoseOut }); break;
            case EAnimOp::Forward:             Set({ PoseIn, PoseOut }); break;
            default: break;
            }
            return Out;
        }

        // Drops every op Keep rejects and repoints the state-code jumps at the next op that survives.
        void CompactOps(const TVector<FAnimPlanOp>& Ops, const TVector<bool>& Keep, FAnimGraphPlan& Plan)
        {
            TVector<FAnimPlanOp> Kept;
            TVector<uint32> NewIndex;
            NewIndex.SetNum(Ops.Num() + 1);
            for (int32 i = 0; i < Ops.Num(); ++i)
            {
                NewIndex[i] = (uint32)Kept.Num();
                if (Keep[i])
                {
                    Kept.Add(Ops[i]);
                }
            }
            NewIndex[Ops.Num()] = (uint32)Kept.Num();

            for (uint32& Jump : Plan.Jumps)
            {
                if (Jump != FAnimGraphPlan::InvalidJump)
                {
                    Jump = NewIndex[(int32)Jump];
                }
            }
            Plan.Ops = Move(Kept);
        }
    }

    void LowerAnimGraphPlan(const CAnimationGraph& Graph, FAnimGraphPlan& OutPlan, FAnimPlanSources* OutSources)
    {
        OutPlan.Reset();
        FAnimPlanSources Sources;
        FBytecodeCursor Reader{ Graph.Bytecode.GetData(), (SIZE_T)Graph.Bytecode.Num() };
        const SIZE_T Size = Reader.Size;

        TVector<FAnimPlanOp> Decoded;
        TVector<uint32> DecodedOffsets;
        TVector<uint32> IndexOfOffset;
        IndexOfOffset.Init(FAnimGraphPlan::InvalidJump, (int32)Size + 1);

        while (Reader.Cursor < Size)
        {
            IndexOfOffset[(int32)Reader.Cursor] = (uint32)Decoded.Num();
            DecodedOffsets.Add((uint32)Reader.Cursor);
            FAnimPlanOp& Op = Decoded.EmplaceGetRef();
            Op.Op    = Reader.Read<uint8>();
            Op.Extra = (uint32)OutPlan.Immediates.Num();

            if ((EAnimOp)Op.Op == EAnimOp::EnterStateMachine)
            {
                Op.U[0] = Reader.Read<uint16>();
                Op.U[1] = Reader.Read<uint16>();
                Op.Extra = (uint32)OutPlan.Jumps.Num();
                OutPlan.Jumps.Add(Reader.Read<uint32>());
                for (int32 i = 0; i < Op.U[1] * 2; ++i)
                {
                    OutPlan.Jumps.Add(Reader.Read<uint32>());
                }
                continue;
            }

            const FLayout Layout = LayoutOf((EAnimOp)Op.Op);
            if (!Layout.bKnown)
            {
                // An unknown opcode ends the stream, as it does at runtime.
                break;
            }

            int32 NextOperand = 0;
            for (int32 f = 0; f < Layout.Count; ++f)
            {
                switch (Layout.Fields[f])
                {
                case EOperand::U8:   Op.U[NextOperand++] = Reader.Read<uint8>(); break;
                case EOperand::U16:  Op.U[NextOperand++] = Reader.Read<uint16>(); break;
                case EOperand::F32:  OutPlan.Immediates.Add(Reader.Read<float>()); break;
                case EOperand::Vec3: for (int32 i = 0; i < 3; ++i) { OutPlan.Immediates.Add(Reader.Read<float>()); } break;
                case EOperand::Quat: for (int32 i = 0; i < 4; ++i) { OutPlan.Immediates.Add(Reader.Read<float>()); } break;
                }
            }
        }
        if (Reader.Cursor == Size)
        {
            IndexOfOffset[(int32)Size] = (uint32)Decoded.Num();
        }

        // Byte offsets become op indices, and one that is not an instruction boundary fails its range check.
        for (uint32& Jump : OutPlan.Jumps)
        {
            Jump = Jump <= Size ? IndexOfOffset[(int32)Jump] : FAnimGraphPlan::InvalidJump;
        }

        TVector<int32> ScalarWrites;
        TVector<int32> ObjectWrites;
        ScalarWrites.Init(0, Graph.NumScalarRegisters);
        ObjectWrites.Init(0, Graph.NumObjectRegisters);
        for (const FAnimPlanOp& Op : Decoded)
        {
            const FRoles Roles = RolesOf((EAnimOp)Op.Op);
            for (int32 u = 0; u < FAnimPlanOp::MaxOperands; ++u)
            {
                if (Roles.Of[u] == ERole::ScalarOut && Op.U[u] < Graph.NumScalarRegisters)
                {
                    ++ScalarWrites[Op.U[u]];
                }
                else if (Roles.Of[u] == ERole::ObjectOut && Op.U[u] < Graph.NumObjectRegisters)
                {
                    ++ObjectWrites[Op.U[u]];
                }
            }
        }

        // A load is lifted only when nothing else writes its register, so running it first changes no value any op reads.
        TVector<bool> Keep;
        Keep.Init(false, Decoded.Num());
        for (int32 i = 0; i < Decoded.Num(); ++i)
        {
            const FAnimPlanOp& Op = Decoded[i];
            const EAnimOp Code = (EAnimOp)Op.Op;
            const uint16 ScalarDst = Code == EAnimOp::LoadConst ? Op.U[0] : Op.U[1];
            const bool bScalarLoad = (Code == EAnimOp::LoadConst || Code == EAnimOp::LoadParam)
                && ScalarDst < Graph.NumScalarRegisters && ScalarWrites[ScalarDst] == 1;
            const bool bObjectLoad = (Code == EAnimOp::LoadObjectParam || Code == EAnimOp::LoadObjectConst)
                && Op.U[1] < Graph.NumObjectRegisters && ObjectWrites[Op.U[1]] == 1;

            if (bScalarLoad && Code == EAnimOp::LoadConst)
            {
                OutPlan.Constants.Add({ ScalarDst, OutPlan.Immediates[Op.Extra] });
                Sources.Constants.Add(DecodedOffsets[i]);
            }
            else if (bScalarLoad)
            {
                OutPlan.ParameterLoads.Add({ ScalarDst, Op.U[0] });
                Sources.ParameterLoads.Add(DecodedOffsets[i]);
            }
            else if (bObjectLoad && Code == EAnimOp::LoadObjectParam)
            {
                OutPlan.ObjectParameterLoads.Add({ Op.U[1], Op.U[0] });
                Sources.ObjectParameterLoads.Add(DecodedOffsets[i]);
            }
            else if (bObjectLoad)
            {
                OutPlan.ObjectConstantLoads.Add({ Op.U[1], Op.U[0] });
                Sources.ObjectConstantLoads.Add(DecodedOffsets[i]);
            }
            else
            {
                Keep[i] = true;
                Sources.Ops.Add(DecodedOffsets[i]);
            }
        }
        CompactOps(Decoded, Keep, OutPlan);

        if (OutSources != nullptr)
        {
            *OutSources = Move(Sources);
        }
    }

    void PruneAnimGraphPlanForCrowd(const CAnimationGraph& Graph, const FAnimGraphPlan& Full, FAnimGraphPlan& OutPlan, TVector<EAnimCrowdOpFate>* OutFates)
    {
        OutPlan = Full;
        TVector<FAnimPlanOp> Ops = Full.Ops;
        TVector<bool> Bypassed;
        Bypassed.Init(false, Ops.Num());

        // Overlays a distant character does without hand their base pose straight through.
        for (int32 i = 0; i < Ops.Num(); ++i)
        {
            FAnimPlanOp& Op = Ops[i];
            uint16 Src = 0;
            uint16 Dst = 0;
            switch ((EAnimOp)Op.Op)
            {
            case EAnimOp::FootIK:         Src = Op.U[0]; Dst = Op.U[12]; break;
            case EAnimOp::TwoBoneIK:
            case EAnimOp::FABRIK:         Src = Op.U[0]; Dst = Op.U[8]; break;
            case EAnimOp::LookAt:
            case EAnimOp::TranslateBone:  Src = Op.U[0]; Dst = Op.U[6]; break;
            case EAnimOp::AxisRotateBone: Src = Op.U[0]; Dst = Op.U[4]; break;
            case EAnimOp::BoneTransform:  Src = Op.U[0]; Dst = Op.U[5]; break;
            case EAnimOp::ApplyAdditive:  Src = Op.U[0]; Dst = Op.U[3]; break;
            case EAnimOp::BlendMasked:    Src = Op.U[0]; Dst = Op.U[4]; break;
            default: continue;
            }
            Op = FAnimPlanOp();
            Op.Op   = (uint8)EAnimOp::Forward;
            Op.U[0] = Src;
            Op.U[1] = Dst;
            Bypassed[i] = true;
        }

        TVector<bool> LiveScalar;
        TVector<bool> LivePose;
        TVector<bool> LiveObject;
        TVector<bool> LiveSnapshot;
        TVector<bool> LiveOp;
        LiveScalar.Init(false, Graph.NumScalarRegisters);
        LivePose.Init(false, Graph.NumPoseRegisters);
        LiveObject.Init(false, Graph.NumObjectRegisters);
        LiveSnapshot.Init(false, Graph.PoseSnapshotNames.Num());
        LiveOp.Init(false, Ops.Num());

        const auto LiveFor = [&](ERole Role) -> TVector<bool>*
        {
            switch (Role)
            {
            case ERole::ScalarIn:
            case ERole::ScalarOut:   return &LiveScalar;
            case ERole::PoseIn:
            case ERole::PoseOut:     return &LivePose;
            case ERole::ObjectIn:
            case ERole::ObjectOut:   return &LiveObject;
            case ERole::SnapshotIn:
            case ERole::SnapshotOut: return &LiveSnapshot;
            default:                 return nullptr;
            }
        };
        const auto IsOutput = [](ERole Role)
        {
            return Role == ERole::ScalarOut || Role == ERole::PoseOut || Role == ERole::ObjectOut || Role == ERole::SnapshotOut;
        };
        const auto Mark = [](TVector<bool>& Live, uint16 Register)
        {
            if (Register < Live.Num())
            {
                Live[Register] = true;
            }
        };

        // Flow-insensitive, so a value is live if any live op anywhere reads it, which keeps every path a state machine can take.
        bool bChanged = true;
        while (bChanged)
        {
            bChanged = false;
            for (int32 i = Ops.Num() - 1; i >= 0; --i)
            {
                if (LiveOp[i])
                {
                    continue;
                }
                const FAnimPlanOp& Op = Ops[i];
                const EAnimOp Code = (EAnimOp)Op.Op;
                const FRoles Roles = RolesOf(Code);

                bool bLive = Code == EAnimOp::EnterStateMachine || Code == EAnimOp::Output || Code == EAnimOp::Halt;
                for (int32 u = 0; u < FAnimPlanOp::MaxOperands && !bLive; ++u)
                {
                    const TVector<bool>* Live = LiveFor(Roles.Of[u]);
                    bLive = IsOutput(Roles.Of[u]) && Op.U[u] < Live->Num() && (*Live)[Op.U[u]];
                }
                if (!bLive)
                {
                    continue;
                }

                LiveOp[i] = true;
                bChanged = true;
                for (int32 u = 0; u < FAnimPlanOp::MaxOperands; ++u)
                {
                    if (TVector<bool>* Live = LiveFor(Roles.Of[u]); Live != nullptr && !IsOutput(Roles.Of[u]))
                    {
                        Mark(*Live, Op.U[u]);
                    }
                }
                if (Code == EAnimOp::EvalStateMachine && Op.U[0] < Graph.StateMachines.Num())
                {
                    const FAnimGraphStateMachine& Machine = Graph.StateMachines[Op.U[0]];
                    for (const uint16 Register : Machine.StatePoseRegisters)
                    {
                        Mark(LivePose, Register);
                    }
                    for (const uint16 Register : Machine.StateFinishedRegisters)
                    {
                        Mark(LiveScalar, Register);
                    }
                }
            }
        }

        // A machine whose states no longer run any live op has nothing left to decide.
        for (int32 i = 0; i < Ops.Num(); ++i)
        {
            if ((EAnimOp)Ops[i].Op != EAnimOp::EnterStateMachine)
            {
                continue;
            }
            const uint32 StatesEnd = Full.Jumps[(int32)Ops[i].Extra];
            bool bAnyLive = false;
            for (uint32 j = (uint32)i + 1; j < StatesEnd && j < (uint32)Ops.Num() && !bAnyLive; ++j)
            {
                bAnyLive = LiveOp[(int32)j];
            }
            LiveOp[i] = bAnyLive;
        }

        OutPlan.Constants.Reset();
        OutPlan.ParameterLoads.Reset();
        OutPlan.ObjectParameterLoads.Reset();
        OutPlan.ObjectConstantLoads.Reset();
        for (const FAnimPlanConstant& Constant : Full.Constants)
        {
            if (Constant.Register < LiveScalar.Num() && LiveScalar[Constant.Register])
            {
                OutPlan.Constants.Add(Constant);
            }
        }
        for (const FAnimPlanLoad& Load : Full.ParameterLoads)
        {
            if (Load.Register < LiveScalar.Num() && LiveScalar[Load.Register])
            {
                OutPlan.ParameterLoads.Add(Load);
            }
        }
        for (const FAnimPlanLoad& Load : Full.ObjectParameterLoads)
        {
            if (Load.Register < LiveObject.Num() && LiveObject[Load.Register])
            {
                OutPlan.ObjectParameterLoads.Add(Load);
            }
        }
        for (const FAnimPlanLoad& Load : Full.ObjectConstantLoads)
        {
            if (Load.Register < LiveObject.Num() && LiveObject[Load.Register])
            {
                OutPlan.ObjectConstantLoads.Add(Load);
            }
        }
        if (OutFates != nullptr)
        {
            OutFates->SetNum(Ops.Num());
            for (int32 i = 0; i < Ops.Num(); ++i)
            {
                (*OutFates)[i] = !LiveOp[i] ? EAnimCrowdOpFate::Removed : Bypassed[i] ? EAnimCrowdOpFate::Bypassed : EAnimCrowdOpFate::Kept;
            }
        }
        CompactOps(Ops, LiveOp, OutPlan);
    }
}
