#include "RuntimePCH.h"
#include "ParticleSystem.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Entity/Components/ParticleSystemComponent.h"

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

    void CParticleSystem::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);
    }

    void CParticleSystem::PostLoad()
    {
        if (!ComputeShaderBinaries.empty())
        {
            ComputeShader = FShaderLibrary::Commit(FName((GetGUID().ToString() + "_CS").c_str()), ERHIShaderType::Compute,
                TSpan<const uint32>(ComputeShaderBinaries.data(), ComputeShaderBinaries.size()));
        }
    }

    void CParticleSystem::OnDestroy()
    {
        CObject::OnDestroy();
        ComputeShader = nullptr;
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

    FResolvedParticleParams ResolveParticleParams(const CParticleSystem& Asset, const SParticleSystemComponent& Component)
    {
        FResolvedParticleParams R;

        R.MaxParticles            = Asset.MaxParticles;
        R.SpawnRate               = ResolveBoundFloat(Asset, Component, "SpawnRate",              Asset.SpawnRate);
        R.BurstCount              = ResolveBoundInt  (Asset, Component, "BurstCount",             Asset.BurstCount);
        R.Duration                = ResolveBoundFloat(Asset, Component, "Duration",               Asset.Duration);
        R.bLooping                = ResolveBoundBool (Asset, Component, "bLooping",               Asset.bLooping);

        R.Shape                   = Asset.Shape;
        R.ShapeSize               = ResolveBoundVec3 (Asset, Component, "ShapeSize",              Asset.ShapeSize);
        R.ShapeAngle              = ResolveBoundFloat(Asset, Component, "ShapeAngle",             Asset.ShapeAngle);

        R.VelocityMode            = Asset.VelocityMode;
        R.VelocityMin             = ResolveBoundVec3 (Asset, Component, "VelocityMin",            Asset.VelocityMin);
        R.VelocityMax             = ResolveBoundVec3 (Asset, Component, "VelocityMax",            Asset.VelocityMax);
        R.SpeedRange              = ResolveBoundVec2 (Asset, Component, "SpeedRange",             Asset.SpeedRange);
        R.LifetimeRange           = ResolveBoundVec2 (Asset, Component, "LifetimeRange",          Asset.LifetimeRange);

        R.Gravity                 = ResolveBoundVec3 (Asset, Component, "Gravity",                Asset.Gravity);
        R.Drag                    = ResolveBoundFloat(Asset, Component, "Drag",                   Asset.Drag);
        R.InheritEmitterVelocity  = ResolveBoundFloat(Asset, Component, "InheritEmitterVelocity", Asset.InheritEmitterVelocity);

        R.StartColor              = ResolveBoundVec4 (Asset, Component, "StartColor",             Asset.StartColor);
        R.EndColor                = ResolveBoundVec4 (Asset, Component, "EndColor",               Asset.EndColor);
        R.StartSizeRange          = ResolveBoundVec2 (Asset, Component, "StartSizeRange",         Asset.StartSizeRange);
        R.EndSizeRange            = ResolveBoundVec2 (Asset, Component, "EndSizeRange",           Asset.EndSizeRange);
        R.RotationRange           = ResolveBoundVec2 (Asset, Component, "RotationRange",          Asset.RotationRange);
        R.RotationSpeedRange      = ResolveBoundVec2 (Asset, Component, "RotationSpeedRange",     Asset.RotationSpeedRange);

        R.NoiseStrength           = ResolveBoundVec3 (Asset, Component, "NoiseStrength",          Asset.NoiseStrength);
        R.NoiseScale              = ResolveBoundFloat(Asset, Component, "NoiseScale",             Asset.NoiseScale);
        R.NoiseSpeed              = ResolveBoundFloat(Asset, Component, "NoiseSpeed",             Asset.NoiseSpeed);

        R.BlendMode               = Asset.BlendMode;
        R.bBillboardToCamera      = ResolveBoundBool (Asset, Component, "bBillboardToCamera",     Asset.bBillboardToCamera);
        R.bWriteDepth             = ResolveBoundBool (Asset, Component, "bWriteDepth",            Asset.bWriteDepth);

        return R;
    }
}
