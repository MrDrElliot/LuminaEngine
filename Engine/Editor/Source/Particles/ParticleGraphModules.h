#pragma once

#include "ParticleModule.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Particle/ParticleNodeGraph.h"
#include "ParticleGraphModules.generated.h"

namespace Lumina
{
    enum class EParticleContext : uint8;

    // A module built from nodes instead of Slang, edited in the particle editor's Module Graph panel.
    REFLECT(MinimalAPI)
    class CParticleModule_Graph : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        FString GetDisplayName() const override { return ModuleName.empty() ? FString("Graph Module") : ModuleName; }
        FString GetCategory() const override { return "Custom"; }
        uint32 GetAccentColor() const override { return IM_COL32(110, 160, 225, 255); }
        void GetOwnedObjects(TVector<CObject*>& OutObjects) const override;

        // Made on first use with nodes suited to the stack it sits in.
        EDITOR_API CParticleNodeGraph* GetGraph();

        // Shown on the stack row, so several graph modules in one stack can be told apart.
        PROPERTY(Editable, Category = "Graph")
        FString ModuleName = "Graph Module";

    protected:
        void GenerateInto(FParticleCompiler& Compiler, EParticleContext Context);
        virtual void SeedGraph(CParticleNodeGraph* NewGraph) { }

        PROPERTY(Hidden)
        TStrongObjectPtr<CParticleNodeGraph> Graph;
    };

    REFLECT(MinimalAPI)
    class CParticleModule_GraphSpawn : public CParticleModule_Graph
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetTooltip() const override { return "Build what happens at a particle's birth from nodes."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

    protected:
        void SeedGraph(CParticleNodeGraph* NewGraph) override;
    };

    REFLECT(MinimalAPI)
    class CParticleModule_GraphUpdate : public CParticleModule_Graph
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetTooltip() const override { return "Build what happens to every live particle each frame from nodes."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

    protected:
        void SeedGraph(CParticleNodeGraph* NewGraph) override;
    };

    REFLECT(MinimalAPI)
    class CParticleModule_GraphGrid : public CParticleModule_Graph
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Grid; }
        FString GetTooltip() const override { return "Build a pass over every grid cell from nodes."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

    protected:
        void SeedGraph(CParticleNodeGraph* NewGraph) override;
    };

    REFLECT(MinimalAPI)
    class CParticleModule_GraphTarget : public CParticleModule_Graph
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Target; }
        FString GetTooltip() const override { return "Build the color of every render target texel from nodes."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

    protected:
        void SeedGraph(CParticleNodeGraph* NewGraph) override;
    };
}
