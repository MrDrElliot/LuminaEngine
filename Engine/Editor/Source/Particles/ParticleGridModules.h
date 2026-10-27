#pragma once

#include "ParticleModule.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "ParticleGridModules.generated.h"

namespace Lumina
{
    enum class EParticleContext : uint8;

    REFLECT()
    enum class EParticleGridBrushShape : uint8
    {
        Everywhere,
        // A disc around Center, Radius out in grid UV.
        Circle,
        // A square around Center, Radius out along each axis in grid UV.
        Box,
        // The outermost ring of cells, for walls and drains.
        Border,
    };

    REFLECT()
    enum class EParticleGridOperation : uint8
    {
        Set,
        Add,
        Multiply,
        Max,
    };

    REFLECT()
    enum class EParticleGridWriteMode : uint8
    {
        // Adds Amount to Channel.
        Value,
        // Adds the particle's velocity across the grid, times Amount, to Velocity X and Velocity Y.
        Velocity,
    };

    REFLECT()
    enum class EParticleGridForceMode : uint8
    {
        // Accelerates up the channel's slope, or down it with a negative strength.
        Gradient,
        // Pulls each particle's motion across the grid toward the velocity in Velocity X and Velocity Y.
        VelocityField,
    };

    REFLECT()
    enum class EParticleGridSampleTarget : uint8
    {
        Alpha,
        Size,
        // Scales or sets the particle's RGB.
        Brightness,
        // A per-particle float any module or custom code declares by name.
        Attribute,
    };

    // Begins a pass over the grid, so the modules under it run together, Iterations times a frame.
    REFLECT(MinimalAPI)
    class CParticleModule_SimulationStage : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Grid; }
        FString GetDisplayName() const override { return "Simulation Stage"; }
        FString GetCategory() const override { return "Stages"; }
        FString GetTooltip() const override { return "Start a new pass over the grid. Each iteration sees everything the previous one wrote."; }
        uint32 GetAccentColor() const override { return IM_COL32(225, 185, 80, 255); }

        PROPERTY(Editable, Category = "Stage")
        FString StageName = "Stage";

        // Passes a frame, as for a pressure solve that converges over many. Zero skips the stage.
        PROPERTY(Editable, Category = "Stage", ClampMin = 0, ClampMax = 256)
        int32 Iterations = 1;
    };

    // Shared by the modules that run on grid cells.
    REFLECT()
    class CParticleGridModule : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Grid; }
        FString GetCategory() const override { return "Grid"; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 175, 170, 255); }
    };

    // Paints a value into a channel, the way to inject density, heat or a ripple from a script parameter.
    REFLECT(MinimalAPI)
    class CParticleModule_GridBrush : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Grid Brush"; }
        FString GetTooltip() const override { return "Set, add or scale a channel inside a shape. Bind Center to a parameter to paint from script."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Brush")
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Brush")
        EParticleGridOperation Operation = EParticleGridOperation::Add;

        PROPERTY(Editable, Category = "Brush")
        SParticleParam Value { 1.0f };

        // Treats an added Value as an amount per second, for a steady source.
        PROPERTY(Editable, Category = "Brush", EditCondition = "Operation == Add")
        bool bPerSecond = true;

        PROPERTY(Editable, Category = "Brush")
        EParticleGridBrushShape Shape = EParticleGridBrushShape::Circle;

        // Grid UV the shape is centered on, zero to one across the grid.
        PROPERTY(Editable, Category = "Brush")
        SParticleParam Center { FVector2(0.5f, 0.5f) };

        PROPERTY(Editable, Category = "Brush")
        SParticleParam Radius { 0.08f };

        // Zero fades all the way from the center to the edge, and one cuts off hard at the edge.
        PROPERTY(Editable, Category = "Brush", ClampMin = 0.0f, ClampMax = 1.0f)
        float Hardness = 0.5f;
    };

    // Evens a channel out with its neighbors, the way heat and smoke spread.
    REFLECT()
    class CParticleModule_GridDiffuse : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Grid Diffuse"; }
        FString GetTooltip() const override { return "Blur a channel into its neighbors over time."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Diffuse")
        FName Channel = FName("Density");

        // How far toward its neighbors' average a cell moves each second.
        PROPERTY(Editable, Category = "Diffuse", ClampMin = 0.0f)
        SParticleParam Rate { 5.0f };
    };

    // Eases a channel toward a resting value, so trails and deposits fade.
    REFLECT(MinimalAPI)
    class CParticleModule_GridDecay : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Grid Decay"; }
        FString GetTooltip() const override { return "Fade a channel toward a resting value at a rate per second."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Decay")
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Decay", ClampMin = 0.0f)
        SParticleParam Rate { 1.0f };

        PROPERTY(Editable, Category = "Decay")
        SParticleParam Target { 0.0f };
    };

    // Carries channels along a velocity field, the transport step of a fluid.
    REFLECT()
    class CParticleModule_GridAdvect : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_GridAdvect();
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Grid Advect"; }
        FString GetTooltip() const override { return "Move channels along the grid's velocity, plus a constant wind."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // Include the velocity channels themselves for a fluid that carries its own motion.
        PROPERTY(Editable, Category = "Advect")
        TVector<FName> Channels;

        // Meters per second along the emitter's right axis. Leave both empty to move by Wind alone.
        PROPERTY(Editable, Category = "Advect")
        FName VelocityX = FName("VelocityX");

        // Meters per second along the emitter's forward axis.
        PROPERTY(Editable, Category = "Advect")
        FName VelocityY = FName("VelocityY");

        PROPERTY(Editable, Category = "Advect")
        SParticleParam Wind { FVector2(0.0f, 0.0f) };
    };

    // Measures how much a velocity field spreads out of each cell, which the pressure solve then removes.
    REFLECT()
    class CParticleModule_FluidDivergence : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Fluid Divergence"; }
        FString GetCategory() const override { return "Fluid"; }
        FString GetTooltip() const override { return "Compute the divergence of the velocity. Put it in a stage before Fluid Pressure Solve."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Fluid")
        FName VelocityX = FName("VelocityX");

        PROPERTY(Editable, Category = "Fluid")
        FName VelocityY = FName("VelocityY");

        PROPERTY(Editable, Category = "Fluid")
        FName Divergence = FName("Divergence");
    };

    // One Jacobi step toward the pressure that cancels the divergence, so its stage wants many iterations.
    REFLECT(MinimalAPI)
    class CParticleModule_FluidPressure : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Fluid Pressure Solve"; }
        FString GetCategory() const override { return "Fluid"; }
        FString GetTooltip() const override { return "Relax pressure against divergence. Give its stage 20 to 60 iterations."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Fluid")
        FName Pressure = FName("Pressure");

        PROPERTY(Editable, Category = "Fluid")
        FName Divergence = FName("Divergence");
    };

    // Subtracts the pressure gradient, leaving a velocity field that swirls instead of spreading.
    REFLECT()
    class CParticleModule_FluidProject : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Fluid Project"; }
        FString GetCategory() const override { return "Fluid"; }
        FString GetTooltip() const override { return "Remove the pressure gradient from the velocity. Put it in a stage after the pressure solve."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Fluid")
        FName VelocityX = FName("VelocityX");

        PROPERTY(Editable, Category = "Fluid")
        FName VelocityY = FName("VelocityY");

        PROPERTY(Editable, Category = "Fluid")
        FName Pressure = FName("Pressure");
    };

    // The wave equation, for ripples on water and shock rings.
    REFLECT()
    class CParticleModule_GridWave : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Grid Wave"; }
        FString GetTooltip() const override { return "Propagate ripples through a height channel. Add height with a brush or Write To Grid."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Wave")
        FName Height = FName("Height");

        // How fast each cell's height is changing, which the wave keeps between frames.
        PROPERTY(Editable, Category = "Wave")
        FName HeightVelocity = FName("HeightVelocity");

        // Cells a ripple crosses per second, capped where the solve would turn unstable.
        PROPERTY(Editable, Category = "Wave", ClampMin = 0.0f)
        SParticleParam Speed { 30.0f };

        // Fraction of motion lost per second.
        PROPERTY(Editable, Category = "Wave", ClampMin = 0.0f)
        SParticleParam Damping { 0.5f };
    };

    // Gray-Scott reaction diffusion, for coral, spots and stripes that grow on their own.
    REFLECT()
    class CParticleModule_ReactionDiffusion : public CParticleGridModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Reaction Diffusion"; }
        FString GetTooltip() const override { return "Grow patterns from two chemicals. Seed B with a brush and run the stage several iterations a frame."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Reaction")
        FName ChemicalA = FName("ChemicalA");

        PROPERTY(Editable, Category = "Reaction")
        FName ChemicalB = FName("ChemicalB");

        // How fast A is replenished, which with Kill picks the pattern.
        PROPERTY(Editable, Category = "Reaction", ClampMin = 0.0f)
        SParticleParam Feed { 0.055f };

        PROPERTY(Editable, Category = "Reaction", ClampMin = 0.0f)
        SParticleParam Kill { 0.062f };

        PROPERTY(Editable, Category = "Reaction", ClampMin = 0.0f)
        SParticleParam DiffusionA { 1.0f };

        PROPERTY(Editable, Category = "Reaction", ClampMin = 0.0f)
        SParticleParam DiffusionB { 0.5f };

        // Reaction steps per iteration, which grows the pattern faster at the cost of stability above one.
        PROPERTY(Editable, Category = "Reaction", ClampMin = 0.0f)
        SParticleParam Speed { 1.0f };
    };

    // Adds to the grid where particles are, so they leave trails or push a fluid.
    REFLECT(MinimalAPI)
    class CParticleModule_WriteToGrid : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Write To Grid"; }
        FString GetCategory() const override { return "Grid"; }
        FString GetTooltip() const override { return "Deposit a value or the particle's velocity into the emitter's grid."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 175, 170, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Grid")
        EParticleGridWriteMode Mode = EParticleGridWriteMode::Value;

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == Value", EditConditionHides)
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == Velocity", EditConditionHides)
        FName VelocityX = FName("VelocityX");

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == Velocity", EditConditionHides)
        FName VelocityY = FName("VelocityY");

        PROPERTY(Editable, Category = "Grid")
        SParticleParam Amount { 1.0f };

        // Treats Amount as a rate, so the deposit does not depend on the frame rate.
        PROPERTY(Editable, Category = "Grid")
        bool bPerSecond = true;
    };

    // Moves particles by the grid, up a slope or along a stored velocity.
    REFLECT()
    class CParticleModule_GridForce : public CParticleModule
    {
        GENERATED_BODY()
    public:
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Grid Force"; }
        FString GetCategory() const override { return "Grid"; }
        FString GetTooltip() const override { return "Steer particles by a channel's slope or by the grid's velocity field."; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 175, 170, 255); }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Grid")
        EParticleGridForceMode Mode = EParticleGridForceMode::Gradient;

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == Gradient", EditConditionHides)
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == VelocityField", EditConditionHides)
        FName VelocityX = FName("VelocityX");

        PROPERTY(Editable, Category = "Grid", EditCondition = "Mode == VelocityField", EditConditionHides)
        FName VelocityY = FName("VelocityY");

        PROPERTY(Editable, Category = "Grid")
        SParticleParam Strength { 5.0f };
    };

    // Reads a channel under each particle into one of its values.
    REFLECT()
    class CParticleModule_SampleGrid : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        FString GetCategory() const override { return "Grid"; }
        uint32 GetAccentColor() const override { return IM_COL32(70, 175, 170, 255); }

        PROPERTY(Editable, Category = "Grid")
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Grid")
        EParticleGridSampleTarget Target = EParticleGridSampleTarget::Alpha;

        PROPERTY(Editable, Category = "Grid", EditCondition = "Target == Attribute", EditConditionHides)
        FName AttributeName;

        PROPERTY(Editable, Category = "Grid")
        EParticleGridOperation Operation = EParticleGridOperation::Multiply;

        // The sample is multiplied by Scale and then has Bias added before it is applied.
        PROPERTY(Editable, Category = "Grid")
        SParticleParam Scale { 1.0f };

        PROPERTY(Editable, Category = "Grid")
        SParticleParam Bias { 0.0f };

    protected:
        void GenerateInto(FParticleCompiler& Compiler, EParticleContext Context, int32 ModuleIndex);
    };

    REFLECT()
    class CParticleModule_SampleGridSpawn : public CParticleModule_SampleGrid
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Spawn; }
        FString GetDisplayName() const override { return "Sample Grid"; }
        FString GetTooltip() const override { return "Read a grid channel where the particle is born into its alpha, size, brightness or an attribute."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    REFLECT()
    class CParticleModule_SampleGridUpdate : public CParticleModule_SampleGrid
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Update; }
        FString GetDisplayName() const override { return "Sample Grid"; }
        FString GetTooltip() const override { return "Read a grid channel under the particle into its alpha, size, brightness or an attribute every frame."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;
    };

    // Shared by the modules that pick the render target's color.
    REFLECT()
    class CParticleTargetModule : public CParticleModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return false; }
        EParticleModuleStage GetStage() const override { return EParticleModuleStage::Target; }
        FString GetCategory() const override { return "Render Target"; }
        uint32 GetAccentColor() const override { return IM_COL32(190, 120, 200, 255); }
    };

    // Colors the target by running a channel through a gradient.
    REFLECT(MinimalAPI)
    class CParticleModule_TargetGradient : public CParticleTargetModule
    {
        GENERATED_BODY()
    public:
        CParticleModule_TargetGradient();
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Channel To Gradient"; }
        FString GetTooltip() const override { return "Map a channel from Min to Max through a color gradient."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Target")
        FName Channel = FName("Density");

        PROPERTY(Editable, Category = "Target")
        SParticleParam Min { 0.0f };

        PROPERTY(Editable, Category = "Target")
        SParticleParam Max { 1.0f };

        PROPERTY(Editable, Category = "Target")
        SGradient Gradient;
    };

    // Routes channels straight into the target's red, green, blue and alpha.
    REFLECT()
    class CParticleModule_TargetChannels : public CParticleTargetModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Channels To Color"; }
        FString GetTooltip() const override { return "Pick which channel feeds each color component, then scale and offset the result."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        // An empty name leaves that component as the color before this module.
        PROPERTY(Editable, Category = "Target")
        FName Red;

        PROPERTY(Editable, Category = "Target")
        FName Green;

        PROPERTY(Editable, Category = "Target")
        FName Blue;

        PROPERTY(Editable, Category = "Target")
        FName Alpha;

        PROPERTY(Editable, Category = "Target")
        SParticleParam Scale { FVector4(1.0f, 1.0f, 1.0f, 1.0f) };

        PROPERTY(Editable, Category = "Target")
        SParticleParam Bias { FVector4(0.0f, 0.0f, 0.0f, 0.0f) };
    };

    // Writes a tangent space normal map from a height channel, for water and ground the grid deforms.
    REFLECT()
    class CParticleModule_TargetNormal : public CParticleTargetModule
    {
        GENERATED_BODY()
    public:
        bool IsPaletteVisible() const override { return true; }
        FString GetDisplayName() const override { return "Height To Normal"; }
        FString GetTooltip() const override { return "Encode the slope of a height channel as a normal map in RGB."; }
        void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) override;

        PROPERTY(Editable, Category = "Target")
        FName Height = FName("Height");

        PROPERTY(Editable, Category = "Target")
        SParticleParam Strength { 1.0f };
    };
}
