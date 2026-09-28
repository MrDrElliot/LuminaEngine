#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "ParticleMotionModules.generated.h"

namespace Lumina
{
    // Caps speed, so forces and bursts cannot fling particles past a believable pace.
    REFLECT()
    class CParticleModule_SpeedLimit : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Speed Limit"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Clamp each particle's speed. Place it after the forces it should limit."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam MaxSpeed { 10.0f };
    };

    // Accelerates particles away from the emitter, or toward it with a negative strength.
    REFLECT()
    class CParticleModule_RadialAcceleration : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_RadialAcceleration();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Radial Acceleration"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Push particles outward from the emitter, or pull them in when negative."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces")
        SParticleParam Strength { 5.0f };

        // Multiplies Strength across the particle's life.
        PROPERTY(Editable, Category = "Forces")
        SCurve OverLife;
    };

    // Accelerates particles around the emitter's up axis, for swirls and vortices.
    REFLECT()
    class CParticleModule_TangentialAcceleration : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_TangentialAcceleration();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Tangential Acceleration"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Push particles around the emitter's up axis."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces")
        SParticleParam Strength { 5.0f };

        PROPERTY(Editable, Category = "Forces")
        SCurve OverLife;
    };

    // Moves particles around the emitter's up axis without touching their velocity, the way Godot's orbit velocity does.
    REFLECT()
    class CParticleModule_OrbitVelocity : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_OrbitVelocity();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Orbit Velocity"; }
        FString GetCategory() const override { return "Velocity"; }
        FString GetTooltip() const override { return "Circle particles around the emitter's up axis, in turns per second."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Velocity")
        SParticleParam TurnsPerSecond { 0.5f };

        PROPERTY(Editable, Category = "Velocity")
        SCurve OverLife;
    };

    // Adds motion on top of the simulated velocity, shaped over life and in the emitter's frame.
    REFLECT()
    class CParticleModule_VelocityOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_VelocityOverLife();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Velocity Over Life"; }
        FString GetCategory() const override { return "Velocity"; }
        FString GetTooltip() const override { return "Move particles along a direction in the emitter's frame, scaled by a curve over life."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 160, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Velocity")
        SParticleParam Velocity { FVector3(0.0f, 1.0f, 0.0f) };

        PROPERTY(Editable, Category = "Velocity")
        SCurve OverLife;
    };

    // Turns sprites and meshes over life, independent of the spin they spawned with.
    REFLECT()
    class CParticleModule_SpinOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_SpinOverLife();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Spin Over Life"; }
        FString GetCategory() const override { return "Rotation"; }
        FString GetTooltip() const override { return "Rotate particles by a curve-shaped rate in degrees per second."; }
        uint32 GetAccentColor() const override { return IM_COL32(190, 150, 70, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Rotation")
        SParticleParam DegreesPerSecond { 90.0f };

        PROPERTY(Editable, Category = "Rotation")
        SCurve OverLife;
    };

    // Drag that changes over life, so a burst can fly free and then stall.
    REFLECT()
    class CParticleModule_DampingOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_DampingOverLife();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Damping Over Life"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Slow particles by a drag that follows a curve over life."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam Damping { 2.0f };

        PROPERTY(Editable, Category = "Forces")
        SCurve OverLife;
    };

    // Stretches sprites on each axis over life, where Size Over Life scales both together.
    REFLECT()
    class CParticleModule_ScaleOverLife : public CParticleModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_ScaleOverLife();
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Scale Over Life"; }
        FString GetCategory() const override { return "Size"; }
        FString GetTooltip() const override { return "Scale sprite width and height separately across the particle's life."; }
        uint32 GetAccentColor() const override { return IM_COL32(190, 150, 70, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Size")
        SCurve Width;

        PROPERTY(Editable, Category = "Size")
        SCurve Height;
    };

    // Shifts each particle's hue by a random amount, so one color reads as a natural spread.
    REFLECT()
    class CParticleModule_HueVariation : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Hue Variation"; }
        FString GetCategory() const override { return "Color"; }
        FString GetTooltip() const override { return "Randomly rotate the spawn color's hue. Color Over Life replaces it unless Scale Spawn Color is on."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Largest shift either way, as a fraction of the full hue circle.
        PROPERTY(Editable, Category = "Color", ClampMin = 0.0f, ClampMax = 0.5f)
        SParticleParam Variation { 0.05f };
    };

    // Opts the emitter into every Particle Attractor component in the world.
    REFLECT()
    class CParticleModule_AttractorForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Attractor Force"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Let Particle Attractor components in the world pull on these particles."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Multiplies every attractor's strength for this emitter.
        PROPERTY(Editable, Category = "Forces")
        SParticleParam Scale { 1.0f };
    };

    // Bounces particles off Particle Collider components and terrain, which work off-screen unlike Collide With Scene.
    REFLECT()
    class CParticleModule_ShapeCollision : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Collide With Shapes"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Bounce off Particle Collider components and terrain. Place it before Solve Forces and Velocity."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f, ClampMax = 1.0f)
        SParticleParam Restitution { 0.35f };

        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f, ClampMax = 1.0f)
        SParticleParam Friction { 0.3f };

        // Meters the particle keeps from a surface, scaled with the emitter.
        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f)
        SParticleParam Radius { 0.05f };

        PROPERTY(Editable, Category = "Collision")
        bool bCollideWithTerrain = true;

        PROPERTY(Editable, Category = "Collision")
        bool bKillOnHit = false;
    };
}
