#pragma once

#include "Renderer/ShaderHandle.h"
#include "Containers/StaticArray.h"
#include "Containers/Span.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Renderer/RHIFwd.h"
#include "Renderer/RHI.h"
#include "ParticleSystem.generated.h"

namespace Lumina
{
    class CTexture;
    class CMaterialInterface;
    class CStaticMesh;
    class CTextureRenderTarget;

    REFLECT()
    enum class EParticleEmitterShape : uint8
    {
        Point,
        Sphere,
        Box,
        Cone,
        Ring,
        Disk,
        // The half of a sphere above the emitter, so a ground burst never spawns underground.
        Hemisphere,
        // The surface of the emitter's EmissionMesh, with ShapeSize.x scaling it.
        Mesh,
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
        // Turns toward the camera about world up only, so fire columns and distant smoke stay upright.
        VerticalAxis,
        // Lies across its velocity, so a sub-emitter spawned along a collision normal lies flat on the surface its parent hit.
        SurfaceAligned,
    };

    REFLECT()
    enum class EParticleRenderMode : uint8
    {
        Sprite,
        // Draws the emitter's Mesh at every particle, for debris and casings.
        Mesh,
        // A strip through each particle's recent positions, for rocket and tracer trails.
        Ribbon,
        // Draws nothing, for an emitter that only runs its grid or feeds other emitters.
        None,
    };

    REFLECT()
    enum class EParticleSortMode : uint8
    {
        // Farthest first, which alpha blending needs.
        ByDistance,
        OldestFirst,
        YoungestFirst,
        // Draw order is slot order, which is cheapest and fine for additive and opaque emitters.
        None,
    };

    REFLECT()
    enum class EParticleFlipbookMode : uint8
    {
        // One pass through the sheet across the particle's life.
        OverLife,
        // Plays at FlipbookFPS and wraps.
        Loop,
    };

    // What a parent emitter's particle must do for a sub-emitter to spawn from it.
    REFLECT()
    enum class EParticleEventType : uint8
    {
        Death,
        Collision,
        // EventRate times per second for as long as the parent lives.
        Continuous,
    };

    // Where a particle hit, read back from the GPU for an emitter that reports its collisions.
    REFLECT()
    struct RUNTIME_API SParticleCollision
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FVector3 Position = FVector3(0.0f);

        PROPERTY(Hidden)
        FVector3 Normal = FVector3(0.0f, 1.0f, 0.0f);

        // After the bounce, so a particle that dies on the hit reports zero.
        PROPERTY(Hidden)
        FVector3 Velocity = FVector3(0.0f);

        // How fast it was moving into the surface when it struck.
        PROPERTY(Hidden)
        float ImpactSpeed = 0.0f;

        PROPERTY(Hidden)
        float Size = 0.0f;

        PROPERTY(Hidden)
        int32 EmitterIndex = 0;
    };

    // An emitter's grid copied back from the GPU, a few frames old, for scripts to sample.
    struct RUNTIME_API FParticleGridSnapshot
    {
        int32           EmitterIndex = 0;
        FUIntVector2    Size = FUIntVector2(0u);
        uint32          Channels = 0;
        bool            bWraps = false;

        // The plane the grid lay on when it was copied, the same one the simulation used.
        FVector3        Origin = FVector3(0.0f);
        FVector3        Right = FVector3(1.0f, 0.0f, 0.0f);
        FVector3        Forward = FVector3(0.0f, 0.0f, 1.0f);
        FVector2        WorldSize = FVector2(1.0f);

        TVector<FName>  ChannelNames;

        // Cell after cell along rows, each holding Channels floats.
        TVector<float>  Values;

        // Bilinear between cell centers like the shader's SampleGrid, or zero for a channel the grid does not have.
        float Sample(FName Channel, const FVector3& WorldPosition) const;
    };

    // Which fields of an EmitParticle call replace what the spawn stack computed.
    namespace EParticleEmitFlags
    {
        enum Type : uint32
        {
            Position = 1u << 0,
            Velocity = 1u << 1,
            Color    = 1u << 2,
            Size     = 1u << 3,
        };
    }

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
    struct RUNTIME_API SParticleParameter
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName Name;

        PROPERTY(Hidden)
        EParticleParameterType Type = EParticleParameterType::Float;

        float       Scalar  = 0.0f;
        int32       Integer = 0;
        bool        Boolean = false;
        /** Vec2/Vec3/Vec4/Color storage; higher components zero for narrower types. */
        FVector4   Vector  = FVector4(0.0f);

        /** Writes only the storage matching Type. */
        bool Serialize(FArchive& Ar);

        void CopyFrom(const SParticleParameter& Other)
        {
            Name    = Other.Name;
            Type    = Other.Type;
            Scalar  = Other.Scalar;
            Integer = Other.Integer;
            Boolean = Other.Boolean;
            Vector  = Other.Vector;
        }

        bool operator==(const SParticleParameter& Other) const
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
    struct RUNTIME_API SParticlePropertyBinding
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName PropertyName;

        PROPERTY(Hidden)
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
        PROPERTY(Hidden)
        EParticleParameterType Type = EParticleParameterType::Float;

        /** The authored value, used whenever ParameterName is None. Kept when a binding is added so that
         *  unbinding restores what was there -- and it stays in the compiled slot as the fallback, which
         *  is what the editor preview and any unresolved binding fall back to. */
        PROPERTY(Hidden)
        FVector4 Constant = FVector4(0.0f);

        /** None means "use Constant". Otherwise the value comes from the system's user parameter of this
         *  name, with a component override taking priority over the asset's declared default. */
        PROPERTY(Hidden)
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

        PROPERTY(Hidden)
        FName ParameterName;

        /** Index into CParticleEmitter::ModuleParamValues. */
        PROPERTY(Hidden)
        int32 SlotIndex = 0;

        /** Width the module declared the input at; a narrower parameter broadcasts to fill it. */
        PROPERTY(Hidden)
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

    /** Candidate itself when it can shade sprites, or null when the texture path applies instead. */
    RUNTIME_API CMaterialInterface* ResolveParticleSpriteMaterial(CMaterialInterface* Candidate);

    /** One emitter within a CParticleSystem: its own particle budget, module-compiled simulation shader,
     *  and render settings.
     *
     *  Every field here used to sit flat on CParticleSystem, which is why a system could only ever be a
     *  single emitter. A real effect is several at once -- an explosion is a flash, a smoke plume, sparks
     *  and debris, each wanting its own particle count, lifetime, blend mode and texture -- so the whole
     *  block moved here and the system became a list of these.
     *
     *  Emitters simulate independently, with no shared buffers. */
    // A spawn at a set time in the emitter's cycle, optionally repeating.
    REFLECT()
    struct RUNTIME_API SParticleBurst
    {
        GENERATED_BODY()

        // Seconds into the cycle.
        PROPERTY(Editable, Category = "Burst", ClampMin = 0.0f)
        float Time = 0.0f;

        PROPERTY(Editable, Category = "Burst", ClampMin = 0)
        int32 Count = 10;

        // Times it fires again after the first.
        PROPERTY(Editable, Category = "Burst", ClampMin = 0)
        int32 Repeats = 0;

        // Seconds between repeats.
        PROPERTY(Editable, Category = "Burst", ClampMin = 0.0f, EditCondition = "Repeats != 0", EditConditionHides)
        float Interval = 0.5f;
    };

    // Particles the bursts release while the cycle clock moves from PrevAge to Age, half open so no burst fires twice. Age below PrevAge means the cycle wrapped, and CycleLength 0 means it never does.
    RUNTIME_API uint32 CountBurstSpawns(TSpan<const SParticleBurst> Bursts, float PrevAge, float Age, float CycleLength);

    REFLECT()
    class RUNTIME_API CParticleEmitter : public CObject
    {
        GENERATED_BODY()

    public:

        CParticleEmitter() = default;

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;
        void OnDestroy() override;

        // An emitter saved as its own package, which systems reference and share, rather than one living inside a system.
        bool IsAsset() const override;

        // The shader sources a generated simulation can reach, hashed once per run.
        static uint64 GetSimulationTemplateHash();

        // A generated stack with no shader for the current templates, which only an editor recompile can supply.
        bool NeedsShaderCompile() const { return ShaderMode == EParticleShaderMode::Custom && !HasCustomComputeShader(); }

        void StoreShaderInArtifactCache() const;

        /** The Particle-domain material to shade sprites with, or null when the texture path applies. */
        CMaterialInterface* GetRenderMaterial() const;

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

        PROPERTY(Hidden)
        TVector<uint32> ComputeShaderBinaries;

        // GetSimulationTemplateHash when the binaries in memory were built, never saved so a shader edit cannot dirty the asset.
        uint64 CompiledTemplateHash = 0;

        // Set when the asset still carried binaries from before the artifact cache, so the recompile asks for one save to drop them.
        bool bLoadedEmbeddedBinaries = false;

        /** Module-stack value inputs, one float4 slot per parameter, in the order the compiler assigned.
         *  Uploaded verbatim each frame as the shader's ModuleParams array. Editing a module input
         *  rewrites a slot here and needs no recompile; only a structural stack change rebuilds the
         *  shader. Empty for legacy data-driven assets, which read the SimParams uniforms instead. */
        PROPERTY(Hidden)
        TVector<FVector4> ModuleParamValues;

        /** Slots of ModuleParamValues that a named user parameter drives, produced by the compiler from
         *  the module inputs that were bound. Empty for a stack whose inputs are all constants, which is
         *  the common case -- and the check the per-frame resolve early-outs on.
         *
         *  Not part of CompiledCodeHash on purpose: a binding changes who writes a slot, never the shader
         *  that reads it, so adding or clearing one takes the no-recompile path. */
        PROPERTY(Hidden)
        TVector<SParticleParamBinding> ParamBindings;

        /** Layout the compiled shader was built against. A value-only refresh compares the freshly
         *  generated layout to this and forces a full rebuild on mismatch, so a slot can never be read
         *  at the wrong width or index. */
        PROPERTY(Hidden)
        uint64 CompiledCodeHash = 0;

        /** Floats per particle in the declared-attribute buffer, matching PARTICLE_ATTR_FLOATS in the
         *  compiled shader. 1 when no module declared anything (the buffer still exists so indexing stays
         *  well-formed). Structural, so it only changes when the shader is rebuilt. */
        PROPERTY(Hidden)
        uint32 AttributeFloatCount = 1;

        /** Slot index per ParticleRenderAttribute::Type, or -1 when the stack never declared it. Resolved
         *  at compile time, so it only moves with a rebuild -- same lifetime as AttributeFloatCount. */
        PROPERTY(Hidden)
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
         *  so it is linked by NAME rather than by TStrongObjectPtr: a hard reference from a runtime asset would
         *  drag editor-only data into cooked builds. Stable across reorder and rename, and empty only until
         *  the editor first opens the emitter. */
        PROPERTY(Hidden)
        FString AuthoringStackName;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 1, ClampMax = 1048576)
        int32 MaxParticles = 1024;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float SpawnRate = 50.0f;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0)
        int32 BurstCount = 0;

        // Bursts at set times in each cycle, on top of Spawn Rate. Only the first eight are used.
        PROPERTY(Editable, Category = "Simulation")
        TVector<SParticleBurst> Bursts;

        // Particles per meter the emitter travels, for a trail of puffs behind something moving.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float SpawnPerMeter = 0.0f;

        /** 0 means infinite (used with bLooping). */
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float Duration = 0.0f;

        PROPERTY(Editable, Category = "Simulation")
        bool bLooping = true;

        // Carries live particles along when the emitter moves or turns, for exhaust and muzzle effects.
        PROPERTY(Editable, Category = "Simulation")
        bool bLocalSpace = false;

        // Seconds simulated at once when the emitter starts, so a looping effect appears already in progress.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float PrewarmTime = 0.0f;

        // Fraction of each cycle's particles released together at its start, where 1 is one burst per cycle.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f, ClampMax = 1.0f)
        float Explosiveness = 0.0f;

        // Steps the simulation at this rate instead of every frame. Zero follows the frame rate.
        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f)
        float FixedFPS = 0.0f;

        // Draws a fixed-rate emitter where its particles would be now, so a low FixedFPS still moves smoothly.
        PROPERTY(Editable, Category = "Simulation", EditCondition = "FixedFPS")
        bool bInterpolate = true;

        // Restarts the random sequence from Seed on every activation, so the effect plays the same each time.
        PROPERTY(Editable, Category = "Simulation")
        bool bUseFixedSeed = false;

        PROPERTY(Editable, Category = "Simulation", EditCondition = "bUseFixedSeed")
        int32 Seed = 0;

        // Surface a Shape Location module set to Mesh spawns on.
        PROPERTY(Editable, Category = "Simulation")
        TStrongObjectPtr<CStaticMesh> EmissionMesh;

        // Meters around the emitter its particles can reach, scaled with the entity. Off-screen bounds skip drawing, and zero never culls.
        PROPERTY(Editable, Category = "Culling", ClampMin = 0.0f)
        float VisibilityRadius = 20.0f;

        // Beyond this many meters from the camera the emitter neither simulates nor draws. Zero never culls.
        PROPERTY(Editable, Category = "Culling", ClampMin = 0.0f)
        float CullDistance = 0.0f;

        // Name of an emitter in this system whose particles spawn this one's. Empty spawns from the emitter itself.
        PROPERTY(Editable, Category = "Sub-Emitter")
        FString SpawnFromEmitter;

        PROPERTY(Editable, Category = "Sub-Emitter")
        EParticleEventType SpawnEvent = EParticleEventType::Death;

        PROPERTY(Editable, Category = "Sub-Emitter", ClampMin = 1)
        int32 ParticlesPerEvent = 1;

        // Events per second from each parent particle when SpawnEvent is Continuous.
        PROPERTY(Editable, Category = "Sub-Emitter", ClampMin = 0.0f, EditCondition = "SpawnEvent == Continuous")
        float EventRate = 10.0f;

        // Fraction of the parent particle's velocity each spawned particle starts with.
        PROPERTY(Editable, Category = "Sub-Emitter", ClampMin = 0.0f, ClampMax = 1.0f)
        float InheritParentVelocity = 0.0f;

        // Points each spawned particle along the surface normal its parent hit, which SurfaceAligned facing lies flat across.
        PROPERTY(Editable, Category = "Sub-Emitter", EditCondition = "SpawnEvent == Collision")
        bool bSpawnAlongSurfaceNormal = false;

        // Hands where this emitter's particles hit to scripts through CSpawnSubsystem::GetParticleCollisions, a few frames late.
        PROPERTY(Editable, Category = "Events")
        bool bReportCollisions = false;

        // Below: legacy uniform-driven fields kept (non-editable) for assets not yet compiled to a
        // module stack; superseded by editor modules baked into the compute shader. Don't surface in editor.
        PROPERTY(Hidden)
        EParticleShaderMode ShaderMode = EParticleShaderMode::Default;

        PROPERTY(Hidden)
        EParticleEmitterShape Shape = EParticleEmitterShape::Point;

        PROPERTY(Hidden)
        FVector3 ShapeSize = FVector3(1.0f, 1.0f, 1.0f);

        PROPERTY(Hidden)
        float ShapeAngle = 30.0f;

        PROPERTY(Hidden)
        EParticleVelocityMode VelocityMode = EParticleVelocityMode::Explicit;

        PROPERTY(Hidden)
        FVector3 VelocityMin = FVector3(-0.5f, 1.0f, -0.5f);

        PROPERTY(Hidden)
        FVector3 VelocityMax = FVector3(0.5f, 3.0f, 0.5f);

        PROPERTY(Hidden)
        FVector2 SpeedRange = FVector2(1.0f, 3.0f);

        PROPERTY(Hidden)
        FVector2 LifetimeRange = FVector2(1.0f, 2.0f);

        PROPERTY(Hidden)
        FVector3 Gravity = FVector3(0.0f, -9.8f, 0.0f);

        PROPERTY(Hidden)
        float Drag = 0.0f;

        /** 0 = world-space spawns; 1 = particles flow with emitter motion. Applied at spawn regardless of modules. */
        PROPERTY(Editable, Category = "Emitter", ClampMin = 0.0f, ClampMax = 1.0f)
        float InheritEmitterVelocity = 0.0f;

        PROPERTY(Hidden)
        FVector4 StartColor = FVector4(1.0f, 0.6f, 0.2f, 1.0f);

        PROPERTY(Hidden)
        FVector4 EndColor = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

        PROPERTY(Hidden)
        FVector2 StartSizeRange = FVector2(0.2f, 0.3f);

        PROPERTY(Hidden)
        FVector2 EndSizeRange = FVector2(0.0f, 0.0f);

        PROPERTY(Hidden)
        FVector2 RotationRange = FVector2(0.0f, 0.0f);

        PROPERTY(Hidden)
        FVector2 RotationSpeedRange = FVector2(0.0f, 0.0f);

        PROPERTY(Hidden)
        FVector3 NoiseStrength = FVector3(0.0f);

        PROPERTY(Hidden)
        float NoiseScale = 1.0f;

        PROPERTY(Hidden)
        float NoiseSpeed = 1.0f;

        /** Superseded by Material, which carries the blend its graph was authored against. */
        PROPERTY(Editable, Category = "Render", EditCondition = "!Material")
        EParticleBlendMode BlendMode = EParticleBlendMode::Additive;

        /** Sprite shading; null, or any domain but Particle, leaves this emitter on the texture path. */
        PROPERTY(Editable, Category = "Render")
        TStrongObjectPtr<CMaterialInterface> Material;

        /** Sampled only on the texture path; a Particle material samples whatever its graph binds. */
        PROPERTY(Editable, Category = "Render", EditCondition = "!Material")
        TStrongObjectPtr<CTexture> Texture;

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
        PROPERTY(Hidden)
        bool bBillboardToCamera = true;

        /** Superseded by Material, which declares its own depth write. */
        PROPERTY(Editable, Category = "Render", EditCondition = "!Material")
        bool bWriteDepth = false;

        // Meters over which a sprite fades out as it nears the surface behind it, hiding the seam. Zero disables it.
        PROPERTY(Editable, Category = "Render", ClampMin = 0.0f, EditCondition = "!bWriteDepth")
        float SoftFadeDistance = 0.3f;

        // Meters from the camera inside which a sprite fades away, so a burst at the lens never fills the screen. Zero disables it.
        PROPERTY(Editable, Category = "Render", ClampMin = 0.0f, Units = "m")
        float CameraFadeDistance = 0.0f;

        // Shades the sprite with the sun and sky like a surface, for smoke and dust. Additive sprites are light and stay unlit.
        PROPERTY(Editable, Category = "Render", EditCondition = "!Material")
        bool bLit = false;

        PROPERTY(Editable, Category = "Render")
        EParticleRenderMode RenderMode = EParticleRenderMode::Sprite;

        PROPERTY(Editable, Category = "Render", EditCondition = "RenderMode == Mesh")
        TStrongObjectPtr<CStaticMesh> Mesh;

        // Points each mesh's up axis along its velocity instead of tumbling it by the particle's rotation.
        PROPERTY(Editable, Category = "Render", EditCondition = "RenderMode == Mesh")
        bool bAlignMeshToVelocity = false;

        // Samples kept per ribbon, so more segments curve more smoothly over the same length.
        PROPERTY(Editable, Category = "Render", ClampMin = 2, ClampMax = 64, EditCondition = "RenderMode == Ribbon")
        int32 RibbonSegments = 12;

        // Seconds of travel the ribbon trails behind its particle.
        PROPERTY(Editable, Category = "Render", ClampMin = 0.01f, EditCondition = "RenderMode == Ribbon")
        float RibbonLength = 0.4f;

        // Ribbon width at the tail as a fraction of the head, which is the particle's size.
        PROPERTY(Editable, Category = "Render", ClampMin = 0.0f, EditCondition = "RenderMode == Ribbon")
        float RibbonTailWidth = 0.0f;

        // Draws the particles into the sun's shadow cascades, faded by their alpha.
        PROPERTY(Editable, Category = "Render")
        bool bCastShadows = false;

        PROPERTY(Editable, Category = "Render")
        EParticleSortMode SortMode = EParticleSortMode::ByDistance;

        PROPERTY(Editable, Category = "Render")
        EParticleFlipbookMode FlipbookMode = EParticleFlipbookMode::OverLife;

        PROPERTY(Editable, Category = "Render", ClampMin = 0.0f, EditCondition = "FlipbookMode == Loop")
        float FlipbookFPS = 15.0f;

        // Starts each particle on a random cell, so a sheet of variants reads as a mix.
        PROPERTY(Editable, Category = "Render")
        bool bRandomStartFrame = false;

        // Cells across the grid, along the emitter's right axis. The grid exists once a module declares a channel.
        PROPERTY(Editable, Category = "Grid", ClampMin = 1, ClampMax = 1024)
        int32 GridWidth = 128;

        // Cells along the emitter's forward axis.
        PROPERTY(Editable, Category = "Grid", ClampMin = 1, ClampMax = 1024)
        int32 GridHeight = 128;

        // Meters the grid spans along the emitter's right and forward axes, centered on the emitter.
        PROPERTY(Editable, Category = "Grid", Units = "m")
        FVector2 GridWorldSize = FVector2(10.0f, 10.0f);

        // Joins opposite edges, so stencils and deposits wrap around and a pattern tiles.
        PROPERTY(Editable, Category = "Grid")
        bool bGridWraps = false;

        // Receives the grid every frame through the Render Target Output modules, for materials to sample.
        PROPERTY(Editable, Category = "Grid")
        TStrongObjectPtr<CTextureRenderTarget> GridRenderTarget;

        // Copies the grid to the CPU every frame, a few frames late, for SParticleSystemComponent::SampleGrid.
        PROPERTY(Editable, Category = "Grid")
        bool bReadBackGrid = false;

        // Channel names in cell order, so a script can sample one by name.
        PROPERTY(Hidden)
        TVector<FName> GridChannelNames;

        // Arrays the stack reads, in the order the shader indexes them, filled from SParticleSystemComponent::SetArray.
        PROPERTY(Hidden)
        TVector<FName> ArrayNames;

        // Meters across one neighbor cell, which bounds how far a module like Boids can see.
        PROPERTY(Editable, Category = "Neighbors", ClampMin = 0.01f, Units = "m")
        float NeighborCellSize = 0.5f;

        // Cells along each axis of the cube around the emitter that particles find each other in.
        PROPERTY(Editable, Category = "Neighbors", ClampMin = 1, ClampMax = 64)
        int32 NeighborGridResolution = 32;

        // Particles one cell remembers, past which a crowded cell drops the rest.
        PROPERTY(Editable, Category = "Neighbors", ClampMin = 1, ClampMax = 32)
        int32 NeighborsPerCell = 8;

        // Set by the compile when a module looks particles up by their neighbors.
        PROPERTY(Hidden)
        bool bUsesNeighborGrid = false;

        // Floats per grid cell the compiled stack declared, zero when it has no grid.
        PROPERTY(Hidden)
        uint32 GridChannelCount = 0;

        // Dispatches per frame of each simulation stage, in stack order.
        PROPERTY(Hidden)
        TVector<int32> GridStageIterations;

    private:

        uint64 MakeArtifactKey() const;
        bool LoadShaderFromArtifactCache();
        void CommitComputeShader();
        void ReleaseComputeShader();

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
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CParticleEmitter>> Emitters;

        /** Appends a new emitter with a unique display name. Returns it. */
        CParticleEmitter* AddEmitter();

        /** Removes an emitter. Refuses to remove the last one -- a system with no emitters has no meaning
         *  and every consumer would need a special case for it. Returns whether it removed anything. */
        bool RemoveEmitter(CParticleEmitter* Emitter);

        /** Moves an emitter earlier (-1) or later (+1) in draw order. No-op at the ends. */
        void MoveEmitter(CParticleEmitter* Emitter, int32 Direction);

        PROPERTY(Editable, Category = "User Parameters")
        TVector<SParticleParameter> UserParameters;

        PROPERTY(Hidden)
        TVector<SParticlePropertyBinding> PropertyBindings;

        const SParticleParameter* FindUserParameter(const FName& InName) const;

        // Seconds from activation until the last burst particle dies, or zero when an emitter streams forever.
        FUNCTION()
        float GetOneShotLength() const;

        /** Returns NAME_None if no binding exists. */
        FName GetPropertyBinding(const FName& PropertyName) const;

        /** Pass NAME_None as ParameterName to clear. */
        void SetPropertyBinding(const FName& PropertyName, const FName& ParameterName);

        void ClearPropertyBinding(const FName& PropertyName);
        bool HasPropertyBinding(const FName& PropertyName) const;

    };

    /** One simulated particle as the GPU lays it out. Mirrors FGPUParticle in ParticleSimCommon.slang;
     *  the renderer only ever sizes and spans this, never reads a field, so the fields stay named there. */
    struct alignas(16) FGPUParticle
    {
        float Data[16];
    };
    static_assert(sizeof(FGPUParticle) == 64, "FGPUParticle must match ParticleSimCommon.slang");

    /** Render-thread-only GPU + sim state per emitter; lives in FDefaultSceneRenderer::ParticleGPUStates,
     *  NOT on the component, so the render thread never touches a component the game thread may have destroyed. */
    struct FParticleGPUState
    {
        // The one allocation every buffer below is a region of, so an emitter costs one allocation and one clear to set up.
        RHI::FGPUAllocation Block = {};
        uint64          BlockSize = 0;
        RHI::FGPUAllocation ParticleBuffer = {};  // RW structured buffer of FGPUParticle
        uint64          ParticleBufferSize = 0;
        RHI::FGPUAllocation SpawnCounterBuffer = {};  // Single uint, cleared per frame
        // The counters this frame's simulation used, SpawnCounterBuffer or a slot of the renderer's shared counter buffer.
        RHI::GPUPtr     FrameSpawnCounter   = 0;
        // Declared per-particle attributes, parallel to ParticleBuffer and indexed by the same index.
        RHI::FGPUAllocation AttributeBuffer = {};
        uint64          AttributeBufferSize = 0;
        uint32          AllocatedAttributeFloats = 0;
        uint32          AllocatedMax        = 0;
        // Live slots back-to-front, null past PARTICLE_SORT_CAPACITY, which selects the unsorted draw.
        RHI::FGPUAllocation SortIndexBuffer = {};
        RHI::FGPUAllocation SortDrawArgsBuffer = {};
        // Power-of-two span ParticleSortCompact bitonic-sorts, zero when the emitter is too large.
        uint32          SortCount           = 0;
        // Cleared each frame; false means the sim was skipped and the draw args describe a past frame.
        bool            bSimulatedThisFrame = false;
        float           SpawnAccumulator    = 0.0f;
        float           DistanceAccumulator = 0.0f;
        float           TotalTime           = 0.0f;
        float           SystemAge           = 0.0f;
        uint32          FrameSeed           = 0u;
        bool            bBurstPending       = true;
        FVector3        PrevEmitterPosition = FVector3(0.0f);
        bool            bHasPrevPosition    = false;
        // Estimated time until all particles are dead; bumped on spawn, decremented otherwise.
        // At 0 with no spawn, the simulate dispatch is skipped.
        float           AliveTimeRemaining  = 0.0f;

        // Events this emitter's particles raise for sub-emitters, one list per EParticleEventType.
        RHI::FGPUAllocation EventBuffer = {};
        uint64          EventBufferSize     = 0;
        // Past positions per particle for ribbons, RibbonSegments float4 each.
        RHI::FGPUAllocation RibbonHistoryBuffer = {};
        uint64          RibbonHistorySize   = 0;
        uint32          AllocatedRibbonSegments = 0;
        uint32          RibbonHead          = 0;
        float           RibbonTimer         = 0.0f;
        // Keys for emitters too large for the single-workgroup sort.
        RHI::FGPUAllocation SortKeyBuffer = {};
        uint64          SortKeyBufferSize   = 0;
        uint64          SortIndexBufferSize = 0;
        // Unspent time below one fixed step, and the offset the draw extrapolates particles by.
        float           FixedStepRemainder  = 0.0f;
        // Negative until the first explosive cycle fires, which is on the emitter's first step.
        float           CycleTime           = -1.0f;
        FMatrix4        PrevEmitterMatrix   = FMatrix4(1.0f);
        bool            bHasPrevMatrix      = false;
        bool            bPrewarmPending     = true;
        // Seconds a sub-emitter keeps simulating after its parent last ran, so late events still spawn.
        float           ParentAliveTime     = 0.0f;
        // The capacity last refused for exceeding the byte budget, so the error logs once.
        uint32          RejectedMax         = 0;

        // Two grids that simulation stages read one of and write the other, plus fixed point deposits from particles.
        RHI::FGPUAllocation GridBuffers[2] = {};
        RHI::FGPUAllocation GridDepositBuffer = {};
        uint64          GridBufferSize      = 0;
        uint32          AllocatedGridCells  = 0;
        uint32          AllocatedGridChannels = 0;
        // Which of GridBuffers holds the newest grid.
        uint32          GridCurrent         = 0;

        // Particles bucketed by cell after every step, a count per cell and NeighborsPerCell slots of indices.
        RHI::FGPUAllocation NeighborCounts = {};
        RHI::FGPUAllocation NeighborIndices = {};
        uint32          AllocatedNeighborCells = 0;
        uint32          AllocatedNeighborSlots = 0;
    };

    // Matches the ClampMax on CParticleEmitter::MaxParticles.
    inline constexpr int32 ParticleEmitterMaxCapacity = 1 << 20;

    // Matches the ClampMax on CParticleEmitter::GridWidth and GridHeight.
    inline constexpr int32 ParticleGridMaxResolution = 1024;

    // Matches the ClampMax on CParticleModule_SimulationStage::Iterations.
    inline constexpr int32 ParticleStageMaxIterations = 256;

    /** Per-frame, per-emitter snapshot of simulation properties after binding resolution. */
    struct RUNTIME_API FResolvedParticleParams
    {
        int32                   MaxParticles            = 1024;
        float                   SpawnRate               = 0.0f;
        int32                   BurstCount              = 0;
        // Copied rather than pointed at, since the render thread must not read the asset.
        static constexpr int32  MaxBursts               = 8;
        TArray<SParticleBurst, MaxBursts> Bursts        = {};
        int32                   NumBursts               = 0;
        float                   SpawnPerMeter           = 0.0f;
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
        float                   SoftFadeDistance        = 0.0f;
        float                   CameraFadeDistance      = 0.0f;
        bool                    bLit                    = false;

        bool                    bLocalSpace             = false;
        float                   PrewarmTime             = 0.0f;
        float                   Explosiveness           = 0.0f;
        float                   FixedFPS                = 0.0f;
        bool                    bInterpolate            = true;
        bool                    bUseFixedSeed           = false;
        int32                   Seed                    = 0;
        float                   VisibilityRadius        = 0.0f;
        float                   CullDistance            = 0.0f;

        EParticleRenderMode     RenderMode              = EParticleRenderMode::Sprite;
        bool                    bAlignMeshToVelocity    = false;
        int32                   RibbonSegments          = 12;
        float                   RibbonLength            = 0.4f;
        float                   RibbonTailWidth         = 0.0f;
        bool                    bCastShadows            = false;
        EParticleSortMode       SortMode                = EParticleSortMode::ByDistance;
        EParticleFlipbookMode   FlipbookMode            = EParticleFlipbookMode::OverLife;
        float                   FlipbookFPS             = 15.0f;
        bool                    bRandomStartFrame       = false;

        EParticleEventType      SpawnEvent              = EParticleEventType::Death;
        int32                   ParticlesPerEvent       = 1;
        float                   EventRate               = 10.0f;
        float                   InheritParentVelocity   = 0.0f;
        bool                    bSpawnAlongSurfaceNormal = false;
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
