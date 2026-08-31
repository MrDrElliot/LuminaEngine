#include "RuntimePCH.h"
#include "ParticleSystemComponent.h"

#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "World/Entity/Components/MeshComponent.h"

namespace Lumina
{
    CMaterialInterface* SParticleSystemComponent::GetMaterialForEmitter(int32 EmitterIndex) const
    {
        if (EmitterIndex < 0)
        {
            return nullptr;
        }

        if ((size_t)EmitterIndex < MaterialOverrides.size())
        {
            if (CMaterialInterface* Override = MaterialOverrides[(size_t)EmitterIndex])
            {
                return Override;
            }
        }

        CParticleSystem* System = ParticleSystem.Get();
        if (System == nullptr || EmitterIndex >= (int32)System->Emitters.size())
        {
            return nullptr;
        }

        CParticleEmitter* Emitter = System->Emitters[(size_t)EmitterIndex].Get();
        return Emitter != nullptr ? Emitter->Material.Get() : nullptr;
    }

    void SParticleSystemComponent::SetMaterialForEmitter(CMaterialInterface* Material, int32 EmitterIndex)
    {
        if (EmitterIndex < 0)
        {
            return;
        }

        // Grown to cover the index rather than appended, so the material lands on the emitter asked for.
        if (MaterialOverrides.size() <= (size_t)EmitterIndex)
        {
            MaterialOverrides.resize((size_t)EmitterIndex + 1);
        }

        MaterialOverrides[(size_t)EmitterIndex] = Material;
    }

    CMaterialInstance* SParticleSystemComponent::CreateDynamicMaterialInstance(int32 EmitterIndex)
    {
        CMaterialInstance* Dynamic = MeshComponentUtils::MakeDynamicMaterialInstance(GetMaterialForEmitter(EmitterIndex));
        if (Dynamic != nullptr)
        {
            SetMaterialForEmitter(Dynamic, EmitterIndex);
        }
        return Dynamic;
    }

    static FVector4 PromoteToVec4(const FVector2& V) { return FVector4(V, 0.0f, 0.0f); }
    static FVector4 PromoteToVec4(const FVector3& V) { return FVector4(V, 0.0f); }
    [[maybe_unused]] static FVector4 PromoteToVec4(const FVector4& V) { return V; }

    const FParticleParameter* SParticleSystemComponent::FindParameter(const FName& Name) const
    {
        if (Name.IsNone())
        {
            return nullptr;
        }

        for (const FParticleParameter& Override : ParameterOverrides)
        {
            if (Override.Name == Name)
            {
                return &Override;
            }
        }

        if (ParticleSystem)
        {
            return ParticleSystem->FindUserParameter(Name);
        }

        return nullptr;
    }

    FParticleParameter* SParticleSystemComponent::GetOrCreateOverride(FName Name, EParticleParameterType ExpectedType)
    {
        if (Name.IsNone())
        {
            return nullptr;
        }

        for (FParticleParameter& Override : ParameterOverrides)
        {
            if (Override.Name == Name)
            {
                if (Override.Type != ExpectedType)
                {
                    return nullptr;
                }
                return &Override;
            }
        }

        if (ParticleSystem)
        {
            const FParticleParameter* AssetParam = ParticleSystem->FindUserParameter(Name);
            if (AssetParam && AssetParam->Type != ExpectedType)
            {
                return nullptr;
            }
        }

        FParticleParameter NewOverride;
        NewOverride.Name = Name;
        NewOverride.Type = ExpectedType;
        ParameterOverrides.push_back(NewOverride);
        return &ParameterOverrides.back();
    }

    float SParticleSystemComponent::GetFloat(const FName& Name, float Default) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Float) ? Param->Scalar : Default;
    }

    int32 SParticleSystemComponent::GetInt(const FName& Name, int32 Default) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Int) ? Param->Integer : Default;
    }

    bool SParticleSystemComponent::GetBool(const FName& Name, bool Default) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Bool) ? Param->Boolean : Default;
    }

    FVector2 SParticleSystemComponent::GetVec2(const FName& Name) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Vec2) ? FVector2(Param->Vector) : FVector2(0.0f);
    }

    FVector3 SParticleSystemComponent::GetVec3(const FName& Name) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Vec3) ? FVector3(Param->Vector) : FVector3(0.0f);
    }

    FVector4 SParticleSystemComponent::GetVec4(const FName& Name) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Vec4) ? Param->Vector : FVector4(0.0f);
    }

    FVector4 SParticleSystemComponent::GetColor(const FName& Name) const
    {
        const FParticleParameter* Param = FindParameter(Name);
        return (Param && Param->Type == EParticleParameterType::Color) ? Param->Vector : FVector4(0.0f);
    }

    void SParticleSystemComponent::SetFloat(const FName& Name, float Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Float))
        {
            P->Scalar = Value;
        }
    }

    void SParticleSystemComponent::SetInt(const FName& Name, int32 Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Int))
        {
            P->Integer = Value;
        }
    }

    void SParticleSystemComponent::SetBool(const FName& Name, bool Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Bool))
        {
            P->Boolean = Value;
        }
    }

    void SParticleSystemComponent::SetVec2(const FName& Name, FVector2 Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Vec2))
        {
            P->Vector = PromoteToVec4(Value);
        }
    }

    void SParticleSystemComponent::SetVec3(const FName& Name, FVector3 Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Vec3))
        {
            P->Vector = PromoteToVec4(Value);
        }
    }

    void SParticleSystemComponent::SetVec4(const FName& Name, FVector4 Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Vec4))
        {
            P->Vector = Value;
        }
    }

    void SParticleSystemComponent::SetColor(const FName& Name, FVector4 Value)
    {
        if (FParticleParameter* P = GetOrCreateOverride(Name, EParticleParameterType::Color))
        {
            P->Vector = Value;
        }
    }

    void SParticleSystemComponent::ResetParameter(const FName& Name)
    {
        for (auto It = ParameterOverrides.begin(); It != ParameterOverrides.end(); ++It)
        {
            if (It->Name == Name)
            {
                ParameterOverrides.erase(It);
                return;
            }
        }
    }
}
