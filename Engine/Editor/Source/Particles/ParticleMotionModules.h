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

    // Pushes particles that crowd each other apart, the cheap way to stop a swarm collapsing into one point.
    REFLECT()
    class CParticleModule_ParticleSeparation : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Particle Separation"; }
        FString GetCategory() const override { return "Neighbors"; }
        FString GetTooltip() const override { return "Push particles away from others within Radius. Radius past the emitter's Neighbor Cell Size is cut to it."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Neighbors", ClampMin = 0.0f)
        SParticleParam Radius { 0.3f };

        // Meters per second squared between two particles touching.
        PROPERTY(Editable, Category = "Neighbors")
        SParticleParam Strength { 10.0f };
    };

    // Separation, alignment and cohesion, the three rules that make particles flock.
    REFLECT()
    class CParticleModule_Boids : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Boids"; }
        FString GetCategory() const override { return "Neighbors"; }
        FString GetTooltip() const override { return "Flock with neighbors within Radius, steering apart, alongside and together, capped at Max Speed."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // How far a particle sees, cut to the emitter's Neighbor Cell Size.
        PROPERTY(Editable, Category = "Neighbors", ClampMin = 0.0f)
        SParticleParam Radius { 0.5f };

        PROPERTY(Editable, Category = "Neighbors")
        SParticleParam Separation { 6.0f };

        // How quickly a particle matches its neighbors' heading.
        PROPERTY(Editable, Category = "Neighbors")
        SParticleParam Alignment { 2.0f };

        // How strongly a particle steers toward its neighbors' center.
        PROPERTY(Editable, Category = "Neighbors")
        SParticleParam Cohesion { 1.0f };

        PROPERTY(Editable, Category = "Neighbors", ClampMin = 0.0f)
        SParticleParam MaxSpeed { 3.0f };
    };

    REFLECT()
    class CParticleModule_PointForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Point Force"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Pull particles toward a point near the emitter, or push them away with a negative strength. Needs no component in the world."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Offset from the emitter in its frame, scaled with it.
        PROPERTY(Editable, Category = "Forces")
        SParticleParam Offset { FVector3(0.0f) };

        // Meters per second squared at full strength, negative to repel.
        PROPERTY(Editable, Category = "Forces")
        SParticleParam Strength { 5.0f };

        // Meters past which the force is gone, and 0 reaches everywhere.
        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam Radius { 0.0f };

        // How sharply the force fades toward Radius, 1 being linear.
        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam Falloff { 1.0f };

        // Ends particles that come this close to the point, for a drain or a black hole. 0 turns it off.
        PROPERTY(Editable, Category = "Forces", ClampMin = 0.0f)
        SParticleParam KillRadius { 0.0f };

        PROPERTY(Editable, Category = "Forces")
        bool bScaleByMass = false;
    };

    REFLECT()
    class CParticleModule_WindForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Wind Force"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Carry particles along a wind with gusts that sweep across them. The air also drags them toward the wind speed."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // World direction the wind blows toward.
        PROPERTY(Editable, Category = "Wind")
        SParticleParam Direction { FVector3(1.0f, 0.0f, 0.0f) };

        // Meters per second.
        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f)
        SParticleParam Speed { 4.0f };

        // How quickly particles take on the wind velocity, per second.
        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f)
        SParticleParam Resistance { 1.0f };

        // Fraction of Speed the gusts add or take away.
        PROPERTY(Editable, Category = "Gusts", ClampMin = 0.0f)
        SParticleParam Gustiness { 0.5f };

        // Gusts per second passing a fixed point.
        PROPERTY(Editable, Category = "Gusts", ClampMin = 0.0f)
        SParticleParam GustFrequency { 0.5f };

        // Gusts per meter, so neighboring particles feel different gusts.
        PROPERTY(Editable, Category = "Gusts", ClampMin = 0.0f)
        SParticleParam GustScale { 0.15f };

        PROPERTY(Editable, Category = "Wind")
        bool bScaleByMass = false;
    };

    REFLECT()
    class CParticleModule_PlaneCollision : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Collide With Plane"; }
        FString GetCategory() const override { return "Forces"; }
        FString GetTooltip() const override { return "Bounce off an infinite plane near the emitter, such as a floor, wherever the camera looks. Place it before Solve Forces and Velocity."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 120, 190, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // World direction the plane faces.
        PROPERTY(Editable, Category = "Collision")
        SParticleParam Normal { FVector3(0.0f, 1.0f, 0.0f) };

        // Meters from the emitter along Normal, scaled with it.
        PROPERTY(Editable, Category = "Collision")
        SParticleParam Height { 0.0f };

        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f, ClampMax = 1.0f)
        SParticleParam Restitution { 0.35f };

        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f, ClampMax = 1.0f)
        SParticleParam Friction { 0.3f };

        // Meters the particle keeps from the plane, scaled with the emitter.
        PROPERTY(Editable, Category = "Collision", ClampMin = 0.0f)
        SParticleParam Radius { 0.02f };

        PROPERTY(Editable, Category = "Collision")
        bool bKillOnHit = false;
    };
}
