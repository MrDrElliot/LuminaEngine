#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "ParticleCustomModules.generated.h"

namespace Lumina
{
    enum class EParticleContext : uint8;

    // A named value the code reads as a local constant, bindable to a user parameter like any stock input.
    REFLECT()
    struct SParticleCustomInput
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Input")
        FName Name;

        PROPERTY(Editable, Category = "Input")
        SParticleParam Value { 0.0f };
    };

    // A curve the code calls by name, Name(t) with t from 0 to 1.
    REFLECT()
    struct SParticleCustomCurve
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Curve")
        FName Name;

        PROPERTY(Editable, Category = "Curve")
        SCurve Curve;
    };

    // A color ramp the code calls by name, Name(t) with t from 0 to 1, returning a float4.
    REFLECT()
    struct SParticleCustomGradient
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Gradient")
        FName Name;

        PROPERTY(Editable, Category = "Gradient")
        SGradient Gradient;
    };

    // Pastes hand-written Slang into a stage, with every helper the stock modules use in scope.
    REFLECT()
    class CParticleModule_CustomCode : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        FString GetCategory() const override { return "Custom"; }
        uint32 GetAccentColor() const override { return IM_COL32(200, 110, 60, 255); }

        // Each becomes a constant of its type under its name.
        PROPERTY(Editable, Category = "Custom")
        TVector<SParticleCustomInput> Inputs;

        // Per-particle floats the code reads and writes by name, shared with any module declaring the same name.
        PROPERTY(Editable, Category = "Custom")
        TVector<FName> Attributes;

        // Grid channels the code names, read and written as a variable on a grid cell, or sampled at P.Position on a particle.
        PROPERTY(Editable, Category = "Custom")
        TVector<FName> GridChannels;

        // Arrays a script fills with SParticleSystemComponent::SetArray, read as ArrayAt(ARRAY_Name, Index) and ArrayCount(ARRAY_Name).
        PROPERTY(Editable, Category = "Custom")
        TVector<FName> Arrays;

        // Buckets particles by cell, for GatherNeighbors(Index, P.Position, Radius) or NeighborCount and NeighborAt over NeighborCellOf.
        PROPERTY(Editable, Category = "Custom")
        bool bUsesNeighbors = false;

        // Reshaping one is a value edit, so it never recompiles the shader.
        PROPERTY(Editable, Category = "Custom")
        TVector<SParticleCustomCurve> Curves;

        PROPERTY(Editable, Category = "Custom")
        TVector<SParticleCustomGradient> Gradients;

        PROPERTY(Editable, Multiline, Category = "Custom")
        FString Code;

    protected:
        void GenerateInto(FParticleCompiler& Compiler, EParticleContext Context);
    };

    REFLECT()
    class CParticleModule_CustomSpawn : public CParticleModule_CustomCode
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Custom Spawn Code"; }
        FString GetTooltip() const override { return "Run your own Slang once per particle at birth."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    REFLECT()
    class CParticleModule_CustomUpdate : public CParticleModule_CustomCode
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Custom Update Code"; }
        FString GetTooltip() const override { return "Run your own Slang on every live particle each frame."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    // Cell, Coord, UV, WorldPos, DeltaTime, TotalTime and Iteration are in scope, and GridAt reads neighbors.
    REFLECT(MinimalAPI)
    class CParticleModule_CustomGrid : public CParticleModule_CustomCode
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Grid; }
        FString GetDisplayName() const override { return "Custom Grid Code"; }
        FString GetTooltip() const override { return "Run your own Slang on every grid cell, reading neighbors with GridAt(Coord + int2(1, 0), GRID_Name)."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    // Cell holds the channels sampled at this texel, and Color is what the target receives.
    REFLECT()
    class CParticleModule_CustomTarget : public CParticleModule_CustomCode
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Target; }
        FString GetDisplayName() const override { return "Custom Target Code"; }
        FString GetTooltip() const override { return "Run your own Slang on every render target texel and set Color."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };
}
