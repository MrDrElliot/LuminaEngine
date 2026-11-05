#include "RuntimePCH.h"

#include "World/Subsystems/AnimationSubsystem.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        // Parameters live in the graph component's own struct instance, so a name resolves to a field on it.
        void* ResolveParameterField(CWorld* World, ECS::FEntity Entity, const FName& Name,
            EAnimParamValueType& OutType)
        {
            OutType = EAnimParamValueType::Unresolved;
            if (World == nullptr)
            {
                return nullptr;
            }

            SAnimationGraphComponent* Comp = World->TryGetComponent<ECS::Write<SAnimationGraphComponent>>(Entity);
            if (Comp == nullptr || !Comp->Graph.IsValid())
            {
                return nullptr;
            }

            CStruct* Struct = Comp->Graph->GetParameterStruct();
            uint8* Base = static_cast<uint8*>(Comp->GetParameterMemory());
            if (Struct == nullptr || Base == nullptr)
            {
                return nullptr;
            }

            FProperty* Property = Struct->GetProperty(Name);
            if (Property == nullptr || Property->HasSetterOrGetter())
            {
                return nullptr;
            }

            OutType = AnimParamValueTypeFromProperty(Property);
            return OutType == EAnimParamValueType::Unresolved ? nullptr : Base + Property->Offset;
        }

        void WriteParameterScalar(void* Field, EAnimParamValueType Type, float Value)
        {
            switch (Type)
            {
            case EAnimParamValueType::Float:  *static_cast<float*>(Field)  = Value; break;
            case EAnimParamValueType::Double: *static_cast<double*>(Field) = (double)Value; break;
            case EAnimParamValueType::Bool:   *static_cast<bool*>(Field)   = Value != 0.0f; break;
            case EAnimParamValueType::Int32:  *static_cast<int32*>(Field)  = (int32)Value; break;
            case EAnimParamValueType::UInt32: *static_cast<uint32*>(Field) = (uint32)Value; break;
            case EAnimParamValueType::Int64:  *static_cast<int64*>(Field)  = (int64)Value; break;
            case EAnimParamValueType::UInt64: *static_cast<uint64*>(Field) = (uint64)Value; break;
            case EAnimParamValueType::Int16:  *static_cast<int16*>(Field)  = (int16)Value; break;
            case EAnimParamValueType::UInt16: *static_cast<uint16*>(Field) = (uint16)Value; break;
            case EAnimParamValueType::Int8:   *static_cast<int8*>(Field)   = (int8)Value; break;
            case EAnimParamValueType::UInt8:  *static_cast<uint8*>(Field)  = (uint8)Value; break;
            default: break;
            }
        }

        SSimpleAnimationComponent* SimpleOf(CWorld* World, ECS::FEntity Entity)
        {
            return World != nullptr ? World->TryGetComponent<ECS::Write<SSimpleAnimationComponent>>(Entity) : nullptr;
        }

        const SSimpleAnimationComponent* ReadSimpleOf(CWorld* World, ECS::FEntity Entity)
        {
            return World != nullptr ? World->TryGetComponent<ECS::Read<SSimpleAnimationComponent>>(Entity) : nullptr;
        }

        SAnimationGraphComponent* GraphOf(CWorld* World, ECS::FEntity Entity)
        {
            return World != nullptr ? World->TryGetComponent<ECS::Write<SAnimationGraphComponent>>(Entity) : nullptr;
        }

        const SAnimationGraphComponent* ReadGraphOf(CWorld* World, ECS::FEntity Entity)
        {
            return World != nullptr ? World->TryGetComponent<ECS::Read<SAnimationGraphComponent>>(Entity) : nullptr;
        }
    }

    void CAnimationSubsystem::Play(ECS::FEntity Entity, CAnimation* Clip, bool bLoop, float Speed)
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }
        SSimpleAnimationComponent& Comp = World->GetOrEmplaceComponent<SSimpleAnimationComponent>(Entity);
        Comp.PlayAnimation(Clip, bLoop, Speed);
    }

    void CAnimationSubsystem::Stop(ECS::FEntity Entity)
    {
        if (SSimpleAnimationComponent* Comp = SimpleOf(GetWorld(), Entity)) { Comp->Stop(); }
    }

    void CAnimationSubsystem::Pause(ECS::FEntity Entity)
    {
        if (SSimpleAnimationComponent* Comp = SimpleOf(GetWorld(), Entity)) { Comp->Pause(); }
    }

    void CAnimationSubsystem::Resume(ECS::FEntity Entity)
    {
        if (SSimpleAnimationComponent* Comp = SimpleOf(GetWorld(), Entity)) { Comp->Resume(); }
    }

    bool CAnimationSubsystem::IsPlaying(ECS::FEntity Entity)
    {
        const SSimpleAnimationComponent* Comp = ReadSimpleOf(GetWorld(), Entity);
        return Comp != nullptr && Comp->IsPlaying();
    }

    bool CAnimationSubsystem::IsFinished(ECS::FEntity Entity)
    {
        const SSimpleAnimationComponent* Comp = ReadSimpleOf(GetWorld(), Entity);
        return Comp != nullptr && Comp->IsFinished();
    }

    void CAnimationSubsystem::SetSpeed(ECS::FEntity Entity, float Speed)
    {
        if (SSimpleAnimationComponent* Comp = SimpleOf(GetWorld(), Entity)) { Comp->PlaybackSpeed = Speed; }
    }

    void CAnimationSubsystem::SetTime(ECS::FEntity Entity, float Time)
    {
        if (SSimpleAnimationComponent* Comp = SimpleOf(GetWorld(), Entity))
        {
            Comp->CurrentTime = Time;
            Comp->bDirty = true;
        }
    }

    float CAnimationSubsystem::GetTime(ECS::FEntity Entity)
    {
        const SSimpleAnimationComponent* Comp = ReadSimpleOf(GetWorld(), Entity);
        return Comp != nullptr ? Comp->CurrentTime : 0.0f;
    }

    void CAnimationSubsystem::SetFloat(ECS::FEntity Entity, const FName& Name, float Value)
    {
        EAnimParamValueType Type;
        if (void* Field = ResolveParameterField(GetWorld(), Entity, Name, Type))
        {
            WriteParameterScalar(Field, Type, Value);
        }
    }

    float CAnimationSubsystem::GetFloat(ECS::FEntity Entity, const FName& Name, float Default)
    {
        EAnimParamValueType Type;
        void* Field = ResolveParameterField(GetWorld(), Entity, Name, Type);
        if (Field == nullptr)
        {
            return Default;
        }

        FAnimGraphParamBinding Binding;
        Binding.Offset = 0;
        Binding.Type = Type;
        return ReadAnimParamScalar(static_cast<const uint8*>(Field), Binding);
    }

    void CAnimationSubsystem::SetBool(ECS::FEntity Entity, const FName& Name, bool bValue)
    {
        CWorld* World = GetWorld();
        SetFloat(Entity, Name, bValue ? 1.0f : 0.0f);
    }

    bool CAnimationSubsystem::GetBool(ECS::FEntity Entity, const FName& Name, bool bDefault)
    {
        CWorld* World = GetWorld();
        return GetFloat(Entity, Name, bDefault ? 1.0f : 0.0f) != 0.0f;
    }

    bool CAnimationSubsystem::HasParameter(ECS::FEntity Entity, const FName& Name)
    {
        const SAnimationGraphComponent* Comp = ReadGraphOf(GetWorld(), Entity);
        return Comp != nullptr && Comp->HasParameter(Name);
    }

    uint32 CAnimationSubsystem::PlayMontage(ECS::FEntity Entity, CAnimationMontage* Montage,
        float PlayRate, const FName& Section)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || Montage == nullptr)
        {
            return 0;
        }
        SAnimationGraphComponent& Comp = World->GetOrEmplaceComponent<SAnimationGraphComponent>(Entity);
        return Comp.Montages.Play(Montage, PlayRate, Section);
    }

    void CAnimationSubsystem::StopMontage(ECS::FEntity Entity, CAnimationMontage* Montage,
        float BlendOutTime)
    {
        SAnimationGraphComponent* Comp = GraphOf(GetWorld(), Entity);
        if (Comp == nullptr)
        {
            return;
        }
        if (Montage != nullptr)
        {
            Comp->Montages.Stop(Montage, BlendOutTime);
        }
        else
        {
            Comp->Montages.StopAll(BlendOutTime);
        }
    }

    bool CAnimationSubsystem::JumpToMontageSection(ECS::FEntity Entity, CAnimationMontage* Montage,
        const FName& Section)
    {
        SAnimationGraphComponent* Comp = GraphOf(GetWorld(), Entity);
        return Comp != nullptr && Montage != nullptr && Comp->Montages.JumpToSection(Montage, Section);
    }

    bool CAnimationSubsystem::SetNextMontageSection(ECS::FEntity Entity, CAnimationMontage* Montage,
        const FName& Section)
    {
        SAnimationGraphComponent* Comp = GraphOf(GetWorld(), Entity);
        return Comp != nullptr && Montage != nullptr && Comp->Montages.SetNextSection(Montage, Section);
    }

    bool CAnimationSubsystem::IsMontagePlaying(ECS::FEntity Entity, CAnimationMontage* Montage)
    {
        const SAnimationGraphComponent* Comp = ReadGraphOf(GetWorld(), Entity);
        if (Comp == nullptr)
        {
            return false;
        }
        return Montage != nullptr ? Comp->Montages.IsPlaying(Montage) : Comp->Montages.HasActive();
    }

    float CAnimationSubsystem::GetMontagePosition(ECS::FEntity Entity, CAnimationMontage* Montage)
    {
        const SAnimationGraphComponent* Comp = ReadGraphOf(GetWorld(), Entity);
        return (Comp != nullptr && Montage != nullptr) ? Comp->Montages.GetPosition(Montage) : 0.0f;
    }

    float CAnimationSubsystem::GetMontageWeight(ECS::FEntity Entity, CAnimationMontage* Montage)
    {
        const SAnimationGraphComponent* Comp = ReadGraphOf(GetWorld(), Entity);
        return (Comp != nullptr && Montage != nullptr) ? Comp->Montages.GetWeight(Montage) : 0.0f;
    }

    void CAnimationSubsystem::SetMontagePlayRate(ECS::FEntity Entity, CAnimationMontage* Montage,
        float PlayRate)
    {
        SAnimationGraphComponent* Comp = GraphOf(GetWorld(), Entity);
        if (Comp != nullptr && Montage != nullptr)
        {
            Comp->Montages.SetPlayRate(Montage, PlayRate);
        }
    }

    FName CAnimationSubsystem::GetMontageSection(ECS::FEntity Entity, CAnimationMontage* Montage)
    {
        const SAnimationGraphComponent* Comp = ReadGraphOf(GetWorld(), Entity);
        return (Comp != nullptr && Montage != nullptr) ? Comp->Montages.GetCurrentSection(Montage) : FName();
    }
}
