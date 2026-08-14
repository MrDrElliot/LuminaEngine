#pragma once

#include "Renderer/ShaderHandle.h"

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
    enum class EParticleEmitterShape : uint8
    {
        Point,
        Sphere,
        Box,
        Cone,
        Ring,
        Disk,
    };

    REFLECT()
    enum class EParticleVelocityMode : uint8
    {
        /** Random vec3 between VelocityMin and VelocityMax. */
        Explicit,
        /** Outward from emitter origin with magnitude in SpeedRange. */
        Radial,
    };

    REFLECT()
    enum class EParticleBlendMode : uint8
    {
        Alpha,
        Additive,
        PreMultiplied,
        Multiply,
    };

    /** How a particle's billboard is oriented. */
    REFLECT()
    enum class EParticleFacingMode : uint8
    {
        /** Always square-on to the camera. */
        CameraFacing,
        /** Fixed to the world XZ plane; for ground decals and flat sheets. */
        WorldXZ,
        /** Long axis follows the particle's velocity, still turned to face the camera. Sparks, rain,
         *  debris trails -- anything whose direction of travel should read. */
        VelocityAligned,
    };

    REFLECT()
    enum class EParticleShaderMode : uint8
    {
        Default,
        Custom,
    };

    REFLECT()
    enum class EParticleParameterType : uint8
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

    /** Floats a parameter type occupies in the module parameter block. */
    inline uint32 ParticleParamComponents(EParticleParameterType Type)
    {
        switch (Type)
        {
        case EParticleParameterType::Vec2:  return 2;
        case EParticleParameterType::Vec3:  return 3;
        case EParticleParameterType::Vec4:
        case EParticleParameterType::Color: return 4;
        default:                            return 1;   // Float / Int / Bool all read as one scalar
        }
    }

    /** A module input: either the value authored in the details panel, or the name of a user parameter
     *  that supplies it at runtime.
     *
     *  This is what makes a stack drivable from code. Every module value input is one of these instead of
     *  a bare float/vector, so binding is a property of the INPUT rather than a separate mirror table
     *  listing which properties are redirected -- the arrangement this replaced, which had to be kept in
     *  sync by hand and silently rotted whenever a module changed.
     *
     *  Binding does not change the generated HLSL: the shader reads its float4 slot either way, and only
     *  the CPU-side writer of that slot differs. So binding, unbinding and renaming a binding all take the
     *  value-only refresh path -- no Slang, no shader swap, no preview restart. */
    REFLECT()
    struct RUNTIME_API SParticleParam
    {
        GENERATED_BODY()

        SParticleParam() = default;
        SParticleParam(float V)             : Type(EParticleParameterType::Float), Constant(V, 0.0f, 0.0f, 0.0f) {}
        SParticleParam(const FVector2& V)   : Type(EParticleParameterType::Vec2),  Constant(V, 0.0f, 0.0f) {}
        SParticleParam(const FVector3& V)   : Type(EParticleParameterType::Vec3),  Constant(V, 0.0f) {}
        SParticleParam(const FVector4& V)   : Type(EParticleParameterType::Vec4),  Constant(V) {}

        /** Declared by the module that owns the input, never chosen by the user: it picks the editor
         *  widget and filters which user parameters the bind menu offers. Set by the constructor from
         *  whatever value the module initialized the input with, so a module author gets it for free. */
        PROPERTY()
        EParticleParameterType Type = EParticleParameterType::Float;

        /** The authored value, used whenever ParameterName is None. Kept when a binding is added so that
         *  unbinding restores what was there -- and it stays in the compiled slot as the fallback, which
         *  is what the editor preview and any unresolved binding fall back to. */
        PROPERTY()
        FVector4 Constant = FVector4(0.0f);

        /** None means "use Constant". Otherwise the value comes from the system's user parameter of this
         *  name, with a component override taking priority over the asset's declared default. */
        PROPERTY()
        FName ParameterName;

        bool IsBound() const { return !ParameterName.IsNone(); }

        float    AsFloat()   const { return Constant.x; }
        FVector2 AsVector2() const { return FVector2(Constant.x, Constant.y); }
        FVector3 AsVector3() const { return FVector3(Constant.x, Constant.y, Constant.z); }
        FVector4 AsVector4() const { return Constant; }

        bool operator==(const SParticleParam& Other) const
        {
            return Type == Other.Type && Constant == Other.Constant && ParameterName == Other.ParameterName;
        }
    };

    /** One module parameter slot fed by a named user parameter, resolved fresh every frame.
     *
     *  Produced by the compiler alongside the slot values and stored on the emitter, so the runtime never
     *  needs the editor-only module stack to know which slots are drivable. */
    REFLECT()
    struct RUNTIME_API SParticleParamBinding
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        /** Index into CParticleEmitter::ModuleParamValues. */
        PROPERTY()
        int32 SlotIndex = 0;

        /** Width the module declared the input at; a narrower parameter broadcasts to fill it. */
        PROPERTY()
        EParticleParameterType Type = EParticleParameterType::Float;
    };

    /** Declared attributes the RENDERER can consume.
     *
     *  The vertex shader is shared rather than generated, so it cannot know the attribute layout at
     *  compile time the way the simulation shader does. The compiler resolves these names to slot indices
     *  once, the asset stores them, and the renderer passes them as uniforms -- so a module opts in simply
     *  by declaring an attribute under the matching name.
     *
     *  Exposing a new one means adding it here and reading it in ParticleVertex.slang; nothing in between
     *  needs to change. */
    namespace ParticleRenderAttribute
    {
        enum Type : uint8
        {
            SizeScaleX,
            SizeScaleY,
            PrevPosX,
            PrevPosY,
            PrevPosZ,
            Count
        };

        inline const char* const Names[Count] =
        {
            "SizeScaleX", "SizeScaleY",
            "PrevPosX", "PrevPosY", "PrevPosZ",
        };
    }

    struct SParticleSystemComponent;

    /** One emitter within a CParticleSystem: its own particle budget, module-compiled simulation shader,
     *  and render settings.
     *
     *  Every field here used to sit flat on CParticleSystem, which is why a system could only ever be a
     *  single emitter. A real effect is several at once -- an explosion is a flash, a smoke plume, sparks
     *  and debris, each wanting its own particle count, lifetime, blend mode and texture -- so the whole
     *  block moved here and the system became a list of these.
     *
     *  Emitters simulate independently: one dispatch and one draw each, with no shared buffers. */
    REFLECT()
    class RUNTIME_API CParticleEmitter : public CObject
    {
        GENERATED_BODY()

    public:

        CParticleEmitter() = default;

        void PostLoad() override;
        void OnDestroy() override;

        FShaderH GetCustomComputeShader() const { return ComputeShader; }
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

        /** Module-stack value inputs, one float4 slot per parameter, in the order the compiler assigned.
         *  Uploaded verbatim each frame as the shader's ModuleParams array. Editing a module input
         *  rewrites a slot here and needs no recompile; only a structural stack change rebuilds the
         *  shader. Empty for legacy data-driven assets, which read the SimParams uniforms instead. */
        PROPERTY()
        TVector<FVector4> ModuleParamValues;

        /** Slots of ModuleParamValues that a named user parameter drives, produced by the compiler from
         *  the module inputs that were bound. Empty for a stack whose inputs are all constants, which is
         *  the common case -- and the check the per-frame resolve early-outs on.
         *
         *  Not part of CompiledCodeHash on purpose: a binding changes who writes a slot, never the shader
         *  that reads it, so adding or clearing one takes the no-recompile path. */
        PROPERTY()
        TVector<SParticleParamBinding> ParamBindings;

        /** Layout the compiled shader was built against. A value-only refresh compares the freshly
         *  generated layout to this and forces a full rebuild on mismatch, so a slot can never be read
         *  at the wrong width or index. */
        PROPERTY()
        uint64 CompiledCodeHash = 0;

        /** Floats per particle in the declared-attribute buffer, matching PARTICLE_ATTR_FLOATS in the
         *  compiled shader. 1 when no module declared anything (the buffer still exists so indexing stays
         *  well-formed). Structural, so it only changes when the shader is rebuilt. */
        PROPERTY()
        uint32 AttributeFloatCount = 1;

        /** Slot index per ParticleRenderAttribute::Type, or -1 when the stack never declared it. Resolved
         *  at compile time, so it only moves with a rebuild -- same lifetime as AttributeFloatCount. */
        PROPERTY()
        TVector<int32> RenderAttributeSlots;

        /** Slot for a renderer-consumed attribute, or -1. Safe on an asset saved before the entry existed. */
        int32 GetRenderAttributeSlot(ParticleRenderAttribute::Type Attr) const
        {
            return (int32)Attr < (int32)RenderAttributeSlots.size() ? RenderAttributeSlots[(int32)Attr] : -1;
        }

        /** Shown as the column header in the editor. Purely cosmetic, but an effect with four emitters is
         *  unreadable when they are all called "Emitter". */
        PROPERTY(Editable, Category = "Emitter")
        FString EmitterName = "Emitter";

        /** Skipped entirely when false -- no dispatch, no draw, no GPU state. Lets one emitter of an effect
         *  be muted while tuning the others, which is most of what emitter iteration actually is. */
        PROPERTY(Editable, Category = "Emitter")
        bool bEnabled = true;

        /** Package-local name of this emitter's authoring module stack. The stack is an editor-only class,
         *  so it is linked by NAME rather than by TObjectPtr: a hard reference from a runtime asset would
         *  drag editor-only data into cooked builds. Stable across reorder and rename, and empty only until
         *  the editor first opens the emitter. */
        PROPERTY()
        FString AuthoringStackName;

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
        EParticleFacingMode FacingMode = EParticleFacingMode::CameraFacing;

        /** Extra length along the velocity axis, in seconds of travel: the quad is stretched by
         *  speed * this. 0 leaves it square. Only meaningful with VelocityAligned facing.
         *  Note that a Trail module overrides facing entirely -- see EParticleFacingMode. */
        PROPERTY(Editable, Category = "Render", ClampMin = 0.0f)
        float VelocityStretch = 0.0f;

        /** Sprite-sheet subdivisions. 1x1 (the default) uses the whole texture; anything larger plays the
         *  cells as a flipbook across the particle's life, which is what most smoke and explosion sheets
         *  need. The frame comes from age, so it costs no per-particle storage. */
        PROPERTY(Editable, Category = "Render", ClampMin = 1)
        int32 SubUVColumns = 1;

        PROPERTY(Editable, Category = "Render", ClampMin = 1)
        int32 SubUVRows = 1;

        /** Kept so assets authored before FacingMode still load; FacingMode supersedes it. */
        PROPERTY()
        bool bBillboardToCamera = true;

        PROPERTY(Editable, Category = "Render")
        bool bWriteDepth = false;

        FShaderH ComputeShader = {};
    };

    /** GPU particle system asset: an ordered list of emitters plus the parameters they share.
     *
     *  Only user parameters and their property bindings live at the system level -- they are addressed by
     *  name from a component override, so they have to be one namespace per asset rather than per emitter.
     *  Everything else is a property OF an emitter and lives on CParticleEmitter. */
    REFLECT()
    class RUNTIME_API CParticleSystem : public CObject
    {
        GENERATED_BODY()

    public:

        CParticleSystem() = default;

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }
        void PostLoad() override;

        /** Emitters in draw order. Never empty after PostLoad: an asset that somehow has none gets one, so
         *  no caller has to handle a system that cannot render anything. */
        PROPERTY()
        TVector<TObjectPtr<CParticleEmitter>> Emitters;

        /** Appends a new emitter with a unique display name. Returns it. */
        CParticleEmitter* AddEmitter();

        /** Removes an emitter. Refuses to remove the last one -- a system with no emitters has no meaning
         *  and every consumer would need a special case for it. Returns whether it removed anything. */
        bool RemoveEmitter(CParticleEmitter* Emitter);

        /** Moves an emitter earlier (-1) or later (+1) in draw order. No-op at the ends. */
        void MoveEmitter(CParticleEmitter* Emitter, int32 Direction);

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

    };

    /** Render-thread-only GPU + sim state per emitter; lives in FDefaultSceneRenderer::ParticleGPUStates,
     *  NOT on the component, so the render thread never touches a component the game thread may have destroyed. */
    struct FParticleGPUState
    {
        RHI::GPUPtr     ParticleBuffer     = 0;  // RW structured buffer of FGPUParticle (64B stride)
        uint64          ParticleBufferSize = 0;
        RHI::GPUPtr     SpawnCounterBuffer = 0;  // Single uint, cleared per frame
        // Declared per-particle attributes, parallel to ParticleBuffer and indexed by the same index.
        RHI::GPUPtr     AttributeBuffer     = 0;
        uint64          AttributeBufferSize = 0;
        uint32          AllocatedAttributeFloats = 0;
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
        EParticleFacingMode     FacingMode              = EParticleFacingMode::CameraFacing;
        float                   VelocityStretch         = 0.0f;
        int32                   SubUVColumns            = 1;
        int32                   SubUVRows               = 1;
        bool                    bWriteDepth             = false;
    };

    /** Bound properties read through component overrides, falling back to the emitter's authored value.
     *  Takes both: the values come from the emitter, but the property bindings that redirect them to a
     *  named user parameter are declared once per system. */
    RUNTIME_API FResolvedParticleParams ResolveParticleParams(const CParticleSystem& Asset, const CParticleEmitter& Emitter, const SParticleSystemComponent& Component);

    /** Overwrites the bound slots of a module parameter block with the component's live parameter values.
     *
     *  This is the whole runtime half of driving a stack from code: ParticleComponent.SetFloat("Size", x)
     *  writes a component override, and this stamps it into the slot the module input compiled to, on the
     *  way to the GPU. A binding whose parameter resolves to nothing leaves the authored constant, so a
     *  misspelled or not-yet-set name degrades to the value in the asset rather than to zero.
     *
     *  A parameter narrower than the input it drives broadcasts its scalar across the components, so one
     *  float can drive all three axes of a vector input. Wider narrows by truncation. */
    RUNTIME_API void ApplyParticleParamBindings(const CParticleEmitter& Emitter, const SParticleSystemComponent& Component, TVector<FVector4>& InOutValues);
}
