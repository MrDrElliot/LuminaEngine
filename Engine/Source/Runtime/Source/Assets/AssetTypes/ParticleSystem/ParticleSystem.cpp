#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "ParticleSystem.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/ArtifactCache/ArtifactCache.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/ShaderPaths.h"
#include "Renderer/SpirvStrip.h"
#include "Log/Log.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    bool SParticleParameter::Serialize(FArchive& Ar)
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

    void CParticleEmitter::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Particles");
        if (!Ar.IsWriting())
        {
            CObject::Serialize(Ar);
            return;
        }

        // An editor asset keeps its binaries in the artifact cache, and a cook embeds them since a game has no compiler.
        TVector<uint32> EditorBinaries = Move(ComputeShaderBinaries);
        ComputeShaderBinaries.clear();
        if (Ar.IsCooking())
        {
            ComputeShaderBinaries = EditorBinaries;
            Spirv::StripDebugInfo(ComputeShaderBinaries);
        }
        CObject::Serialize(Ar);
        ComputeShaderBinaries = Move(EditorBinaries);
    }

    void CParticleEmitter::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Particles");
        if (ShaderMode != EParticleShaderMode::Custom)
        {
            return;
        }

#if USING(WITH_EDITOR)
        // Binaries embedded in an editor asset predate the artifact cache or were built against other templates.
        bool bCurrent = !ComputeShaderBinaries.empty() && CompiledTemplateHash == GetSimulationTemplateHash();
#else
        // A cooked game ships the binaries it was built with and has no compiler to replace them.
        bool bCurrent = !ComputeShaderBinaries.empty();
#endif
        if (!bCurrent)
        {
            bLoadedEmbeddedBinaries |= !ComputeShaderBinaries.empty();
            ComputeShaderBinaries.clear();
            bCurrent = LoadShaderFromArtifactCache();
        }

        if (bCurrent)
        {
            CommitComputeShader();
            return;
        }

        // A shader built for other templates reads arguments laid out differently, so the emitter waits for a recompile instead.
        ReleaseComputeShader();
#if !USING(WITH_EDITOR)
        LOG_ERROR("Particle emitter '{}' in '{}' has no simulation shader for the current templates. Open the project in the editor once so it compiles into the artifact cache, or cook the content.",
                  EmitterName.c_str(), GetPackage() != nullptr ? GetPackage()->GetPackagePath().c_str() : "");
#endif
    }

    bool CParticleEmitter::IsAsset() const
    {
        const CPackage* Package = GetPackage();
        if (Package == nullptr)
        {
            return false;
        }
        const FFixedString Path = Package->GetPackagePath();
        return GetName() == FName(VFS::FileName(FStringView(Path.c_str(), Path.size()), true));
    }

    void CParticleEmitter::OnDestroy()
    {
        CObject::OnDestroy();
        ReleaseComputeShader();
    }

    uint64 CParticleEmitter::GetSimulationTemplateHash()
    {
        // Computed once per run, since the sources only change between runs.
        static const uint64 CachedHash = []() -> uint64
        {
            constexpr FStringView Directories[] = { "/Engine/Resources/Shaders/Particles", "/Engine/Resources/Shaders/Includes", "/Engine/Resources/Shaders/Shared" };
            return Shaders::HashSourceDirectories(Directories);
        }();
        return CachedHash;
    }

    uint64 CParticleEmitter::MakeArtifactKey() const
    {
        constexpr uint64 kParticleArtifactVersion = 1;
        const uint64 Parts[] = { CompiledCodeHash, GetSimulationTemplateHash(), kParticleArtifactVersion };
        return Hash::GetHash64(Parts, sizeof(Parts), 0x50415254534D4C41ull);
    }

    bool CParticleEmitter::LoadShaderFromArtifactCache()
    {
        // Test hosts run without an engine, and a stack that never compiled has no key.
        TVector<uint8> Bytes;
        if (CompiledCodeHash == 0 || GEngine == nullptr || !GEngine->GetArtifactCache().Load(MakeArtifactKey(), Bytes) || Bytes.empty() || Bytes.size() % sizeof(uint32) != 0)
        {
            return false;
        }

        ComputeShaderBinaries.resize(Bytes.size() / sizeof(uint32));
        Memory::Memcpy(ComputeShaderBinaries.data(), Bytes.data(), Bytes.size());
        CompiledTemplateHash = GetSimulationTemplateHash();
        return true;
    }

    void CParticleEmitter::StoreShaderInArtifactCache() const
    {
        if (CompiledCodeHash == 0 || ComputeShaderBinaries.empty() || GEngine == nullptr)
        {
            return;
        }

        const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(ComputeShaderBinaries.data()), ComputeShaderBinaries.size() * sizeof(uint32));
        GEngine->GetArtifactCache().Store(MakeArtifactKey(), Bytes);
    }

    void CParticleEmitter::CommitComputeShader()
    {
        // The GUID keeps the key unique, and the readable prefix is what pipeline and crash logs print.
        FString ShaderKey = GetPackage() != nullptr ? GetPackage()->GetName().ToString() + "." : FString();
        ShaderKey += EmitterName + "_" + GetGUID().ToString() + "_CS";

        // Commit adds a reference every call, so the one held for the previous compile is given back, even when it is the same entry.
        const FShaderH Previous = ComputeShader;
        ComputeShader = FShaderLibrary::Commit(FName(ShaderKey), ERHIShaderType::Compute,
            TSpan<const uint32>(ComputeShaderBinaries.data(), ComputeShaderBinaries.size()));
        if (Previous != nullptr)
        {
            FShaderLibrary::Release(Previous);
        }
    }

    void CParticleEmitter::ReleaseComputeShader()
    {
        if (ComputeShader != nullptr)
        {
            FShaderLibrary::Release(ComputeShader);
        }
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
        // A shared emitter asset that was deleted leaves an empty slot nothing can draw or edit.
        if (const size_t Missing = Emitters.RemoveAllBy([](const TStrongObjectPtr<CParticleEmitter>& Emitter) { return !Emitter.IsValid(); }))
        {
            LOG_WARN("Particle system '{}' dropped {} emitter(s) whose asset no longer exists.", GetName().c_str(), Missing);
        }

        // Emitters is the only thing every consumer indexes, so an empty one gets a default instead.
        if (Emitters.empty())
        {
            AddEmitter();
        }

        // A nested export gets no PostLoad from the outer load, while a shared emitter asset already had its own.
        bool bNeedsCompile = false;
        for (const TStrongObjectPtr<CParticleEmitter>& Emitter : Emitters)
        {
            if (Emitter->GetPackage() == GetPackage())
            {
                Emitter->PostLoad();
            }
            bNeedsCompile |= Emitter->NeedsShaderCompile();
        }

#if USING(WITH_EDITOR)
        // A tool or test host without a renderer compiles nothing, so the emitters simply stay idle.
        FRenderManager* Renderer = TryRender();
        if (bNeedsCompile && Renderer != nullptr && GetPackage() != nullptr)
        {
            Renderer->GetParticleCompileQueue().QueueRecompile(this);
        }
#endif
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
            for (const TStrongObjectPtr<CParticleEmitter>& Existing : Emitters)
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
                    TStrongObjectPtr<CParticleEmitter> Tmp = Emitters[i];
                    Emitters[i]      = Emitters[Target];
                    Emitters[Target] = Tmp;
                }
                return;
            }
        }
    }

    float CParticleSystem::GetOneShotLength() const
    {
        auto FindEmitter = [this](const FString& Name) -> const CParticleEmitter*
        {
            for (const TStrongObjectPtr<CParticleEmitter>& Candidate : Emitters)
            {
                if (Candidate != nullptr && Candidate->EmitterName == Name)
                {
                    return Candidate.Get();
                }
            }
            return nullptr;
        };

        float Length = 0.0f;
        for (const TStrongObjectPtr<CParticleEmitter>& Emitter : Emitters)
        {
            if (Emitter == nullptr || !Emitter->bEnabled)
            {
                continue;
            }

            const bool bStreamsForever = Emitter->bLooping || (Emitter->SpawnRate > 0.0f && Emitter->Duration <= 0.0f);
            if (bStreamsForever)
            {
                return 0.0f;
            }

            // A sub-emitter's last particle can be born as late as its parent's last one dies.
            float Start = Emitter->Duration;
            if (const CParticleEmitter* Parent = Emitter->SpawnFromEmitter.empty() ? nullptr : FindEmitter(Emitter->SpawnFromEmitter))
            {
                Start = Math::Max(Start, Parent->Duration + Parent->LifetimeRange.y);
            }

            Length = Math::Max(Length, Start + Emitter->LifetimeRange.y);
        }

        return Length;
    }

    const SParticleParameter* CParticleSystem::FindUserParameter(const FName& InName) const
    {
        if (InName.IsNone())
        {
            return nullptr;
        }
        for (const SParticleParameter& Param : UserParameters)
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
        for (const SParticlePropertyBinding& Binding : PropertyBindings)
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

        for (SParticlePropertyBinding& Binding : PropertyBindings)
        {
            if (Binding.PropertyName == PropertyName)
            {
                Binding.ParameterName = ParameterName;
                return;
            }
        }

        SParticlePropertyBinding NewBinding;
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

    // Interned once per run, since building a name takes the name table's lock and every emitter resolves these each frame.
    struct FBindablePropertyNames
    {
        FName SpawnRate              = "SpawnRate";
        FName BurstCount             = "BurstCount";
        FName Duration               = "Duration";
        FName bLooping               = "bLooping";
        FName ShapeSize              = "ShapeSize";
        FName ShapeAngle             = "ShapeAngle";
        FName VelocityMin            = "VelocityMin";
        FName VelocityMax            = "VelocityMax";
        FName SpeedRange             = "SpeedRange";
        FName LifetimeRange          = "LifetimeRange";
        FName Gravity                = "Gravity";
        FName Drag                   = "Drag";
        FName InheritEmitterVelocity = "InheritEmitterVelocity";
        FName StartColor             = "StartColor";
        FName EndColor               = "EndColor";
        FName StartSizeRange         = "StartSizeRange";
        FName EndSizeRange           = "EndSizeRange";
        FName RotationRange          = "RotationRange";
        FName RotationSpeedRange     = "RotationSpeedRange";
        FName NoiseStrength          = "NoiseStrength";
        FName NoiseScale             = "NoiseScale";
        FName NoiseSpeed             = "NoiseSpeed";
        FName bWriteDepth            = "bWriteDepth";
    };

    // A bound property searches the component once, and an asset with no bindings skips even that.
    static const SParticleParameter* FindBoundParameter(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName)
    {
        return Asset.PropertyBindings.empty() ? nullptr : Comp.FindParameter(Asset.GetPropertyBinding(PropName));
    }

    static float ResolveBoundFloat(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, float Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        return (Param != nullptr && Param->Type == EParticleParameterType::Float) ? Param->Scalar : Literal;
    }

    static int32 ResolveBoundInt(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, int32 Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        return (Param != nullptr && Param->Type == EParticleParameterType::Int) ? Param->Integer : Literal;
    }

    static bool ResolveBoundBool(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, bool Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        return (Param != nullptr && Param->Type == EParticleParameterType::Bool) ? Param->Boolean : Literal;
    }

    static FVector2 ResolveBoundVec2(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, FVector2 Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        if (Param == nullptr)
        {
            return Literal;
        }
        return Param->Type == EParticleParameterType::Vec2 ? FVector2(Param->Vector) : FVector2(0.0f);
    }

    static FVector3 ResolveBoundVec3(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, FVector3 Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        if (Param == nullptr)
        {
            return Literal;
        }
        return Param->Type == EParticleParameterType::Vec3 ? FVector3(Param->Vector) : FVector3(0.0f);
    }

    static FVector4 ResolveBoundVec4(const CParticleSystem& Asset, const SParticleSystemComponent& Comp, const FName& PropName, FVector4 Literal)
    {
        const SParticleParameter* Param = FindBoundParameter(Asset, Comp, PropName);
        if (Param == nullptr)
        {
            return Literal;
        }

        // Color and Vec4 share storage, so either can drive a vec4 property.
        const bool bVectorLike = Param->Type == EParticleParameterType::Color || Param->Type == EParticleParameterType::Vec4;
        return bVectorLike ? Param->Vector : FVector4(0.0f);
    }

    float FParticleGridSnapshot::Sample(FName Channel, const FVector3& WorldPosition) const
    {
        const auto Found = Algo::Find(ChannelNames, Channel);
        if (Found == ChannelNames.end() || Size.x == 0u || Size.y == 0u || Values.size() < (size_t)Size.x * Size.y * Channels)
        {
            return 0.0f;
        }

        const uint32   ChannelIndex = (uint32)(Found - ChannelNames.begin());
        const FVector3 Offset = WorldPosition - Origin;
        const FVector2 UV     = FVector2(Math::Dot(Offset, Right), Math::Dot(Offset, Forward)) / Math::Max(WorldSize, FVector2(1e-4f)) + FVector2(0.5f);
        const FVector2 Cells  = UV * FVector2(Size) - FVector2(0.5f);
        const int32    BaseX  = (int32)Math::Floor(Cells.x);
        const int32    BaseY  = (int32)Math::Floor(Cells.y);
        const FVector2 Blend  = Cells - FVector2((float)BaseX, (float)BaseY);

        auto At = [&](int32 X, int32 Y)
        {
            const int32 Width = (int32)Size.x;
            const int32 Height = (int32)Size.y;
            X = bWraps ? ((X % Width) + Width) % Width : Math::Clamp(X, 0, Width - 1);
            Y = bWraps ? ((Y % Height) + Height) % Height : Math::Clamp(Y, 0, Height - 1);
            return Values[((size_t)Y * Size.x + (size_t)X) * Channels + ChannelIndex];
        };

        const float Low  = Math::Lerp(At(BaseX, BaseY), At(BaseX + 1, BaseY), Blend.x);
        const float High = Math::Lerp(At(BaseX, BaseY + 1), At(BaseX + 1, BaseY + 1), Blend.x);
        return Math::Lerp(Low, High, Blend.y);
    }

    uint32 CountBurstSpawns(TSpan<const SParticleBurst> Bursts, float PrevAge, float Age, float CycleLength)
    {
        const bool bWrapped = Age < PrevAge;
        uint32 Total = 0;
        for (const SParticleBurst& Burst : Bursts)
        {
            for (int32 Fire = 0; Fire <= Burst.Repeats; ++Fire)
            {
                const float At = Burst.Time + Burst.Interval * (float)Fire;
                if (CycleLength > 0.0f && At >= CycleLength)
                {
                    break;
                }

                const bool bCrossed = bWrapped ? (At >= PrevAge || At < Age) : (At >= PrevAge && At < Age);
                if (bCrossed)
                {
                    Total += (uint32)Math::Max(Burst.Count, 0);
                }
            }
        }
        return Total;
    }

    FResolvedParticleParams ResolveParticleParams(const CParticleSystem& Asset, const CParticleEmitter& Emitter, const SParticleSystemComponent& Component)
    {
        static const FBindablePropertyNames Names;
        FResolvedParticleParams R;

        R.MaxParticles            = Math::Clamp(Emitter.MaxParticles, 0, ParticleEmitterMaxCapacity);
        R.SpawnRate               = ResolveBoundFloat(Asset, Component, Names.SpawnRate,             Emitter.SpawnRate);
        R.BurstCount              = ResolveBoundInt  (Asset, Component, Names.BurstCount,            Emitter.BurstCount);
        R.NumBursts               = Math::Min((int32)Emitter.Bursts.size(), FResolvedParticleParams::MaxBursts);
        for (int32 Index = 0; Index < R.NumBursts; ++Index)
        {
            R.Bursts[Index] = Emitter.Bursts[Index];
        }
        R.SpawnPerMeter           = Math::Max(Emitter.SpawnPerMeter, 0.0f);
        R.Duration                = ResolveBoundFloat(Asset, Component, Names.Duration,              Emitter.Duration);
        R.bLooping                = ResolveBoundBool (Asset, Component, Names.bLooping,              Emitter.bLooping);

        R.Shape                   = Emitter.Shape;
        R.ShapeSize               = ResolveBoundVec3 (Asset, Component, Names.ShapeSize,             Emitter.ShapeSize);
        R.ShapeAngle              = ResolveBoundFloat(Asset, Component, Names.ShapeAngle,            Emitter.ShapeAngle);

        R.VelocityMode            = Emitter.VelocityMode;
        R.VelocityMin             = ResolveBoundVec3 (Asset, Component, Names.VelocityMin,           Emitter.VelocityMin);
        R.VelocityMax             = ResolveBoundVec3 (Asset, Component, Names.VelocityMax,           Emitter.VelocityMax);
        R.SpeedRange              = ResolveBoundVec2 (Asset, Component, Names.SpeedRange,            Emitter.SpeedRange);
        R.LifetimeRange           = ResolveBoundVec2 (Asset, Component, Names.LifetimeRange,         Emitter.LifetimeRange);

        R.Gravity                 = ResolveBoundVec3 (Asset, Component, Names.Gravity,               Emitter.Gravity);
        R.Drag                    = ResolveBoundFloat(Asset, Component, Names.Drag,                  Emitter.Drag);
        R.InheritEmitterVelocity  = ResolveBoundFloat(Asset, Component, Names.InheritEmitterVelocity, Emitter.InheritEmitterVelocity);

        R.StartColor              = ResolveBoundVec4 (Asset, Component, Names.StartColor,            Emitter.StartColor);
        R.EndColor                = ResolveBoundVec4 (Asset, Component, Names.EndColor,              Emitter.EndColor);
        R.StartSizeRange          = ResolveBoundVec2 (Asset, Component, Names.StartSizeRange,        Emitter.StartSizeRange);
        R.EndSizeRange            = ResolveBoundVec2 (Asset, Component, Names.EndSizeRange,          Emitter.EndSizeRange);
        R.RotationRange           = ResolveBoundVec2 (Asset, Component, Names.RotationRange,         Emitter.RotationRange);
        R.RotationSpeedRange      = ResolveBoundVec2 (Asset, Component, Names.RotationSpeedRange,    Emitter.RotationSpeedRange);

        R.NoiseStrength           = ResolveBoundVec3 (Asset, Component, Names.NoiseStrength,         Emitter.NoiseStrength);
        R.NoiseScale              = ResolveBoundFloat(Asset, Component, Names.NoiseScale,            Emitter.NoiseScale);
        R.NoiseSpeed              = ResolveBoundFloat(Asset, Component, Names.NoiseSpeed,            Emitter.NoiseSpeed);

        R.BlendMode               = Emitter.BlendMode;
        // An asset saved before FacingMode existed carries its intent in the bool, so the bool still decides.
        R.FacingMode              = (!Emitter.bBillboardToCamera && Emitter.FacingMode == EParticleFacingMode::CameraFacing)
                                  ? EParticleFacingMode::WorldXZ
                                  : Emitter.FacingMode;
        R.VelocityStretch         = Emitter.VelocityStretch;
        R.SubUVColumns            = Math::Max(Emitter.SubUVColumns, 1);
        R.SubUVRows               = Math::Max(Emitter.SubUVRows, 1);
        R.bWriteDepth             = ResolveBoundBool (Asset, Component, Names.bWriteDepth,           Emitter.bWriteDepth);
        R.SoftFadeDistance        = Emitter.SoftFadeDistance;
        R.CameraFadeDistance      = Emitter.CameraFadeDistance;
        R.bLit                    = Emitter.bLit;

        R.bLocalSpace             = Emitter.bLocalSpace;
        R.PrewarmTime             = Emitter.PrewarmTime;
        R.Explosiveness           = Math::Clamp(Emitter.Explosiveness, 0.0f, 1.0f);
        R.FixedFPS                = Emitter.FixedFPS;
        R.bInterpolate            = Emitter.bInterpolate;
        R.bUseFixedSeed           = Emitter.bUseFixedSeed;
        R.Seed                    = Emitter.Seed;
        R.VisibilityRadius        = Emitter.VisibilityRadius;
        R.CullDistance            = Emitter.CullDistance;

        R.RenderMode              = Emitter.RenderMode;
        R.bAlignMeshToVelocity    = Emitter.bAlignMeshToVelocity;
        R.RibbonSegments          = Math::Clamp(Emitter.RibbonSegments, 2, 64);
        R.RibbonLength            = Math::Max(Emitter.RibbonLength, 0.01f);
        R.RibbonTailWidth         = Math::Max(Emitter.RibbonTailWidth, 0.0f);
        R.bCastShadows            = Emitter.bCastShadows;
        R.SortMode                = Emitter.SortMode;
        R.FlipbookMode            = Emitter.FlipbookMode;
        R.FlipbookFPS             = Emitter.FlipbookFPS;
        R.bRandomStartFrame       = Emitter.bRandomStartFrame;

        R.SpawnEvent              = Emitter.SpawnEvent;
        R.ParticlesPerEvent       = Math::Max(Emitter.ParticlesPerEvent, 1);
        R.EventRate               = Math::Max(Emitter.EventRate, 0.0f);
        R.InheritParentVelocity   = Emitter.InheritParentVelocity;
        R.bSpawnAlongSurfaceNormal = Emitter.bSpawnAlongSurfaceNormal;

        return R;
    }

    // The module input decides how many components are read, so an int knob can drive a float input.
    static FVector4 ParameterAsVector4(const SParticleParameter& Param)
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

            const SParticleParameter* Param = Component.FindParameter(Binding.ParameterName);
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
