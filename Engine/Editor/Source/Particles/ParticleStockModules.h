#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "Core/Math/Math.h"
#include "ParticleStockModules.generated.h"

namespace Lumina
{
    /** How an Initial Velocity module derives the spawn velocity direction. */
    REFLECT()
    enum class EParticleInitVelocityMode : uint8
    {
        /** Independent per-axis random between Min and Max. */
        Explicit,
        /** Outward from the emitter origin (uses the spawn location offset). */
        Radial,
        /** Random direction inside a cone around the emitter forward. */
        Cone,
    };

    // Spawn modules

    /** Places newborn particles by sampling an emitter shape and writing P.Position. */
    REFLECT()
    class CParticleModule_SpawnLocation : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Shape Location"; }
        FString GetCategory() const override { return "Location"; }
        FString GetTooltip() const override { return "Spawn particles on an emitter shape (point, sphere, box, cone, ring, disk)."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Shape")
        EParticleEmitterShape Shape = EParticleEmitterShape::Point;

        /** Sphere: x=radius. Box: xyz=half extents. Cone: x=base radius, y=height. Ring/Disk: x=outer, y=inner. */
        PROPERTY(Editable, Category = "Shape")
        SParticleParam ShapeSize { FVector3(1.0f) };

        /** Cone half-angle in degrees. */
        PROPERTY(Editable, Category = "Shape", ClampMin = 0.0f, ClampMax = 180.0f)
        SParticleParam ConeAngle { 30.0f };
    };

    /** Sets the initial P.Velocity. */
    REFLECT()
    class CParticleModule_InitialVelocity : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Add Velocity"; }
        FString GetCategory() const override { return "Velocity"; }
        FString GetTooltip() const override { return "Give newborn particles a starting velocity."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Velocity")
        EParticleInitVelocityMode Mode = EParticleInitVelocityMode::Explicit;

        PROPERTY(Editable, Category = "Velocity")
        SParticleParam VelocityMin { FVector3(-0.5f, 1.0f, -0.5f) };

        PROPERTY(Editable, Category = "Velocity")
        SParticleParam VelocityMax { FVector3(0.5f, 3.0f, 0.5f) };

        /** Speed range for Radial / Cone modes. */
        PROPERTY(Editable, Category = "Velocity")
        SParticleParam SpeedRange { FVector2(1.0f, 3.0f) };

        /** Cone half-angle in degrees for Cone mode. */
        PROPERTY(Editable, Category = "Velocity", ClampMin = 0.0f, ClampMax = 180.0f)
        SParticleParam ConeAngle { 30.0f };
    };

    /** Sets the initial particle color. */
    REFLECT()
    class CParticleModule_InitialColor : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Color"; }
        FString GetCategory() const override { return "Color"; }
        FString GetTooltip() const override { return "Set the starting color of newborn particles."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Color", Color)
        SParticleParam Color { FVector4(1.0f, 0.6f, 0.2f, 1.0f) };
    };

    /** Sets the initial particle size (random within a range). */
    REFLECT()
    class CParticleModule_InitialSize : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Size"; }
        FString GetCategory() const override { return "Size"; }
        FString GetTooltip() const override { return "Set the starting size of newborn particles."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Size", ClampMin = 0.0f)
        SParticleParam SizeRange { FVector2(0.2f, 0.3f) };
    };

    /** Sets how long newborn particles live (random within a range). */
    REFLECT()
    class CParticleModule_Lifetime : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Lifetime"; }
        FString GetCategory() const override { return "Lifetime"; }
        FString GetTooltip() const override { return "Set how many seconds newborn particles live."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Lifetime", ClampMin = 0.01f)
        SParticleParam LifetimeRange { FVector2(1.0f, 2.0f) };
    };

    /** Sets the initial rotation and rotation speed (random within ranges). */
    REFLECT()
    class CParticleModule_InitialRotation : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Rotation"; }
        FString GetCategory() const override { return "Rotation"; }
        FString GetTooltip() const override { return "Set starting rotation and spin (degrees)."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Rotation")
        SParticleParam RotationRange { FVector2(0.0f, 0.0f) };

        PROPERTY(Editable, Category = "Rotation")
        SParticleParam RotationSpeedRange { FVector2(0.0f, 0.0f) };
    };

    /** Writes a per-particle Mass attribute that force modules can divide by. */
    REFLECT()
    class CParticleModule_SetMass : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Mass"; }
        FString GetCategory() const override { return "Physics"; }
        FString GetTooltip() const override { return "Give each particle a mass, so forces that respect it push heavy particles less."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Physics", ClampMin = 0.001f)
        SParticleParam MassRange { FVector2(1.0f, 1.0f) };
    };

    /** Writes the SizeScaleX/SizeScaleY attributes the renderer multiplies into the billboard extent. */
    REFLECT()
    class CParticleModule_NonUniformSize : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Non-Uniform Size"; }
        FString GetCategory() const override { return "Size"; }
        FString GetTooltip() const override { return "Scale the sprite independently on each axis: thin sparks, wide sheets, squashed smoke."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        /** Multipliers on the simulated size. 1,1 matches a square sprite. */
        PROPERTY(Editable, Category = "Size", ClampMin = 0.0f)
        SParticleParam Scale { FVector2(1.0f, 1.0f) };
    };

    // Update modules

    /** Accelerates particles by a constant gravity vector. */
    REFLECT()
    class CParticleModule_GravityForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Gravity Force"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Apply a constant acceleration each frame."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces")
        SParticleParam Gravity { FVector3(0.0f, -9.8f, 0.0f) };
    };

    /** Exponentially damps velocity (framerate-independent). */
    REFLECT()
    class CParticleModule_Drag : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Drag"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Slow particles down over time."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam Drag { 0.5f };

        /** Divide drag by each particle's Mass attribute so heavy particles slow less. Declares the
         *  attribute itself, so this works whether or not a Set Mass module is present -- without one
         *  every particle takes the declared default of 1 and the result matches unscaled drag. */
        PROPERTY(Editable, Category = "Forces")
        bool bScaleByMass = false;
    };

    /** Adds turbulence via a cheap curl-noise field. */
    REFLECT()
    class CParticleModule_CurlNoiseForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Curl Noise Force"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Push particles around with animated turbulence."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Noise")
        SParticleParam Strength { FVector3(1.0f) };

        PROPERTY(Editable, Category = "Noise", ClampMin = 0.0001f)
        SParticleParam Scale { 1.0f };

        PROPERTY(Editable, Category = "Noise")
        SParticleParam Speed { 1.0f };
    };

    /** Blends color from Start to End over the particle's life. */
    REFLECT()
    class CParticleModule_ColorOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Color Over Life"; }
        FString GetCategory() const override { return "Color"; }
        FString GetTooltip() const override { return "Fade color (and alpha) across the particle's lifetime."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        CParticleModule_ColorOverLife();

        /** Full color ramp across the particle's life, not just a start/end pair -- the two-stop lerp
         *  this replaced could not express a flash, a mid-life peak, or a hold-then-fade. */
        PROPERTY(Editable, Category = "Color")
        SGradient Gradient;
    };

    /** Interpolates size from Start to End over the particle's life. */
    REFLECT()
    class CParticleModule_SizeOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Size Over Life"; }
        FString GetCategory() const override { return "Size"; }
        FString GetTooltip() const override { return "Grow or shrink particles across their lifetime."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        CParticleModule_SizeOverLife();

        /** Size multiplier curve over life. Scales the size the Spawn stack set, so this shapes growth
         *  independently of how large the particle started. */
        PROPERTY(Editable, Category = "Size")
        SCurve Curve;
    };

    /** Records a lagging previous position so the renderer can draw a streak along the path traveled. */
    REFLECT()
    class CParticleModule_Trail : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Trail"; }
        FString GetCategory() const override { return "Render"; }
        FString GetTooltip() const override { return "Stretch each particle into a streak along the path it actually traveled. Overrides the emitter's Facing Mode."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        /** How far behind the particle the tail lags, in SECONDS of travel -- not world units, and not a
         *  segment count. A fraction of a second reads as motion blur; approaching the particle's lifetime
         *  the tail stays pinned near the spawn point and the streak spans the whole flight. Capped well
         *  short of any value that could only be a misreading of the unit. */
        PROPERTY(Editable, Category = "Trail", ClampMin = 0.0f, ClampMax = 5.0f)
        SParticleParam TrailLength { 0.1f };
    };

    /**
     * Integrates velocity into position (and spin into rotation). Normally the last module in the
     * Update stack so all forces for the frame are accounted for first ("Solve Forces and Velocity").
     */
    REFLECT()
    class CParticleModule_Integrate : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Solve Forces and Velocity"; }
        FString GetCategory() const override { return "Solve"; }
        FString GetTooltip() const override { return "Move particles by their velocity. Place last in the Update stack."; }
        uint32 GetAccentColor() const override { return IM_COL32(180, 140, 70, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };
}
