#include "Platform/GenericPlatform.h"
#include "World/ECS/Registry.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "World/World.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

// Two backends share one facade, and every call but Play is a no-op when the component is absent.

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    // Parameters live in the graph component's own struct instance; a name resolves to a field on it.
    void* ResolveParameterField(CWorld* World, ECS::FEntity Entity, const FName& Name, EAnimParamValueType& OutType)
    {
        OutType = EAnimParamValueType::Unresolved;

        SAnimationGraphComponent* Comp = World->TryGetComponent<SAnimationGraphComponent>(Entity);
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
}

LUMINA_DOTNET_EXPORT(void, Animation_Play)(uint64 World, uint32 Entity, void* AnimationPtr, int32 bLoop, float Speed)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    CAnimation* Clip = static_cast<CAnimation*>(AnimationPtr);
    SSimpleAnimationComponent& Comp = W->GetOrEmplaceComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    Comp.PlayAnimation(Clip, bLoop != 0, Speed);
}

LUMINA_DOTNET_EXPORT(void, Animation_Stop)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Stop();
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_Pause)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Pause();
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_Resume)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Resume();
    }
}

LUMINA_DOTNET_EXPORT(int32, Animation_IsPlaying)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->IsPlaying()) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_IsFinished)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->IsFinished()) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, Animation_SetSpeed)(uint64 World, uint32 Entity, float Speed)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->PlaybackSpeed = Speed;
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_SetTime)(uint64 World, uint32 Entity, float Time)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->CurrentTime = Time;
        Comp->bDirty = true;
    }
}

LUMINA_DOTNET_EXPORT(float, Animation_GetTime)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0.0f;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->CurrentTime : 0.0f;
}

//~ Graph parameters (SAnimationGraphComponent). Names cross as UTF-8 (char*, len).

LUMINA_DOTNET_EXPORT(void, Animation_SetFloat)(uint64 World, uint32 Entity, const char* Name, int32 Length, float Value)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return;
    }
    EAnimParamValueType Type;
    if (void* Field = ResolveParameterField(W, AsEntity(Entity), FName(FStringView(Name, (size_t)Length)), Type))
    {
        WriteParameterScalar(Field, Type, Value);
    }
}

LUMINA_DOTNET_EXPORT(float, Animation_GetFloat)(uint64 World, uint32 Entity, const char* Name, int32 Length, float Default)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return Default;
    }
    EAnimParamValueType Type;
    void* Field = ResolveParameterField(W, AsEntity(Entity), FName(FStringView(Name, (size_t)Length)), Type);
    if (Field == nullptr)
    {
        return Default;
    }

    FAnimGraphParamBinding Binding;
    Binding.Offset = 0;
    Binding.Type   = Type;
    return ReadAnimParamScalar(static_cast<const uint8*>(Field), Binding);
}

LUMINA_DOTNET_EXPORT(void, Animation_SetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bValue)
{
    LuminaSharp_Animation_SetFloat(World, Entity, Name, Length, bValue != 0 ? 1.0f : 0.0f);
}

LUMINA_DOTNET_EXPORT(int32, Animation_GetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bDefault)
{
    return LuminaSharp_Animation_GetFloat(World, Entity, Name, Length, bDefault != 0 ? 1.0f : 0.0f) != 0.0f ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_HasParameter)(uint64 World, uint32 Entity, const char* Name, int32 Length)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return 0;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->HasParameter(FName(FStringView(Name, (size_t)Length)))) ? 1 : 0;
}

// The graph must contain a matching Slot node for anything to show.

LUMINA_DOTNET_EXPORT(uint32, Animation_PlayMontage)(uint64 World, uint32 Entity, void* MontagePtr, float PlayRate,
                                                    const char* Section, int32 Length)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr)
    {
        return 0;
    }
    CAnimationMontage* Montage = static_cast<CAnimationMontage*>(MontagePtr);
    SAnimationGraphComponent& Comp = W->GetOrEmplaceComponent<SAnimationGraphComponent>(AsEntity(Entity));
    const FName StartSection = (Section != nullptr && Length > 0) ? FName(FStringView(Section, (size_t)Length)) : FName();
    return Comp.Montages.Play(Montage, PlayRate, StartSection);
}

LUMINA_DOTNET_EXPORT(void, Animation_StopMontage)(uint64 World, uint32 Entity, void* MontagePtr, float BlendOutTime)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        if (MontagePtr != nullptr)
        {
            Comp->Montages.Stop(static_cast<CAnimationMontage*>(MontagePtr), BlendOutTime);
        }
        else
        {
            Comp->Montages.StopAll(BlendOutTime);
        }
    }
}

LUMINA_DOTNET_EXPORT(int32, Animation_JumpToMontageSection)(uint64 World, uint32 Entity, void* MontagePtr,
                                                            const char* Section, int32 Length)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr || Section == nullptr)
    {
        return 0;
    }
    SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return 0;
    }
    return Comp->Montages.JumpToSection(static_cast<CAnimationMontage*>(MontagePtr),
                                        FName(FStringView(Section, (size_t)Length))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_SetNextMontageSection)(uint64 World, uint32 Entity, void* MontagePtr,
                                                             const char* Section, int32 Length)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr || Section == nullptr)
    {
        return 0;
    }
    SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return 0;
    }
    return Comp->Montages.SetNextSection(static_cast<CAnimationMontage*>(MontagePtr),
                                         FName(FStringView(Section, (size_t)Length))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_IsMontagePlaying)(uint64 World, uint32 Entity, void* MontagePtr)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return 0;
    }
    if (MontagePtr == nullptr)
    {
        return Comp->Montages.HasActive() ? 1 : 0;
    }
    return Comp->Montages.IsPlaying(static_cast<CAnimationMontage*>(MontagePtr)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(float, Animation_GetMontagePosition)(uint64 World, uint32 Entity, void* MontagePtr)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr)
    {
        return 0.0f;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->Montages.GetPosition(static_cast<CAnimationMontage*>(MontagePtr)) : 0.0f;
}

LUMINA_DOTNET_EXPORT(float, Animation_GetMontageWeight)(uint64 World, uint32 Entity, void* MontagePtr)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr)
    {
        return 0.0f;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->Montages.GetWeight(static_cast<CAnimationMontage*>(MontagePtr)) : 0.0f;
}

LUMINA_DOTNET_EXPORT(void, Animation_SetMontagePlayRate)(uint64 World, uint32 Entity, void* MontagePtr, float PlayRate)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr)
    {
        return;
    }
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        Comp->Montages.SetPlayRate(static_cast<CAnimationMontage*>(MontagePtr), PlayRate);
    }
}

// A two-pass string return, sizing with a null buffer then filling, returning the name length.
LUMINA_DOTNET_EXPORT(int32, Animation_GetMontageSection)(uint64 World, uint32 Entity, void* MontagePtr,
                                                         char* Buffer, int32 Capacity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || MontagePtr == nullptr)
    {
        return 0;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return 0;
    }

    const FName Section = Comp->Montages.GetCurrentSection(static_cast<CAnimationMontage*>(MontagePtr));
    const FStringView Name(Section.c_str());
    const int32 Len = (int32)Name.size();
    if (Buffer != nullptr && Capacity > 0)
    {
        const int32 N = Len < Capacity ? Len : Capacity;
        for (int32 i = 0; i < N; ++i)
        {
            Buffer[i] = Name[(size_t)i];
        }
    }
    return Len;
}

// Name-checked rather than trusted, since the offsets C# reads come from its own mirror.
LUMINA_DOTNET_EXPORT(void*, AnimGraph_GetParameterMemory)(void* Component, const char* TypeName, int32 Length)
{
    auto* Comp = static_cast<SAnimationGraphComponent*>(Component);
    if (Comp == nullptr || TypeName == nullptr || !Comp->Graph.IsValid())
    {
        return nullptr;
    }

    CStruct* Struct = Comp->Graph->GetParameterStruct();
    if (Struct == nullptr || Struct->GetName() != FName(FStringView(TypeName, (size_t)Length)))
    {
        return nullptr;
    }

    return Comp->GetParameterMemory();
}
