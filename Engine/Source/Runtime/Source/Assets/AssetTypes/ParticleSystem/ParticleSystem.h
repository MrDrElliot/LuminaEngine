#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Renderer/RHIFwd.h"
#include "Renderer/RHI.h"
#include "ParticleSystem.generated.h"

namespace Lumina
{
    class CTexture;

    REFLECT()
    enum class RUNTIME_API EParticleEmitterShape : uint8
    {
        Point,
        Sphere,
        Box,
        Cone,
        Ring,
        Disk,
    };

    REFLECT()
    enum class RUNTIME_API EParticleVelocityMode : uint8
    {
        /** Random vec3 between VelocityMin and VelocityMax. */
        Explicit,
        /** Outward from emitter origin with magnitude in SpeedRange. */
        Radial,
    };

    REFLECT()
    enum class RUNTIME_API EParticleBlendMode : uint8
    {
        Alpha,
        Additive,
        PreMultiplied,
        Multiply,
    };

    REFLECT()
    enum class RUNTIME_API EParticleShaderMode : uint8
    {
        Default,
        Custom,
    };

    REFLECT()
    enum class RUNTIME_API EParticleParameterType : uint8
    {
        Float,
        Int,
        Bool,
        Vec2,
        Vec3,
        Vec4,
        Color,
    };

    /** Named, typed value on a particle asset; component overrides reuse the same shape. */
    REFLECT()
    struct RUNTIME_API FParticleParameter
    {
        GENERATED_BODY()

        PROPERTY()
        FName Name;

        PROPERTY()
        EParticleParameterType Type = EParticleParameterType::Float;

        float       Scalar  = 0.0f;
        int32       Integer = 0;
        bool        Boolean = false;
        /** Vec2/Vec3/Vec4/Color storage; higher components zero for narrower types. */
        FVector4   Vector  = FVector4(0.0f);

        /** Writes only the storage matching Type. */
        bool Serialize(FArchive& Ar);

        void CopyFrom(const FParticleParameter& Other)
        {
            Name    = Other.Name;
            Type    = Other.Type;
            Scalar  = Other.Scalar;
            Integer = Other.Integer;
            Boolean = Other.Boolean;
            Vector  = Other.Vector;
        }

        bool operator==(const FParticleParameter& Other) const
        {
            return Name    == Other.Name
                && Type    == Other.Type
                && Scalar  == Other.Scalar
                && Integer == Other.Integer
                && Boolean == Other.Boolean
                && Vector  == Other.Vector;
        }
    };

    /** Routes a built-in simulation property through a named user parameter. */
    REFLECT()
    struct RUNTIME_API FParticlePropertyBinding
    {
        GENERATED_BODY()

        PROPERTY()
        FName PropertyName;

        PROPERTY()
        FName ParameterName;
    };

    struct SParticleSystemComponent;

    /** GPU particle system asset; data-driven modules with optional graph-compiled custom compute shader. */
    REFLECT()
    class RUNTIME_API CParticleSystem : public CObject
    {
        GENERATED_BODY()

    public:

        CParticleSystem() = default;

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }
        void PostLoad() override;
        void OnDestroy() override;

        const FShaderEntry* GetCustomComputeShader() const { return ComputeShader; }
        bool HasCustomComputeShader() const { return ComputeShader != nullptr; }
        bool UsesCustomShader() const { return ShaderMode == EParticleShaderMode::Custom && HasCustomComputeShader(); }

        bool IsReadyForSimulation() const
        {
            if (MaxParticles <= 0)
            {
                return false;
            }
            if (ShaderMode == EParticleShaderMode::Custom && !HasCustomComputeShader())
            {
                return false;
            }
            return true;
        }

        PROPERTY()
        TVector<uint32> ComputeShaderBinaries;

        PROPERTY(Editable, Category = "User Parameters")
        TVector<FParticleParameter> UserParameters;

        PROPERTY()
        TVector<FParticlePropertyBinding> PropertyBindings;

        const FParticleParameter* FindUserParameter(const FName& InName) const;

        /** Returns NAME_None if no binding exists. */
        FName GetPropertyBinding(const FName& PropertyName) const;

        /** Pass NAME_None as ParameterName to clear. */
        void SetPropertyBinding(const FName& PropertyName, const FName& ParameterName);

        void ClearPropertyBinding(const FName& PropertyName);
        bool HasPropertyBinding(const FName& PropertyName) const;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 1)
        int32 MaxParticles = 1024;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float SpawnRate = 50.0f;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0)
        int32 BurstCount = 0;

        /** 0 means infinite (used with bLooping). */
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float Duration = 0.0f;

        PROPERTY(Editable, Category = "Simulation")
        bool bLooping = true;

        // Below: legacy uniform-driven fields kept (non-editable) for assets not yet compiled to a
        // module stack; superseded by editor modules baked into the compute shader. Don't surface in editor.
        PROPERTY()
        EParticleShaderMode ShaderMode = EParticleShaderMode::Default;

        PROPERTY()
        EParticleEmitterShape Shape = EParticleEmitterShape::Point;

        PROPERTY()
        FVector3 ShapeSize = FVector3(1.0f, 1.0f, 1.0f);

        PROPERTY()
        float ShapeAngle = 30.0f;

        PROPERTY()
        EParticleVelocityMode VelocityMode = EParticleVelocityMode::Explicit;

        PROPERTY()
        FVector3 VelocityMin = FVector3(-0.5f, 1.0f, -0.5f);

        PROPERTY()
        FVector3 VelocityMax = FVector3(0.5f, 3.0f, 0.5f);

        PROPERTY()
        FVector2 SpeedRange = FVector2(1.0f, 3.0f);

        PROPERTY()
        FVector2 LifetimeRange = FVector2(1.0f, 2.0f);

        PROPERTY()
        FVector3 Gravity = FVector3(0.0f, -9.8f, 0.0f);

        PROPERTY()
        float Drag = 0.0f;

        /** 0 = world-space spawns; 1 = particles flow with emitter motion. Applied at spawn regardless of modules. */
        PROPERTY(Editable, Category = "Emitter", ClampMin = 0.0f, ClampMax = 1.0f)
        float InheritEmitterVelocity = 0.0f;

        PROPERTY()
        FVector4 StartColor = FVector4(1.0f, 0.6f, 0.2f, 1.0f);

        PROPERTY()
        FVector4 EndColor = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

        PROPERTY()
        FVector2 StartSizeRange = FVector2(0.2f, 0.3f);

        PROPERTY()
        FVector2 EndSizeRange = FVector2(0.0f, 0.0f);

        PROPERTY()
        FVector2 RotationRange = FVector2(0.0f, 0.0f);

        PROPERTY()
        FVector2 RotationSpeedRange = FVector2(0.0f, 0.0f);

        PROPERTY()
        FVector3 NoiseStrength = FVector3(0.0f);

        PROPERTY()
        float NoiseScale = 1.0f;

        PROPERTY()
        float NoiseSpeed = 1.0f;

        PROPERTY(Editable, Category = "Render")
        EParticleBlendMode BlendMode = EParticleBlendMode::Additive;

        PROPERTY(Editable, Category = "Render")
        TObjectPtr<CTexture> Texture;

        PROPERTY(Editable, Category = "Render")
        bool bBillboardToCamera = true;

        PROPERTY(Editable, Category = "Render")
        bool bWriteDepth = false;

        const FShaderEntry* ComputeShader = nullptr;
    };

    /** Render-thread-only GPU + sim state per emitter; lives in FForwardRenderScene::ParticleGPUStates,
     *  NOT on the component, so the render thread never touches a component the game thread may have destroyed. */
    struct FParticleGPUState
    {
        RHI::GPUPtr     ParticleBuffer     = 0;  // RW structured buffer of FGPUParticle (64B stride)
        uint64          ParticleBufferSize = 0;
        RHI::GPUPtr     SpawnCounterBuffer = 0;  // Single uint, cleared per frame
        uint32          AllocatedMax        = 0;
        float           SpawnAccumulator    = 0.0f;
        float           TotalTime           = 0.0f;
        float           SystemAge           = 0.0f;
        uint32          FrameSeed           = 0u;
        bool            bBurstPending       = true;
        FVector3        PrevEmitterPosition = FVector3(0.0f);
        bool            bHasPrevPosition    = false;
        // Estimated time until all particles are dead; bumped on spawn, decremented otherwise.
        // At 0 with no spawn, the simulate dispatch is skipped.
        float           AliveTimeRemaining  = 0.0f;
    };

    /** Per-frame, per-emitter snapshot of simulation properties after binding resolution. */
    struct RUNTIME_API FResolvedParticleParams
    {
        int32                   MaxParticles            = 1024;
        float                   SpawnRate               = 0.0f;
        int32                   BurstCount              = 0;
        float                   Duration                = 0.0f;
        bool                    bLooping                = true;

        EParticleEmitterShape   Shape                   = EParticleEmitterShape::Point;
        FVector3               ShapeSize               = FVector3(1.0f);
        float                   ShapeAngle              = 30.0f;

        EParticleVelocityMode   VelocityMode            = EParticleVelocityMode::Explicit;
        FVector3               VelocityMin             = FVector3(0.0f);
        FVector3               VelocityMax             = FVector3(0.0f);
        FVector2               SpeedRange              = FVector2(1.0f, 3.0f);
        FVector2               LifetimeRange           = FVector2(1.0f, 2.0f);

        FVector3               Gravity                 = FVector3(0.0f, -9.8f, 0.0f);
        float                   Drag                    = 0.0f;
        float                   InheritEmitterVelocity  = 0.0f;

        FVector4               StartColor              = FVector4(1.0f);
        FVector4               EndColor                = FVector4(1.0f);
        FVector2               StartSizeRange          = FVector2(0.2f, 0.3f);
        FVector2               EndSizeRange            = FVector2(0.0f);
        FVector2               RotationRange           = FVector2(0.0f);
        FVector2               RotationSpeedRange      = FVector2(0.0f);

        FVector3               NoiseStrength           = FVector3(0.0f);
        float                   NoiseScale              = 1.0f;
        float                   NoiseSpeed              = 1.0f;

        EParticleBlendMode      BlendMode               = EParticleBlendMode::Additive;
        bool                    bBillboardToCamera      = true;
        bool                    bWriteDepth             = false;
    };

    /** Bound properties read through component overrides, falling back to asset parameter default. */
    RUNTIME_API FResolvedParticleParams ResolveParticleParams(const CParticleSystem& Asset, const SParticleSystemComponent& Component);
}
