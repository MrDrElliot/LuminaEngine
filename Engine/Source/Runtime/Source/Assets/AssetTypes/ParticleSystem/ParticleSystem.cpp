#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "ParticleSystem.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    bool FParticleParameter::Serialize(FArchive& Ar)
    {
        Ar << Name;
        Ar << Type;

        switch (Type)
        {
        case EParticleParameterType::Float: Ar << Scalar;  break;
        case EParticleParameterType::Int:   Ar << Integer; break;
        case EParticleParameterType::Bool:  Ar << Boolean; break;
        case EParticleParameterType::Vec2:
        case EParticleParameterType::Vec3:
        case EParticleParameterType::Vec4:
        case EParticleParameterType::Color: Ar << Vector;  break;
        }

        return true;
    }

    CMaterialInterface* ResolveParticleSpriteMaterial(CMaterialInterface* Candidate)
    {
        if (Candidate == nullptr || !Candidate->IsUsableInDomain(EMaterialType::Particle))
        {
            return nullptr;
        }

        // Without a table slot the shaders would read another material's uniforms.
        return Candidate->GetMaterialIndex() >= 0 ? Candidate : nullptr;
    }

    CMaterialInterface* CParticleEmitter::GetRenderMaterial() const
    {
        return ResolveParticleSpriteMaterial(Material.Get());
    }

    void CParticleEmitter::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Particles");
        if (!ComputeShaderBinaries.empty())
        {
            ComputeShader = FShaderLibrary::Commit(FName((GetGUID().ToString() + "_CS").c_str()), ERHIShaderType::Compute,
                TSpan<const uint32>(ComputeShaderBinaries.data(), ComputeShaderBinaries.size()));
        }
    }

    void CParticleEmitter::OnDestroy()
    {
        CObject::OnDestroy();
        ComputeShader = {};
    }

    void CParticleSystem::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Particles");
        CObject::Serialize(Ar);
    }

    void CParticleSystem::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Particles");
        // Emitters is the only thing every consumer indexes, so an empty one gets a default instead.
        if (Emitters.empty())
        {
            AddEmitter();
        }

        // PostLoad commits their shader binaries, and a nested export gets none from the outer load.
        for (const TObjectPtr<CParticleEmitter>& Emitter : Emitters)
        {
            if (Emitter.IsValid())
            {
                Emitter->PostLoad();
            }
        }
    }

    CParticleEmitter* CParticleSystem::AddEmitter()
    {
        CParticleEmitter* Emitter = NewObject<CParticleEmitter>(GetPackage());
        if (Emitter == nullptr)
        {
            return nullptr;
        }

        // Names only disambiguate column headers, so a running index skipping taken ones is enough.
        FString Candidate;
        for (int32 Suffix = (int32)Emitters.size(); ; ++Suffix)
        {
            Candidate = FString("Emitter ") + Format("{}", Suffix).c_str();
            bool bTaken = false;
            for (const TObjectPtr<CParticleEmitter>& Existing : Emitters)
            {
                if (Existing.IsValid() && Existing->EmitterName == Candidate)
                {
                    bTaken = true;
                    break;
                }
            }
            if (!bTaken)
            {
                break;
            }
        }
        Emitter->EmitterName = Candidate;

        Emitters.push_back(Emitter);
        return Emitter;
    }

    bool CParticleSystem::RemoveEmitter(CParticleEmitter* Emitter)
    {
        if (Emitter == nullptr || Emitters.size() <= 1)
        {
            return false;
        }

        for (auto It = Emitters.begin(); It != Emitters.end(); ++It)
        {
            if (It->Get() == Emitter)
            {
                Emitters.erase(It);
                return true;
            }
        }
        return false;
    }

    void CParticleSystem::MoveEmitter(CParticleEmitter* Emitter, int32 Direction)
    {
        if (Emitter == nullptr || Direction == 0)
        {
            return;
        }

        for (int32 i = 0; i < (int32)Emitters.size(); ++i)
        {
            if (Emitters[i].Get() == Emitter)
            {
                const int32 Target = i + (Direction < 0 ? -1 : 1);
                if (Target >= 0 && Target < (int32)Emitters.size())
                {
                    TObjectPtr<CParticleEmitter> Tmp = Emitters[i];
                    Emitters[i]      = Emitters[Target];
                    Emitters[Target] = Tmp;
                }
                return;
            }
        }
    }

    const FParticleParameter* CParticleSystem::FindUserParameter(const FName& InName) const
    {
        if (InName.IsNone())
        {
            return nullptr;
        }
        for (const FParticleParameter& Param : UserParameters)
        {
            if (Param.Name == InName)
            {
                return &Param;
            }
        }
        return nullptr;
    }

    FName CParticleSystem::GetPropertyBinding(const FName& PropertyName) const
    {
        if (PropertyName.IsNone())
        {
            return FName();
        }
        for (const FParticlePropertyBinding& Binding : PropertyBindings)
        {
            if (Binding.PropertyName == PropertyName)
            {
                return Binding.ParameterName;
            }
        }
        return FName();
    }

    void CParticleSystem::SetPropertyBinding(const FName& PropertyName, const FName& ParameterName)
    {
        if (PropertyName.IsNone())
        {
            return;
        }

        if (ParameterName.IsNone())
        {
            ClearPropertyBinding(PropertyName);
            return;
        }

        for (FParticlePropertyBinding& Binding : PropertyBindings)
        {
            if (Binding.PropertyName == PropertyName)
            {
                Binding.ParameterName = ParameterName;
                return;
            }
        }

        FParticlePropertyBinding NewBinding;
        NewBinding.PropertyName  = PropertyName;
        NewBinding.ParameterName = ParameterName;
        PropertyBindings.push_back(NewBinding);
    }

    void CParticleSystem::ClearPropertyBinding(const FName& PropertyName)
    {
        for (auto It = PropertyBindings.begin(); It != PropertyBindings.end(); ++It)
        {
            if (It->PropertyName == PropertyName)
            {
                PropertyBindings.erase(It);
                return;
            }
        }
    }

    bool CParticleSystem::HasPropertyBinding(const FName& PropertyName) const
    {
        return !GetPropertyBinding(PropertyName).IsNone();
    }

    static float ResolveBoundFloat(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, float Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        return ParamName.IsNone() ? Literal : Comp.GetFloat(ParamName, Literal);
    }

    static int32 ResolveBoundInt(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, int32 Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        return ParamName.IsNone() ? Literal : Comp.GetInt(ParamName, Literal);
    }

    static bool ResolveBoundBool(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, bool Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        return ParamName.IsNone() ? Literal : Comp.GetBool(ParamName, Literal);
    }

    static FVector2 ResolveBoundVec2(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, FVector2 Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        return (ParamName.IsNone() || !Comp.HasParameter(ParamName)) ? Literal : Comp.GetVec2(ParamName);
    }

    static FVector3 ResolveBoundVec3(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, FVector3 Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        return (ParamName.IsNone() || !Comp.HasParameter(ParamName)) ? Literal : Comp.GetVec3(ParamName);
    }

    static FVector4 ResolveBoundVec4(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, FName PropName, FVector4 Literal)
    {
        const FName ParamName = Asset.GetPropertyBinding(PropName);
        if (ParamName.IsNone())
        {
            return Literal;
        }

        // Color and Vec4 share storage; either can drive a vec4 property.
        if (Comp.HasParameter(ParamName))
        {
            return Comp.GetColor(ParamName) + Comp.GetVec4(ParamName);
        }
        return Literal;
    }

    FResolvedParticleParams ResolveParticleParams(const CParticleSystem& Asset, const CParticleEmitter& Emitter, const SParticleSystemComponent& Component)
    {
        FResolvedParticleParams R;

        R.MaxParticles            = Emitter.MaxParticles;
        R.SpawnRate               = ResolveBoundFloat(Asset, Component, "SpawnRate",              Emitter.SpawnRate);
        R.BurstCount              = ResolveBoundInt  (Asset, Component, "BurstCount",             Emitter.BurstCount);
        R.Duration                = ResolveBoundFloat(Asset, Component, "Duration",               Emitter.Duration);
        R.bLooping                = ResolveBoundBool (Asset, Component, "bLooping",               Emitter.bLooping);

        R.Shape                   = Emitter.Shape;
        R.ShapeSize               = ResolveBoundVec3 (Asset, Component, "ShapeSize",              Emitter.ShapeSize);
        R.ShapeAngle              = ResolveBoundFloat(Asset, Component, "ShapeAngle",             Emitter.ShapeAngle);

        R.VelocityMode            = Emitter.VelocityMode;
        R.VelocityMin             = ResolveBoundVec3 (Asset, Component, "VelocityMin",            Emitter.VelocityMin);
        R.VelocityMax             = ResolveBoundVec3 (Asset, Component, "VelocityMax",            Emitter.VelocityMax);
        R.SpeedRange              = ResolveBoundVec2 (Asset, Component, "SpeedRange",             Emitter.SpeedRange);
        R.LifetimeRange           = ResolveBoundVec2 (Asset, Component, "LifetimeRange",          Emitter.LifetimeRange);

        R.Gravity                 = ResolveBoundVec3 (Asset, Component, "Gravity",                Emitter.Gravity);
        R.Drag                    = ResolveBoundFloat(Asset, Component, "Drag",                   Emitter.Drag);
        R.InheritEmitterVelocity  = ResolveBoundFloat(Asset, Component, "InheritEmitterVelocity", Emitter.InheritEmitterVelocity);

        R.StartColor              = ResolveBoundVec4 (Asset, Component, "StartColor",             Emitter.StartColor);
        R.EndColor                = ResolveBoundVec4 (Asset, Component, "EndColor",               Emitter.EndColor);
        R.StartSizeRange          = ResolveBoundVec2 (Asset, Component, "StartSizeRange",         Emitter.StartSizeRange);
        R.EndSizeRange            = ResolveBoundVec2 (Asset, Component, "EndSizeRange",           Emitter.EndSizeRange);
        R.RotationRange           = ResolveBoundVec2 (Asset, Component, "RotationRange",          Emitter.RotationRange);
        R.RotationSpeedRange      = ResolveBoundVec2 (Asset, Component, "RotationSpeedRange",     Emitter.RotationSpeedRange);

        R.NoiseStrength           = ResolveBoundVec3 (Asset, Component, "NoiseStrength",          Emitter.NoiseStrength);
        R.NoiseScale              = ResolveBoundFloat(Asset, Component, "NoiseScale",             Emitter.NoiseScale);
        R.NoiseSpeed              = ResolveBoundFloat(Asset, Component, "NoiseSpeed",             Emitter.NoiseSpeed);

        R.BlendMode               = Emitter.BlendMode;
        // An asset saved before FacingMode existed carries its intent in the bool, so the bool still decides.
        R.FacingMode              = (!Emitter.bBillboardToCamera && Emitter.FacingMode == EParticleFacingMode::CameraFacing)
                                  ? EParticleFacingMode::WorldXZ
                                  : Emitter.FacingMode;
        R.VelocityStretch         = Emitter.VelocityStretch;
        R.SubUVColumns            = Math::Max(Emitter.SubUVColumns, 1);
        R.SubUVRows               = Math::Max(Emitter.SubUVRows, 1);
        R.bWriteDepth             = ResolveBoundBool (Asset, Component, "bWriteDepth",            Emitter.bWriteDepth);

        return R;
    }

    // The module input decides how many components are read, so an int knob can drive a float input.
    static FVector4 ParameterAsVector4(const FParticleParameter& Param)
    {
        switch (Param.Type)
        {
        case EParticleParameterType::Float: return FVector4(Param.Scalar, 0.0f, 0.0f, 0.0f);
        case EParticleParameterType::Int:   return FVector4((float)Param.Integer, 0.0f, 0.0f, 0.0f);
        case EParticleParameterType::Bool:  return FVector4(Param.Boolean ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        default:                            return Param.Vector;
        }
    }

    void ApplyParticleParamBindings(const CParticleEmitter& Emitter, const SParticleSystemComponent& Component, TVector<FVector4>& InOutValues)
    {
        // The common case, a stack of plain constants, pays one empty check per emitter.
        if (Emitter.ParamBindings.empty())
        {
            return;
        }

        for (const SParticleParamBinding& Binding : Emitter.ParamBindings)
        {
            // An out-of-range slot means bindings saved against a different compile, so skip and keep the constant.
            if (Binding.SlotIndex < 0 || Binding.SlotIndex >= (int32)InOutValues.size())
            {
                continue;
            }

            const FParticleParameter* Param = Component.FindParameter(Binding.ParameterName);
            if (Param == nullptr)
            {
                continue;   // never declared and never set from code; the authored constant stands
            }

            FVector4 Value = ParameterAsVector4(*Param);

            // Without the broadcast, y and z read zero, which reads as the module having stopped working.
            if (ParticleParamComponents(Param->Type) == 1 && ParticleParamComponents(Binding.Type) > 1)
            {
                Value = FVector4(Value.x, Value.x, Value.x, Value.x);
            }

            InOutValues[Binding.SlotIndex] = Value;
        }
    }
}
