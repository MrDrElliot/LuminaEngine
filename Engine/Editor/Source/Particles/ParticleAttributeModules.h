#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "ParticleAttributeModules.generated.h"

namespace Lumina
{
    enum class EParticleContext : uint8;

    REFLECT()
    enum class EParticleValueTarget : uint8
    {
        Velocity,
        Color,
        Alpha,
        Size,
        // Degrees.
        Rotation,
        // Degrees per second.
        RotationSpeed,
        Lifetime,
        // A per-particle float any module or custom code declares by name.
        Attribute,
    };

    REFLECT()
    enum class EParticleValueOperation : uint8
    {
        Set,
        Add,
        Multiply,
    };

    // Writes one particle value from a constant or a random range, the general tool for anything no dedicated module covers.
    REFLECT()
    class CParticleModule_SetValue : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        FString GetCategory() const override { return "General"; }
        uint32 GetAccentColor() const override { return IM_COL32(120, 120, 140, 255); }

        PROPERTY(Editable, Category = "Value")
        EParticleValueTarget Target = EParticleValueTarget::Velocity;

        PROPERTY(Editable, Category = "Value", EditCondition = "Target == Attribute", EditConditionHides)
        FName AttributeName;

        PROPERTY(Editable, Category = "Value")
        EParticleValueOperation Operation = EParticleValueOperation::Set;

        // Velocity is in meters per second along the emitter's right, up and forward when Emitter Space is on, else world axes.
        PROPERTY(Editable, Category = "Value")
        SParticleParam Value { FVector3(0.0f, 1.0f, 0.0f) };

        // Picks per particle between Value and Max, held for the particle's whole life.
        PROPERTY(Editable, Category = "Value")
        bool bRandomRange = false;

        PROPERTY(Editable, Category = "Value", EditCondition = "bRandomRange", EditConditionHides)
        SParticleParam Max { FVector3(0.0f, 2.0f, 0.0f) };

        PROPERTY(Editable, Category = "Value", EditCondition = "Target == Velocity", EditConditionHides)
        bool bEmitterSpace = false;

    protected:
        // The value's width follows the target, which is also what picks how many fields the editor shows.
        void SyncValueWidth();
        void GenerateInto(FParticleCompiler& Compiler, EParticleContext Context, int32 ModuleIndex, const FString& Scale, bool bPerSecond);
    };

    REFLECT()
    class CParticleModule_SetValueSpawn : public CParticleModule_SetValue
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Set Particle Value"; }
        FString GetTooltip() const override { return "Set, add to or scale any particle value at birth, from a constant or a random range."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    REFLECT()
    class CParticleModule_SetValueUpdate : public CParticleModule_SetValue
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Update Particle Value"; }
        FString GetTooltip() const override { return "Set, add to or scale any particle value every frame, optionally shaped by a curve over life."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        CParticleModule_SetValueUpdate();

        // Add accumulates and Multiply compounds this much per second rather than per frame, so the result does not depend on the frame rate.
        PROPERTY(Editable, Category = "Value", EditCondition = "Operation != Set", EditConditionHides)
        bool bPerSecond = true;

        PROPERTY(Editable, Category = "Curve")
        bool bScaleByCurve = false;

        // Multiplies the value across the particle's life.
        PROPERTY(Editable, Category = "Curve", EditCondition = "bScaleByCurve", EditConditionHides)
        SCurve Curve;
    };

    REFLECT()
    enum class EParticleKillShape : uint8
    {
        Sphere,
        Box,
        // Everything below the plane through Center that faces along the emitter's up.
        Plane,
    };

    REFLECT()
    class CParticleModule_KillInVolume : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Kill In Volume"; }
        FString GetCategory() const override { return "Kill"; }
        FString GetTooltip() const override { return "End particles that enter, or leave, a sphere, box or half-space around the emitter. They still raise their death event."; }
        uint32 GetAccentColor() const override { return IM_COL32(190, 70, 70, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Kill")
        EParticleKillShape Shape = EParticleKillShape::Sphere;

        // Off kills what leaves the volume instead, to keep an effect inside its bounds.
        PROPERTY(Editable, Category = "Kill")
        bool bKillInside = false;

        // Offset from the emitter in its frame, scaled with it.
        PROPERTY(Editable, Category = "Kill")
        SParticleParam Center { FVector3(0.0f) };

        PROPERTY(Editable, Category = "Kill", ClampMin = 0.0f, EditCondition = "Shape == Sphere", EditConditionHides)
        SParticleParam Radius { 5.0f };

        PROPERTY(Editable, Category = "Kill", ClampMin = 0.0f, EditCondition = "Shape == Box", EditConditionHides)
        SParticleParam HalfExtent { FVector3(2.5f) };
    };

    // Places each new particle at a point a script supplied, for point clouds, plotted data and spawns at game positions.
    REFLECT()
    class CParticleModule_SpawnFromArray : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Spawn From Array"; }
        FString GetCategory() const override { return "Location"; }
        FString GetTooltip() const override { return "Put each new particle at a point from an array a script fills with SParticleSystemComponent::SetArray."; }
        uint32 GetAccentColor() const override { return IM_COL32(90, 150, 90, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Each value's xyz is a position, and its w the particle's size when Size From W is on.
        PROPERTY(Editable, Category = "Array")
        FName Positions = FName("Points");

        // Reads positions as offsets from the emitter that turn and scale with it, rather than as world positions.
        PROPERTY(Editable, Category = "Array")
        bool bEmitterSpace = true;

        PROPERTY(Editable, Category = "Array")
        bool bSizeFromW = false;

        // A second array whose value at the same index colors the particle. Empty leaves the color alone.
        PROPERTY(Editable, Category = "Array")
        FName Colors;

        // Picks an entry at random instead of by the particle's slot, which a burst of one particle per entry wants off.
        PROPERTY(Editable, Category = "Array")
        bool bRandomPick = false;
    };

    REFLECT()
    class CParticleModule_KillBySpeed : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Kill By Speed"; }
        FString GetCategory() const override { return "Kill"; }
        FString GetTooltip() const override { return "End particles that slow below, or speed past, a threshold, such as debris that has come to rest."; }
        uint32 GetAccentColor() const override { return IM_COL32(190, 70, 70, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Meters per second. Particles slower than this end, and 0 turns the check off.
        PROPERTY(Editable, Category = "Kill", ClampMin = 0.0f)
        SParticleParam MinSpeed { 0.05f };

        // Meters per second. Particles faster than this end, and 0 turns the check off.
        PROPERTY(Editable, Category = "Kill", ClampMin = 0.0f)
        SParticleParam MaxSpeed { 0.0f };

        // Seconds of life before the check applies, so a particle born at rest is not ended on its first frame.
        PROPERTY(Editable, Category = "Kill", ClampMin = 0.0f)
        SParticleParam MinAge { 0.25f };
    };

    REFLECT()
    enum class EParticleSpeedBlend : uint8
    {
        // Overwrites the value each frame.
        Replace,
        // Scales the value the spawn stack left, kept from birth.
        ScaleSpawnValue,
        // Scales whatever earlier update modules set this frame. Use it only after one that writes the value every frame.
        MultiplyCurrent,
    };

    REFLECT()
    class CParticleModule_ColorBySpeed : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Color By Speed"; }
        FString GetCategory() const override { return "Color"; }
        FString GetTooltip() const override { return "Color particles from a gradient across a speed range, such as sparks cooling as they slow."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        CParticleModule_ColorBySpeed();

        PROPERTY(Editable, Category = "Color")
        SGradient Gradient;

        // Meters per second at the gradient's left end.
        PROPERTY(Editable, Category = "Color", ClampMin = 0.0f)
        SParticleParam MinSpeed { 0.0f };

        // Meters per second at the gradient's right end.
        PROPERTY(Editable, Category = "Color", ClampMin = 0.0f)
        SParticleParam MaxSpeed { 10.0f };

        PROPERTY(Editable, Category = "Color")
        EParticleSpeedBlend Blend = EParticleSpeedBlend::Replace;
    };

    REFLECT()
    class CParticleModule_SizeBySpeed : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Size By Speed"; }
        FString GetCategory() const override { return "Size"; }
        FString GetTooltip() const override { return "Scale particles by a curve across a speed range, such as embers that shrink as they slow."; }
        uint32 GetAccentColor() const override { return IM_COL32(150, 90, 180, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        CParticleModule_SizeBySpeed();

        // Size multiplier across the speed range.
        PROPERTY(Editable, Category = "Size")
        SCurve Curve;

        PROPERTY(Editable, Category = "Size", ClampMin = 0.0f)
        SParticleParam MinSpeed { 0.0f };

        PROPERTY(Editable, Category = "Size", ClampMin = 0.0f)
        SParticleParam MaxSpeed { 10.0f };

        PROPERTY(Editable, Category = "Size")
        EParticleSpeedBlend Blend = EParticleSpeedBlend::ScaleSpawnValue;
    };
}
