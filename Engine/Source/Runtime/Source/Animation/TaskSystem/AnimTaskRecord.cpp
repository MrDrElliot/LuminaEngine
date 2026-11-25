#include "RuntimePCH.h"
#include "AnimTaskRecord.h"

#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Containers/Algorithm.h"
#include "Memory/Memcpy.h"

namespace Lumina
{
    namespace
    {
        enum EPayloadField : uint8
        {
            PayloadTime  = 1 << 0,
            PayloadAlpha = 1 << 1,
            PayloadDelta = 1 << 2,
            PayloadFlags = 1 << 3,
            PayloadT     = 1 << 4,
            PayloadR     = 1 << 5,
        };

        // The fields an update rewrites every frame per task type; everything else is fixed by the path and lives in the topology.
        uint8 PayloadOf(EAnimTaskType Type)
        {
            switch (Type)
            {
            case EAnimTaskType::SampleClip:       return PayloadTime;
            case EAnimTaskType::Blend:
            case EAnimTaskType::BlendMasked:
            case EAnimTaskType::ApplyAdditive:    return PayloadAlpha;
            case EAnimTaskType::Inertialize:
            case EAnimTaskType::DeadBlend:        return PayloadTime | PayloadDelta | PayloadFlags;
            case EAnimTaskType::SavePoseSnapshot: return PayloadFlags;
            case EAnimTaskType::BoneTransform:    return PayloadAlpha | PayloadR;
            case EAnimTaskType::TwoBoneIK:
            case EAnimTaskType::FABRIK:
            case EAnimTaskType::TranslateBone:    return PayloadAlpha | PayloadT;
            case EAnimTaskType::LookAt:           return PayloadAlpha | PayloadT | PayloadTime;
            case EAnimTaskType::FootIK:           return PayloadAlpha | PayloadT | PayloadR | PayloadTime;
            default:                              return 0;
            }
        }

        enum EStateRefKind : int32
        {
            NoStateRef        = 0,
            MachineInertRef   = 1,
            NodeInertRef      = 2,
            DeadBlendRef      = 3,
            SnapshotRef       = 4,
        };

        constexpr int32 kStateRefShift = 24;

        template <typename T>
        int32 IndexIn(const T* Pointer, const TVector<T>& Owner)
        {
            if (Pointer == nullptr || Owner.IsEmpty() || Pointer < Owner.GetData() || Pointer >= Owner.GetData() + Owner.Num())
            {
                return Constants::kIndexNone;
            }
            return (int32)(Pointer - Owner.GetData());
        }

        int32 StateRefOf(const FAnimTask& Task, const FAnimGraphVMState& State)
        {
            if (const int32 Machine = IndexIn(Task.Inert, State.Inertializers); Machine != Constants::kIndexNone)
            {
                return (MachineInertRef << kStateRefShift) | Machine;
            }
            if (const int32 Node = IndexIn(Task.Inert, State.NodeInertializers); Node != Constants::kIndexNone)
            {
                return (NodeInertRef << kStateRefShift) | Node;
            }
            if (const int32 Dead = IndexIn(Task.Dead, State.DeadBlends); Dead != Constants::kIndexNone)
            {
                return (DeadBlendRef << kStateRefShift) | Dead;
            }
            if (const int32 Snapshot = IndexIn(Task.Snapshot, State.PoseSnapshots); Snapshot != Constants::kIndexNone)
            {
                return (SnapshotRef << kStateRefShift) | Snapshot;
            }
            return NoStateRef;
        }

        struct FTopologyHash
        {
            uint64 Value = 0x84222325CBF29CE4ull;

            FORCEINLINE void Mix(uint64 Word)
            {
                Value = (Value ^ Word) * 0x9E3779B97F4A7C15ull;
                Value ^= Value >> 29;
            }

            FORCEINLINE void MixFloat(float Value32)
            {
                uint32 Bits;
                Memory::Memcpy(&Bits, &Value32, sizeof(Bits));
                Mix(Bits);
            }

            FORCEINLINE void MixVector(const FVector3& Vector)
            {
                MixFloat(Vector.x);
                MixFloat(Vector.y);
                MixFloat(Vector.z);
            }

            FORCEINLINE void MixQuat(const FQuat& Quat)
            {
                MixFloat(Quat.x);
                MixFloat(Quat.y);
                MixFloat(Quat.z);
                MixFloat(Quat.w);
            }
        };

        FORCEINLINE float FlagsOf(const FAnimTask& Task)
        {
            return (float)((Task.bCapture ? 1 : 0) | (Task.bApply ? 2 : 0));
        }
    }

    namespace Anim
    {
        void AppendTaskPayload(const FAnimTask& Task, TVector<float>& Payload)
        {
            const uint8 Fields = PayloadOf(Task.Type);
            if (Fields & PayloadTime)  { Payload.Add(Task.Time); }
            if (Fields & PayloadAlpha) { Payload.Add(Task.Alpha); }
            if (Fields & PayloadDelta) { Payload.Add(Task.DeltaTime); }
            if (Fields & PayloadFlags) { Payload.Add(FlagsOf(Task)); }
            if (Fields & PayloadT)
            {
                Payload.Add(Task.T.x);
                Payload.Add(Task.T.y);
                Payload.Add(Task.T.z);
            }
            if (Fields & PayloadR)
            {
                Payload.Add(Task.R.x);
                Payload.Add(Task.R.y);
                Payload.Add(Task.R.z);
                Payload.Add(Task.R.w);
            }
        }

        void RecordTaskList(const FAnimTaskList& Tasks, const FAnimGraphVMState& State, FAnimTaskRecord& Out)
        {
            Out.Payload.Reset();
            FTopologyHash Hash;
            Hash.Mix((uint64)(uint16)Tasks.OutputTask | ((uint64)Tasks.bLockRoot << 16) | ((uint64)(uint32)Tasks.RootBoneIndex << 32));
            Hash.Mix((uint64)Tasks.Tasks.Num());
            Hash.Mix((uint64)(uintptr_t)Tasks.Skeleton);

            for (const FAnimTask& Task : Tasks.Tasks)
            {
                const uint8 Payload = PayloadOf(Task.Type);
                Hash.Mix((uint64)Task.Type | ((uint64)Task.Stage << 8) | ((uint64)(uint16)Task.DepA << 16) | ((uint64)(uint16)Task.DepB << 32)
                         | ((uint64)Task.AdditiveSpace << 48) | ((uint64)Task.Space << 56));
                Hash.Mix((uint64)Task.Mode | ((uint64)Task.BoneA << 16) | ((uint64)Task.BoneB << 32) | ((uint64)Task.BoneC << 48));
                Hash.Mix((uint64)(uintptr_t)Task.Clip);
                Hash.Mix((uint64)(uintptr_t)Task.MaskWeights);
                Hash.Mix((uint64)(uint32)StateRefOf(Task, State));
                Hash.MixVector(Task.S);

                if (!(Payload & PayloadTime))  { Hash.MixFloat(Task.Time); }
                if (!(Payload & PayloadAlpha)) { Hash.MixFloat(Task.Alpha); }
                if (!(Payload & PayloadDelta)) { Hash.MixFloat(Task.DeltaTime); }
                if (!(Payload & PayloadFlags)) { Hash.MixFloat(FlagsOf(Task)); }
                if (!(Payload & PayloadT))     { Hash.MixVector(Task.T); }
                if (!(Payload & PayloadR))     { Hash.MixQuat(Task.R); }
                AppendTaskPayload(Task, Out.Payload);
            }
            Out.TopologyKey = Hash.Value;
        }

        void MakeTopology(const FAnimTaskList& Tasks, const FAnimGraphVMState& State, FAnimTopology& Out)
        {
            Out.Tasks         = Tasks.Tasks;
            Out.OutputTask    = Tasks.OutputTask;
            Out.bLockRoot     = Tasks.bLockRoot;
            Out.RootBoneIndex = Tasks.RootBoneIndex;
            Out.StateRefs.Reset();
            Out.PayloadOffsets.Reset();
            Out.PayloadFloats = 0;

            for (FAnimTask& Task : Out.Tasks)
            {
                Out.StateRefs.Add(StateRefOf(Task, State));
                Out.PayloadOffsets.Add(Out.PayloadFloats);
                Task.Inert    = nullptr;
                Task.Dead     = nullptr;
                Task.Snapshot = nullptr;

                const uint8 Payload = PayloadOf(Task.Type);
                Out.PayloadFloats += ((Payload & PayloadTime) ? 1 : 0) + ((Payload & PayloadAlpha) ? 1 : 0) + ((Payload & PayloadDelta) ? 1 : 0)
                    + ((Payload & PayloadFlags) ? 1 : 0) + ((Payload & PayloadT) ? 3 : 0) + ((Payload & PayloadR) ? 4 : 0);
            }
        }

        void ExpandTaskList(const FAnimTopology& Topology, const FAnimTaskRecord& Record, FAnimGraphVMState& State, FAnimTaskList& Out)
        {
            Out.Tasks         = Topology.Tasks;
            Out.OutputTask    = Topology.OutputTask;
            Out.bLockRoot     = Topology.bLockRoot;
            Out.RootBoneIndex = Topology.RootBoneIndex;

            const float* Values = Record.Payload.GetData();
            for (int32 t = 0; t < Out.Tasks.Num(); ++t)
            {
                FAnimTask& Task = Out.Tasks[t];
                const int32 Ref   = Topology.StateRefs[t];
                const int32 Index = Ref & ((1 << kStateRefShift) - 1);
                switch (Ref >> kStateRefShift)
                {
                case MachineInertRef: Task.Inert    = &State.Inertializers[Index]; break;
                case NodeInertRef:    Task.Inert    = &State.NodeInertializers[Index]; break;
                case DeadBlendRef:    Task.Dead     = &State.DeadBlends[Index]; break;
                case SnapshotRef:     Task.Snapshot = &State.PoseSnapshots[Index]; break;
                default: break;
                }

                const uint8 Payload = PayloadOf(Task.Type);
                if (Payload & PayloadTime)  { Task.Time      = *Values++; }
                if (Payload & PayloadAlpha) { Task.Alpha     = *Values++; }
                if (Payload & PayloadDelta) { Task.DeltaTime = *Values++; }
                if (Payload & PayloadFlags)
                {
                    const int32 Flags = (int32)*Values++;
                    Task.bCapture = (Flags & 1) != 0;
                    Task.bApply   = (Flags & 2) != 0;
                }
                if (Payload & PayloadT)
                {
                    Task.T = FVector3(Values[0], Values[1], Values[2]);
                    Values += 3;
                }
                if (Payload & PayloadR)
                {
                    Task.R.x = Values[0];
                    Task.R.y = Values[1];
                    Task.R.z = Values[2];
                    Task.R.w = Values[3];
                    Values += 4;
                }
            }
        }

        void MakeSampleProgram(const FAnimTopology& Topology, FAnimSampleProgram& Out)
        {
            Out = FAnimSampleProgram();
            Out.bExpressible = Topology.OutputTask != FAnimTask::NoTask;

            // Payload fields keep AppendTaskPayload's order, so a blend's alpha and an inertializer's elapsed time lead its payload.
            constexpr int32 kFlagsAfterElapsed = 2;

            TVector<FAnimSampleProgram::FFactor> Path;
            const auto Walk = [&](auto& Self, int16 TaskIndex) -> void
            {
                if (!Out.bExpressible || TaskIndex < 0 || TaskIndex >= Topology.Tasks.Num())
                {
                    Out.bExpressible = false;
                    return;
                }
                const FAnimTask& Task = Topology.Tasks[TaskIndex];
                const int32 Payload = Topology.PayloadOffsets[TaskIndex];
                switch (Task.Type)
                {
                case EAnimTaskType::SampleClip:
                case EAnimTaskType::ReferencePose:
                {
                    // A delta pose only means something added onto a base, and the GPU keeps no scale, so neither fits a weighted sum.
                    if (Task.Clip != nullptr && (Task.Clip->IsAdditive() || Task.Clip->ScalesBones()))
                    {
                        Out.bExpressible = false;
                        return;
                    }
                    FAnimSampleProgram::FSample& Sample = Out.Samples.EmplaceGetRef();
                    Sample.Clip        = Task.Type == EAnimTaskType::SampleClip ? Task.Clip : nullptr;
                    Sample.TimeFloat   = Task.Type == EAnimTaskType::SampleClip ? Payload : Constants::kIndexNone;
                    Sample.FirstFactor = Out.Factors.Num();
                    Sample.NumFactors  = Path.Num();
                    Out.Factors.Append(Path);
                    break;
                }
                case EAnimTaskType::Blend:
                    Path.Add({ Payload, false });
                    Self(Self, Task.DepA);
                    Path.Last().bSecondInput = true;
                    Self(Self, Task.DepB);
                    Path.Pop();
                    break;
                case EAnimTaskType::Inertialize:
                {
                    // A seam a cached pose reaches by two paths is still one seam.
                    const int32 StateRef = Topology.StateRefs[TaskIndex];
                    if (!Algo::AnyOf(Out.Transitions, [StateRef](const FAnimSampleProgram::FTransition& Seen) { return Seen.StateRef == StateRef; }))
                    {
                        Out.Transitions.Add({ Payload, Payload + kFlagsAfterElapsed, StateRef });
                    }
                    Self(Self, Task.DepA);
                    break;
                }
                default:
                    Out.bExpressible = false;
                    break;
                }
            };
            Walk(Walk, Topology.OutputTask);
        }

        void EvaluateSampleProgram(const FAnimSampleProgram& Program, const FAnimTaskRecord& Record, TVector<FAnimWeightedSample>& Out)
        {
            Out.Reset();
            const float* Payload = Record.Payload.GetData();
            for (const FAnimSampleProgram::FSample& Sample : Program.Samples)
            {
                // The executor takes a blend at or past either end as that input alone, so alphas clamp before weighting.
                float Weight = 1.0f;
                for (int32 f = Sample.FirstFactor; f < Sample.FirstFactor + Sample.NumFactors; ++f)
                {
                    const FAnimSampleProgram::FFactor& Factor = Program.Factors[f];
                    const float Alpha = Math::Clamp(Payload[Factor.AlphaFloat], 0.0f, 1.0f);
                    Weight *= Factor.bSecondInput ? Alpha : 1.0f - Alpha;
                }
                if (Weight > 0.0f)
                {
                    Out.Add({ Sample.Clip, Sample.TimeFloat >= 0 ? Payload[Sample.TimeFloat] : 0.0f, Weight });
                }
            }
        }
    }

    const FAnimTopologyEntry* FAnimTopologyCache::Find(uint64 Key) const
    {
        const auto Read = Entries.Read();
        const TUniquePtr<FAnimTopologyEntry>* Entry = Read->Find(Key);
        return Entry != nullptr ? Entry->Get() : nullptr;
    }

    const FAnimTopologyEntry& FAnimTopologyCache::FindOrAdd(uint64 Key, const FAnimTaskList& Tasks, const FAnimGraphVMState& State)
    {
        if (const FAnimTopologyEntry* Existing = Find(Key))
        {
            return *Existing;
        }

        // Built outside the lock, since two workers reaching one new path at once only waste a build.
        TUniquePtr<FAnimTopologyEntry> Built = MakeUnique<FAnimTopologyEntry>();
        Anim::MakeTopology(Tasks, State, Built->Topology);
        Anim::MakeSampleProgram(Built->Topology, Built->Program);

        auto Write = Entries.Write();
        TUniquePtr<FAnimTopologyEntry>& Slot = Write->FindOrAdd(Key);
        if (!Slot.IsValid())
        {
            Slot = Move(Built);
        }
        return *Slot;
    }

    int32 FAnimTopologyCache::Num() const
    {
        return (int32)Entries.Read()->Num();
    }
}
