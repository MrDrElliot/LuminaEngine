#pragma once

#include "UI/Tools/NodeGraph/Particle/ParticleGraphNode.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "ParticleGraphNodes.generated.h"

namespace Lumina
{
    enum class EParticleContext : uint8;
    struct FMaterialFormula;

    // The particle's values from before the module runs.
    REFLECT(MinimalAPI)
    class CParticleNode_Particle : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Particle"; }
        FStringView GetNodeDisplayName() const override { return "Particle"; }
        FStringView GetNodeTooltip() const override { return "Read the particle's position, velocity, color, size, age and seed."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;
    };

    // Where the emitter is, how it is turned and the simulation clock.
    REFLECT()
    class CParticleNode_Emitter : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Particle"; }
        FStringView GetNodeDisplayName() const override { return "Emitter"; }
        FStringView GetNodeTooltip() const override { return "Read the emitter's position and axes, its scale, and the time step and clock."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;
    };

    // A module input, editable without a recompile and bindable to a user parameter.
    REFLECT(MinimalAPI)
    class CParticleNode_Parameter : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "Parameter"; }
        FStringView GetNodeTooltip() const override { return "A value set in the details panel, which a user parameter can drive."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Parameter")
        SParticleParam Value { 1.0f };
    };

    // A random value between Min and Max, fixed per particle or per cell.
    REFLECT()
    class CParticleNode_Random : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "Random"; }
        FStringView GetNodeTooltip() const override { return "A random value between Min and Max that stays the same for a particle or a cell."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;
    };

    REFLECT()
    class CParticleNode_GetAttribute : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Particle"; }
        FStringView GetNodeDisplayName() const override { return "Get Attribute"; }
        FStringView GetNodeTooltip() const override { return "Read a per-particle float that a module or custom code declared by name."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Attribute")
        FName AttributeName = FName("Custom");
    };

    // A value a script supplied with SParticleSystemComponent::SetArray, by index.
    REFLECT()
    class CParticleNode_ArrayElement : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Inputs"; }
        FStringView GetNodeDisplayName() const override { return "Array Element"; }
        FStringView GetNodeTooltip() const override { return "Read an entry of an array a script fills with SetArray, and how many entries it has."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Array")
        FName ArrayName = FName("Points");
    };

    // A grid channel and its slope at a position, where the particle or cell is by default.
    REFLECT()
    class CParticleNode_SampleGrid : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Grid"; }
        FStringView GetNodeDisplayName() const override { return "Sample Grid"; }
        FStringView GetNodeTooltip() const override { return "Read a grid channel and its slope where the particle or cell is, or at Position."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");
    };

    // A channel of the cell this stage is updating, including what earlier modules wrote to it.
    REFLECT(MinimalAPI)
    class CParticleNode_Cell : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Grid"; }
        FStringView GetNodeDisplayName() const override { return "Cell"; }
        FStringView GetNodeTooltip() const override { return "Read a channel of this grid cell, with its neighbors' average and Laplacian."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");
    };

    REFLECT()
    class CParticleNode_GridNeighbor : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Grid"; }
        FStringView GetNodeDisplayName() const override { return "Grid Neighbor"; }
        FStringView GetNodeTooltip() const override { return "Read a channel of the cell at an offset from this one, as the previous stage left it."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Grid")
        int32 OffsetX = 1;

        PROPERTY(Editable, Category = "Grid")
        int32 OffsetY = 0;
    };

    REFLECT()
    class CParticleNode_CellInfo : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Grid"; }
        FStringView GetNodeDisplayName() const override { return "Cell Info"; }
        FStringView GetNodeTooltip() const override { return "Where this cell or texel is, in grid UV and in the world, and which iteration of the stage is running."; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;
    };

    // One of the material graph's formulas, so the particle graph shares its whole math and noise library.
    REFLECT(MinimalAPI)
    class CParticleNode_Formula : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override;
        FStringView GetNodeDisplayName() const override;
        FStringView GetNodeTooltip() const override;
        void GetVariants(TVector<FGraphNodeVariant>& OutVariants) const override;
        void ApplyVariant(FName Variant) override { FormulaClass = Variant; }
        void BuildNode() override;
        void GenerateDefinition(FParticleCompiler& Compiler) override;

        // The material formula node class this node runs.
        PROPERTY(Hidden)
        FName FormulaClass;

    private:
        // Found once, since the title and tooltip ask for it every frame the node draws.
        const FMaterialFormula* GetFormula() const;
        mutable const FMaterialFormula* CachedFormula = nullptr;
    };

    // Shared by the nodes that write, each reading every connected input first and writing once all sinks have read.
    REFLECT(NotPlaceable)
    class CParticleSinkNode : public CParticleGraphNode
    {
        GENERATED_BODY()
    public:
        bool IsSink() const override { return true; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(150, 70, 60, 255); }
        void GenerateDefinition(FParticleCompiler& Compiler) override;
        void EmitWrites(FParticleCompiler& Compiler) override;

    protected:
        // The value each connected input pin was read into, by pin index.
        FString InputVar(int32 Pin);
        bool IsWired(int32 Pin) const;
        virtual void Write(FParticleCompiler& Compiler) { }
    };

    // Writes only the values that are wired.
    REFLECT(MinimalAPI)
    class CParticleNode_SetParticle : public CParticleSinkNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Write"; }
        FStringView GetNodeDisplayName() const override { return "Set Particle"; }
        FStringView GetNodeTooltip() const override { return "Write the particle's position, velocity, color, size, rotation or lifetime. Unwired pins leave theirs alone."; }
        void BuildNode() override;

    protected:
        void Write(FParticleCompiler& Compiler) override;
    };

    REFLECT()
    class CParticleNode_SetAttribute : public CParticleSinkNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Write"; }
        FStringView GetNodeDisplayName() const override { return "Set Attribute"; }
        FStringView GetNodeTooltip() const override { return "Write a per-particle float by name, shared with every module naming the same one."; }
        void BuildNode() override;

        PROPERTY(Editable, Category = "Attribute")
        FName AttributeName = FName("Custom");

    protected:
        void Write(FParticleCompiler& Compiler) override;
    };

    REFLECT(MinimalAPI)
    class CParticleNode_WriteToGrid : public CParticleSinkNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Write"; }
        FStringView GetNodeDisplayName() const override { return "Write To Grid"; }
        FStringView GetNodeTooltip() const override { return "Add Value to a grid channel where the particle is, or at Position."; }
        void BuildNode() override;

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");

    protected:
        void Write(FParticleCompiler& Compiler) override;
    };

    REFLECT(MinimalAPI)
    class CParticleNode_SetCell : public CParticleSinkNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Write"; }
        FStringView GetNodeDisplayName() const override { return "Set Cell"; }
        FStringView GetNodeTooltip() const override { return "Write a channel of this grid cell."; }
        void BuildNode() override;

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");

    protected:
        void Write(FParticleCompiler& Compiler) override;
    };

    REFLECT(MinimalAPI)
    class CParticleNode_SetTargetColor : public CParticleSinkNode
    {
        GENERATED_BODY()
    public:
        FFixedString GetNodeCategory() const override { return "Write"; }
        FStringView GetNodeDisplayName() const override { return "Set Target Color"; }
        FStringView GetNodeTooltip() const override { return "Write the color this render target texel receives."; }
        void BuildNode() override;

    protected:
        void Write(FParticleCompiler& Compiler) override;
    };
}
