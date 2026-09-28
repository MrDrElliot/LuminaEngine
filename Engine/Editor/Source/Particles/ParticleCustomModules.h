#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
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

    // Pastes hand-written Slang into a stage, with P, Seed, DeltaTime, LifeRatio and every helper the stock modules use in scope.
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
}
